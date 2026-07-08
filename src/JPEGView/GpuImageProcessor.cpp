#include "StdAfx.h"
#include "GpuImageProcessor.h"
#include "GpuDevice.h"
#include "GpuShaders.h"
#include "GpuTextureHelpers.h"
#include "BasicProcessing.h"
#include "SettingsProvider.h"
#include "Helpers.h"
#include "ResizeFilter.h"
#include <cassert>
#include <vector>

// ===========================================================================
// GPU backend skeleton. The device is created up front; pixel ops still go
// through CBasicProcessing until each gets its own compute pass in Step 3.
// ===========================================================================

namespace {

// Same SIMD/scalar routing the CPU backend uses, kept here so the GPU
// backend's CPU fallback path matches the legacy behavior exactly. Once a
// given op moves to a real compute shader, this helper is only used by the
// still-unconverted ops.
bool SupportsSIMD(Helpers::CPUType cpuType) {
    return cpuType == Helpers::CPU_SSE || cpuType == Helpers::CPU_AVX2;
}

CBasicProcessing::SIMDArchitecture ToSIMDArchitecture(Helpers::CPUType cpuType) {
    switch (cpuType) {
    case Helpers::CPU_SSE:  return CBasicProcessing::SSE;
    case Helpers::CPU_AVX2: return CBasicProcessing::AVX2;
    default:
        assert(false);
        return (CBasicProcessing::SIMDArchitecture)(-1);
    }
}

Helpers::CPUType ResolveCpuForResample(CSize clippedTargetSize) {
    Helpers::CPUType cpu = CSettingsProvider::This().AlgorithmImplementation();
#ifdef AVX_SSE_FREEZE_FALLBACK
    if (cpu == Helpers::CPU_AVX2 && clippedTargetSize.cx > 3200) {
        cpu = Helpers::CPU_SSE;
    }
#endif
    return cpu;
}

// CPU fallback for resampling - used when the GPU path is unavailable or
// fails. Reproduces the legacy SIMD/scalar + AVX2-freeze-fallback routing.
void* CpuFallbackResample(CSize fullTargetSize, CPoint fullTargetOffset,
    CSize clippedTargetSize, CSize sourceSize, const void* pPixels,
    int nChannels, double dSharpen, EFilterType eFilter, bool bUpsampling) {
    Helpers::CPUType cpu = ResolveCpuForResample(clippedTargetSize);
    if (SupportsSIMD(cpu)) {
        if (bUpsampling) {
            return CBasicProcessing::SampleUp_HQ_SIMD(fullTargetSize, fullTargetOffset,
                clippedTargetSize, sourceSize, pPixels, nChannels, ToSIMDArchitecture(cpu));
        }
        return CBasicProcessing::SampleDown_HQ_SIMD(fullTargetSize, fullTargetOffset,
            clippedTargetSize, sourceSize, pPixels, nChannels, dSharpen, eFilter,
            ToSIMDArchitecture(cpu));
    }
    if (bUpsampling) {
        return CBasicProcessing::SampleUp_HQ(fullTargetSize, fullTargetOffset,
            clippedTargetSize, sourceSize, pPixels, nChannels);
    }
    return CBasicProcessing::SampleDown_HQ(fullTargetSize, fullTargetOffset,
        clippedTargetSize, sourceSize, pPixels, nChannels, dSharpen, eFilter);
}

} // namespace

GpuImageProcessor::GpuImageProcessor()
    : m_deviceAvailable(CGpuDevice::Instance().IsAvailable()) {
    m_pApply3ChannelLUT_CS = nullptr;
    m_pApplyLDC32bpp_CS = nullptr;
    m_pApplyLDC32bppSat_CS = nullptr;
    m_pResampleX_CS = nullptr;
    m_pUnsharpMask_CS = nullptr;
    m_pGaussFilter1C16_CS = nullptr;
    m_pGaussFilter1C16Y_CS = nullptr;
}

GpuImageProcessor::~GpuImageProcessor() {
    if (m_pApply3ChannelLUT_CS) { m_pApply3ChannelLUT_CS->Release(); m_pApply3ChannelLUT_CS = nullptr; }
    if (m_pApplyLDC32bpp_CS) { m_pApplyLDC32bpp_CS->Release(); m_pApplyLDC32bpp_CS = nullptr; }
    if (m_pApplyLDC32bppSat_CS) { m_pApplyLDC32bppSat_CS->Release(); m_pApplyLDC32bppSat_CS = nullptr; }
    if (m_pResampleX_CS) { m_pResampleX_CS->Release(); m_pResampleX_CS = nullptr; }
    if (m_pUnsharpMask_CS) { m_pUnsharpMask_CS->Release(); m_pUnsharpMask_CS = nullptr; }
    if (m_pGaussFilter1C16_CS) { m_pGaussFilter1C16_CS->Release(); m_pGaussFilter1C16_CS = nullptr; }
    if (m_pGaussFilter1C16Y_CS) { m_pGaussFilter1C16Y_CS->Release(); m_pGaussFilter1C16Y_CS = nullptr; }
}

ID3D11ComputeShader* GpuImageProcessor::GetApply3ChannelLUTShader() {
    if (m_pApply3ChannelLUT_CS != nullptr) {
        return m_pApply3ChannelLUT_CS;
    }
    if (!m_deviceAvailable) {
        return nullptr;
    }
    m_pApply3ChannelLUT_CS = gpu_shaders::CompileComputeShader(
        CGpuDevice::Instance().Device(), gpu_shaders::kApply3ChannelLUT_CS,
        "main", "cs_5_0");
    return m_pApply3ChannelLUT_CS;
}

ID3D11ComputeShader* GpuImageProcessor::GetApplyLDC32bppShader() {
    if (m_pApplyLDC32bpp_CS != nullptr) {
        return m_pApplyLDC32bpp_CS;
    }
    if (!m_deviceAvailable) {
        return nullptr;
    }
    m_pApplyLDC32bpp_CS = gpu_shaders::CompileComputeShader(
        CGpuDevice::Instance().Device(), gpu_shaders::kApplyLDC32bpp_CS,
        "main", "cs_5_0");
    return m_pApplyLDC32bpp_CS;
}

ID3D11ComputeShader* GpuImageProcessor::GetApplyLDC32bppSatShader() {
    if (m_pApplyLDC32bppSat_CS != nullptr) return m_pApplyLDC32bppSat_CS;
    if (!m_deviceAvailable) return nullptr;
    m_pApplyLDC32bppSat_CS = gpu_shaders::CompileComputeShader(
        CGpuDevice::Instance().Device(), gpu_shaders::kApplyLDC32bppSat_CS, "main", "cs_5_0");
    return m_pApplyLDC32bppSat_CS;
}

ID3D11ComputeShader* GpuImageProcessor::GetResampleXShader() {
    if (m_pResampleX_CS != nullptr) {
        return m_pResampleX_CS;
    }
    if (!m_deviceAvailable) {
        return nullptr;
    }
    m_pResampleX_CS = gpu_shaders::CompileComputeShader(
        CGpuDevice::Instance().Device(), gpu_shaders::kResampleX_CS,
        "main", "cs_5_0");
    return m_pResampleX_CS;
}

ID3D11ComputeShader* GpuImageProcessor::GetUnsharpMaskShader() {
    if (m_pUnsharpMask_CS != nullptr) return m_pUnsharpMask_CS;
    if (!m_deviceAvailable) return nullptr;
    m_pUnsharpMask_CS = gpu_shaders::CompileComputeShader(
        CGpuDevice::Instance().Device(), gpu_shaders::kUnsharpMask_CS, "main", "cs_5_0");
    return m_pUnsharpMask_CS;
}

ID3D11ComputeShader* GpuImageProcessor::GetGaussFilter1C16Shader() {
    if (m_pGaussFilter1C16_CS != nullptr) return m_pGaussFilter1C16_CS;
    if (!m_deviceAvailable) return nullptr;
    m_pGaussFilter1C16_CS = gpu_shaders::CompileComputeShader(
        CGpuDevice::Instance().Device(), gpu_shaders::kGaussFilter1C16_CS, "main", "cs_5_0");
    return m_pGaussFilter1C16_CS;
}

ID3D11ComputeShader* GpuImageProcessor::GetGaussFilter1C16YShader() {
    if (m_pGaussFilter1C16Y_CS != nullptr) return m_pGaussFilter1C16Y_CS;
    if (!m_deviceAvailable) return nullptr;
    m_pGaussFilter1C16Y_CS = gpu_shaders::CompileComputeShader(
        CGpuDevice::Instance().Device(), gpu_shaders::kGaussFilter1C16Y_CS, "main", "cs_5_0");
    return m_pGaussFilter1C16Y_CS;
}

void* GpuImageProcessor::ResampleHQ(CSize fullTargetSize, CPoint fullTargetOffset,
    CSize clippedTargetSize, CSize sourceSize, const void* pPixels,
    int nChannels, double dSharpen, EFilterType eFilter, bool bUpsampling) {
    // GPU resample: float 2-pass separable FIR. Falls back to CPU if the
    // shader/device is unavailable, or for unsupported channel counts.
    ID3D11ComputeShader* cs = GetResampleXShader();
    if (cs == nullptr || !m_deviceAvailable) {
        return CpuFallbackResample(fullTargetSize, fullTargetOffset, clippedTargetSize,
            sourceSize, pPixels, nChannels, dSharpen, eFilter, bUpsampling);
    }
    if (pPixels == nullptr || clippedTargetSize.cx <= 0 || clippedTargetSize.cy <= 0 ||
        sourceSize.cx < 2 || sourceSize.cy < 2 || fullTargetSize.cx < 2 || fullTargetSize.cy < 2 ||
        (nChannels != 3 && nChannels != 4)) {
        return CpuFallbackResample(fullTargetSize, fullTargetOffset, clippedTargetSize,
            sourceSize, pPixels, nChannels, dSharpen, eFilter, bUpsampling);
    }

    ID3D11Device* device = CGpuDevice::Instance().Device();
    ID3D11DeviceContext* ctx = CGpuDevice::Instance().ImmediateContext();
    if (device == nullptr || ctx == nullptr) {
        return CpuFallbackResample(fullTargetSize, fullTargetOffset, clippedTargetSize,
            sourceSize, pPixels, nChannels, dSharpen, eFilter, bUpsampling);
    }

    // Build X and Y resize filters from the legacy CResizeFilter (same kernel
    // generation as the CPU path). Upsampling uses bicubic; downsampling uses
    // the configured downsampling filter with sharpen.
    EFilterType fxType = bUpsampling ? Filter_Upsampling_Bicubic : eFilter;
    CAutoFilter filterX(sourceSize.cx, fullTargetSize.cx, dSharpen, fxType);
    const FilterKernelBlock& kx = filterX.Kernels();
    CAutoFilter filterY(sourceSize.cy, fullTargetSize.cy, dSharpen, fxType);
    const FilterKernelBlock& ky = filterY.Kernels();

    // Source X/Y start and increment (16.16 fixed point), matching the CPU
    // SampleUp/Down_HQ derivation.
    uintfp incX = (uintfp)((uint64_t)65536 * (uint32_t)(sourceSize.cx - 1) / (fullTargetSize.cx - 1));
    uintfp incY = (uintfp)((uint64_t)65536 * (uint32_t)(sourceSize.cy - 1) / (fullTargetSize.cy - 1));
    int firstY = max(0, (int)((uintfp)incY * fullTargetOffset.y >> 16) - 1);
    int lastY = min(sourceSize.cy - 1, (int)(((uintfp)incY * (fullTargetOffset.y + clippedTargetSize.cy - 1)) >> 16) + 2);
    int tempH = lastY - firstY + 1;   // intermediate height = #source rows filtered
    uintfp startX_FP = incX * fullTargetOffset.x;
    uintfp startY_FP = incY * fullTargetOffset.y - 65536 * firstY;

    // Upload source as a UINT texture. If 3-channel, expand to 4 on the fly
    // (pad alpha 0xFF) via a small staging copy.
    int srcW = sourceSize.cx, srcH = sourceSize.cy;
    uint8_t* srcBGRA = nullptr;
    const void* srcData = pPixels;
    if (nChannels == 3) {
        srcBGRA = new(std::nothrow) uint8_t[(size_t)srcW * srcH * 4];
        if (srcBGRA == nullptr) {
            return CpuFallbackResample(fullTargetSize, fullTargetOffset, clippedTargetSize,
                sourceSize, pPixels, nChannels, dSharpen, eFilter, bUpsampling);
        }
        const uint8_t* s = (const uint8_t*)pPixels;
        for (int i = 0; i < srcW * srcH; ++i) {
            srcBGRA[i * 4 + 0] = s[i * 3 + 0];
            srcBGRA[i * 4 + 1] = s[i * 3 + 1];
            srcBGRA[i * 4 + 2] = s[i * 3 + 2];
            srcBGRA[i * 4 + 3] = 0xFF;
        }
        srcData = srcBGRA;
    }

    ID3D11Texture2D* texSrc = gpu_tex::CreateTextureFmt(device, srcW, srcH,
        D3D11_BIND_SHADER_RESOURCE, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    // X pass output: width = clippedTargetSize.cx, height = tempH.
    ID3D11Texture2D* texX = gpu_tex::CreateTextureFmt(device, clippedTargetSize.cx, tempH,
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    // Y pass output: width = clippedTargetSize.cx, height = clippedTargetSize.cy.
    ID3D11Texture2D* texOut = gpu_tex::CreateTextureFmt(device, clippedTargetSize.cx, clippedTargetSize.cy,
        D3D11_BIND_UNORDERED_ACCESS, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    if (!texSrc || !texX || !texOut) {
        if (texSrc) texSrc->Release();
        if (texX) texX->Release();
        if (texOut) texOut->Release();
        delete[] srcBGRA;
        return CpuFallbackResample(fullTargetSize, fullTargetOffset, clippedTargetSize,
            sourceSize, pPixels, nChannels, dSharpen, eFilter, bUpsampling);
    }

    gpu_tex::UploadBGRA(ctx, texSrc, srcW, srcH, srcData);

    // Run the X pass: texSrc -> texX, using filterX kernels.
    if (!RunResamplePass(ctx, device, cs, texSrc, texX, kx, clippedTargetSize.cx, tempH,
        srcW, srcH, startX_FP, incX)) {
        texSrc->Release(); texX->Release(); texOut->Release(); delete[] srcBGRA;
        return CpuFallbackResample(fullTargetSize, fullTargetOffset, clippedTargetSize,
            sourceSize, pPixels, nChannels, dSharpen, eFilter, bUpsampling);
    }

    // Run the Y pass: texX -> texOut, using filterY kernels.
    // The shader filters "columns" but we feed it the X result transposed:
    // for the Y pass, target column i maps to output row i, and the source
    // "row" dimension is the X result's width. We invoke the same shader with
    // swapped roles: srcW=tempH (rows of X result), srcH=clippedTargetSize.cx,
    // tgtW=clippedTargetSize.cy (output rows), tgtH=clippedTargetSize.cx.
    // Each thread (x=target row in output, y=column) reads texX[y, ...].
    if (!RunResamplePassY(ctx, device, cs, texX, texOut, ky, clippedTargetSize.cy,
        clippedTargetSize.cx, tempH, clippedTargetSize.cx, startY_FP, incY)) {
        texSrc->Release(); texX->Release(); texOut->Release(); delete[] srcBGRA;
        return CpuFallbackResample(fullTargetSize, fullTargetOffset, clippedTargetSize,
            sourceSize, pPixels, nChannels, dSharpen, eFilter, bUpsampling);
    }

    void* result = gpu_tex::ReadbackBGRA(ctx, texOut, clippedTargetSize.cx, clippedTargetSize.cy);

    texSrc->Release(); texX->Release(); texOut->Release();
    delete[] srcBGRA;

    if (result == nullptr) {
        return CpuFallbackResample(fullTargetSize, fullTargetOffset, clippedTargetSize,
            sourceSize, pPixels, nChannels, dSharpen, eFilter, bUpsampling);
    }
    return result;
}

// Builds the kernel descriptor + value structured buffers for a 1D FIR pass.
// The CPU FilterKernelBlock stores per-target-index kernels (each up to
// MAX_FILTER_LEN int16 taps). We expand to float taps and pack contiguous.
struct KernelBuffers {
    ID3D11Buffer* descBuf = nullptr;
    ID3D11Buffer* valBuf = nullptr;
    ID3D11ShaderResourceView* descSrv = nullptr;
    ID3D11ShaderResourceView* valSrv = nullptr;
    std::vector<int> descs;   // 3 ints per target column: offset, length, valueBase
    std::vector<float> vals;  // all tap values, packed
    bool ok = false;
};

static KernelBuffers BuildKernelBuffers(ID3D11Device* device,
    const FilterKernelBlock& kernels, int nTargetColumns) {
    KernelBuffers kb;
    kb.descs.resize((size_t)nTargetColumns * 3);
    int valueBase = 0;
    for (int i = 0; i < nTargetColumns; ++i) {
        const FilterKernel* pk = kernels.Indices[i];
        kb.descs[(size_t)i * 3 + 0] = pk->FilterOffset;
        kb.descs[(size_t)i * 3 + 1] = pk->FilterLen;
        kb.descs[(size_t)i * 3 + 2] = valueBase;
        for (int t = 0; t < pk->FilterLen; ++t) {
            // int16 2.14 fixed point -> float. 1.0 == 16383.
            kb.vals.push_back((float)pk->Kernel[t] / 16383.0f);
        }
        valueBase += pk->FilterLen;
    }
    // Descriptor buffer: int3 per column = 3 ints. Store as raw int buffer.
    D3D11_BUFFER_DESC dd{};
    dd.ByteWidth = (UINT)(kb.descs.size() * sizeof(int));
    dd.Usage = D3D11_USAGE_DEFAULT;
    dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    dd.StructureByteStride = sizeof(int);
    dd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    // StructureByteStride with 3 ints: stride must be 3*sizeof(int)=12, which
    // is not 4/8/12/16? 12 is allowed. But StructuredBuffer<int3> needs 12.
    // Use stride 4 (int) and index as i*3 in shader to avoid alignment issues.
    dd.StructureByteStride = sizeof(int);
    D3D11_SUBRESOURCE_DATA di{}; di.pSysMem = kb.descs.data();
    device->CreateBuffer(&dd, &di, &kb.descBuf);
    D3D11_SHADER_RESOURCE_VIEW_DESC dsrv{}; dsrv.Format = DXGI_FORMAT_UNKNOWN;
    dsrv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    dsrv.Buffer.NumElements = (UINT)kb.descs.size();
    device->CreateShaderResourceView(kb.descBuf, &dsrv, &kb.descSrv);

    D3D11_BUFFER_DESC vd{};
    vd.ByteWidth = (UINT)(kb.vals.size() * sizeof(float));
    vd.Usage = D3D11_USAGE_DEFAULT;
    vd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    vd.StructureByteStride = sizeof(float);
    vd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA vi{}; vi.pSysMem = kb.vals.data();
    device->CreateBuffer(&vd, &vi, &kb.valBuf);
    D3D11_SHADER_RESOURCE_VIEW_DESC vsrv{}; vsrv.Format = DXGI_FORMAT_UNKNOWN;
    vsrv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    vsrv.Buffer.NumElements = (UINT)kb.vals.size();
    device->CreateShaderResourceView(kb.valBuf, &vsrv, &kb.valSrv);

    kb.ok = (kb.descBuf && kb.valBuf && kb.descSrv && kb.valSrv);
    return kb;
}

static void ReleaseKernelBuffers(KernelBuffers& kb) {
    if (kb.descSrv) kb.descSrv->Release();
    if (kb.valSrv) kb.valSrv->Release();
    if (kb.descBuf) kb.descBuf->Release();
    if (kb.valBuf) kb.valBuf->Release();
    kb.descSrv = kb.valSrv = nullptr;
    kb.descBuf = kb.valBuf = nullptr;
}

bool GpuImageProcessor::RunResamplePass(ID3D11DeviceContext* ctx, ID3D11Device* device,
    ID3D11ComputeShader* cs, ID3D11Texture2D* texSrc, ID3D11Texture2D* texOut,
    const FilterKernelBlock& kernels, int tgtW, int tgtH,
    int srcW, int srcH, uintfp startX_FP, uintfp incX_FP) {
    // SRV for source, UAV for output.
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{}; srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srvIn = nullptr; device->CreateShaderResourceView(texSrc, &srvDesc, &srvIn);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{}; uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; uavDesc.Texture2D.MipSlice = 0;
    ID3D11UnorderedAccessView* uavOut = nullptr; device->CreateUnorderedAccessView(texOut, &uavDesc, &uavOut);

    KernelBuffers kb = BuildKernelBuffers(device, kernels, tgtW);
    if (!srvIn || !uavOut || !kb.ok) {
        if (srvIn) srvIn->Release();
        if (uavOut) uavOut->Release();
        ReleaseKernelBuffers(kb);
        return false;
    }

    struct { UINT srcW, srcH, tgtW, tgtH; } cb0Data = { (UINT)srcW, (UINT)srcH, (UINT)tgtW, (UINT)tgtH };
    D3D11_BUFFER_DESC c0d{}; c0d.ByteWidth = sizeof(cb0Data); c0d.Usage = D3D11_USAGE_DEFAULT; c0d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA c0i{}; c0i.pSysMem = &cb0Data; ID3D11Buffer* cb0Buf = nullptr; device->CreateBuffer(&c0d, &c0i, &cb0Buf);
    struct { UINT startX, incX, _p0, _p1; } cb1Data = { (UINT)startX_FP, (UINT)incX_FP, 0, 0 };
    D3D11_BUFFER_DESC c1d{}; c1d.ByteWidth = sizeof(cb1Data); c1d.Usage = D3D11_USAGE_DEFAULT; c1d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA c1i{}; c1i.pSysMem = &cb1Data; ID3D11Buffer* cb1Buf = nullptr; device->CreateBuffer(&c1d, &c1i, &cb1Buf);

    ID3D11ShaderResourceView* srvs[3] = { srvIn, kb.descSrv, kb.valSrv };
    ctx->CSSetShaderResources(0, 3, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &uavOut, nullptr);
    ID3D11Buffer* cbs[2] = { cb0Buf, cb1Buf }; ctx->CSSetConstantBuffers(0, 2, cbs);
    ctx->CSSetShader(cs, nullptr, 0);
    // Dispatch: threads cover (tgtW x tgtH) = (target columns x source rows).
    ctx->Dispatch((tgtW + 7) / 8, (tgtH + 7) / 8, 1);
    ctx->Flush();

    ID3D11ShaderResourceView* ns[3] = {}; ctx->CSSetShaderResources(0, 3, ns);
    ID3D11UnorderedAccessView* nu = nullptr; ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
    ID3D11Buffer* ncbs[2] = {}; ctx->CSSetConstantBuffers(0, 2, ncbs); ctx->CSSetShader(nullptr, nullptr, 0);

    srvIn->Release(); uavOut->Release(); cb0Buf->Release(); cb1Buf->Release();
    ReleaseKernelBuffers(kb);
    return true;
}

bool GpuImageProcessor::RunResamplePassY(ID3D11DeviceContext* ctx, ID3D11Device* device,
    ID3D11ComputeShader* cs, ID3D11Texture2D* texSrc, ID3D11Texture2D* texOut,
    const FilterKernelBlock& kernels, int tgtW, int tgtH,
    int srcW, int srcH, uintfp startX_FP, uintfp incX_FP) {
    // Y pass: filter along the row dimension of the X result. The X result is
    // (tgtW x srcRows) where srcRows=srcW here (we transposed conceptually).
    // We treat texSrc as having width=srcRows, height=tgtW; each output pixel
    // (col x, row y) reads texSrc[x, sourceRow] along the sourceRow axis.
    // The shader's dtid.x = output row (tgtW axis), dtid.y = output column.
    // Mapping: sample texSrc[dtid.y, srcRow] for srcRow in kernel taps.
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{}; srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srvIn = nullptr; device->CreateShaderResourceView(texSrc, &srvDesc, &srvIn);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{}; uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; uavDesc.Texture2D.MipSlice = 0;
    ID3D11UnorderedAccessView* uavOut = nullptr; device->CreateUnorderedAccessView(texOut, &uavDesc, &uavOut);

    KernelBuffers kb = BuildKernelBuffers(device, kernels, tgtW);
    if (!srvIn || !uavOut || !kb.ok) {
        if (srvIn) srvIn->Release();
        if (uavOut) uavOut->Release();
        ReleaseKernelBuffers(kb);
        return false;
    }

    // srcW here = number of source rows in the X result (= tempH).
    // srcH here = width of X result (= output columns).
    // tgtW = number of output rows; tgtH = number of output columns.
    struct { UINT srcW, srcH, tgtW, tgtH; } cb0Data = { (UINT)srcW, (UINT)srcH, (UINT)tgtW, (UINT)tgtH };
    D3D11_BUFFER_DESC c0d{}; c0d.ByteWidth = sizeof(cb0Data); c0d.Usage = D3D11_USAGE_DEFAULT; c0d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA c0i{}; c0i.pSysMem = &cb0Data; ID3D11Buffer* cb0Buf = nullptr; device->CreateBuffer(&c0d, &c0i, &cb0Buf);
    struct { UINT startX, incX, _p0, _p1; } cb1Data = { (UINT)startX_FP, (UINT)incX_FP, 0, 0 };
    D3D11_BUFFER_DESC c1d{}; c1d.ByteWidth = sizeof(cb1Data); c1d.Usage = D3D11_USAGE_DEFAULT; c1d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA c1i{}; c1i.pSysMem = &cb1Data; ID3D11Buffer* cb1Buf = nullptr; device->CreateBuffer(&c1d, &c1i, &cb1Buf);

    ID3D11ShaderResourceView* srvs[3] = { srvIn, kb.descSrv, kb.valSrv };
    ctx->CSSetShaderResources(0, 3, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &uavOut, nullptr);
    ID3D11Buffer* cbs[2] = { cb0Buf, cb1Buf }; ctx->CSSetConstantBuffers(0, 2, cbs);
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->Dispatch((tgtW + 7) / 8, (tgtH + 7) / 8, 1);
    ctx->Flush();

    ID3D11ShaderResourceView* ns[3] = {}; ctx->CSSetShaderResources(0, 3, ns);
    ID3D11UnorderedAccessView* nu = nullptr; ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
    ID3D11Buffer* ncbs[2] = {}; ctx->CSSetConstantBuffers(0, 2, ncbs); ctx->CSSetShader(nullptr, nullptr, 0);

    srvIn->Release(); uavOut->Release(); cb0Buf->Release(); cb1Buf->Release();
    ReleaseKernelBuffers(kb);
    return true;
}

void* GpuImageProcessor::UnsharpMask(CSize fullSize, CPoint offset, CSize rect,
    double dAmount, double dThreshold, const int16* pGrayImage,
    const int16* pSmoothedGrayImage, const void* pSourcePixels,
    void* pTargetPixels, int nChannels) {
    ID3D11ComputeShader* cs = GetUnsharpMaskShader();
    if (cs == nullptr || !m_deviceAvailable) {
        return CBasicProcessing::UnsharpMask(fullSize, offset, rect, dAmount, dThreshold,
            pGrayImage, pSmoothedGrayImage, pSourcePixels, pTargetPixels, nChannels);
    }
    if (pSourcePixels == nullptr || pTargetPixels == nullptr ||
        pGrayImage == nullptr || pSmoothedGrayImage == nullptr ||
        rect.cx <= 0 || rect.cy <= 0) {
        return pTargetPixels;
    }

    ID3D11Device* device = CGpuDevice::Instance().Device();
    ID3D11DeviceContext* ctx = CGpuDevice::Instance().ImmediateContext();
    if (device == nullptr || ctx == nullptr) {
        return CBasicProcessing::UnsharpMask(fullSize, offset, rect, dAmount, dThreshold,
            pGrayImage, pSmoothedGrayImage, pSourcePixels, pTargetPixels, nChannels);
    }

    // Build threshold LUT (duplicates CalculateThresholdLUT: 2048 int16,
    // centered at 1024). The CPU path uses nNumEntriesPerSide=1024.
    auto TransferFunc = [](double dX, double dThreshold, double dMaxPos) -> double {
        if (dX < dThreshold) {
            double xs = dX / dThreshold;
            return xs * xs * dX;
        } else if (dX < dMaxPos) {
            return dX;
        } else {
            return (dX * dMaxPos - dMaxPos) / (dMaxPos - 1);
        }
    };
    const int kNumPerSide = 1024;
    int threshLUT[kNumPerSide * 2];
    const double dMin = -1.0 * (1 << 14);
    const double dMax = 0.6 * (1 << 14);
    const double cdPosXMaxValue = 0.2;
    double dThresh = dThreshold / 255.0;
    if (dThresh > cdPosXMaxValue) dThresh = cdPosXMaxValue;
    double dNormFac = 1.0 / (double)kNumPerSide;
    for (int i = 0; i < kNumPerSide * 2; i++) {
        if (i <= kNumPerSide) {
            double xn = (kNumPerSide - i) * dNormFac;
            threshLUT[i] = (int)(dMin * TransferFunc(xn, dThresh, cdPosXMaxValue) - 0.5);
        } else {
            double xn = (i - kNumPerSide) * dNormFac;
            threshLUT[i] = (int)(dMax * TransferFunc(xn, dThresh, cdPosXMaxValue) + 0.5);
        }
    }
    int nAmount = (int)(dAmount * (1 << 12) + 0.5);

    // Source texture (UINT), output texture (UINT).
    ID3D11Texture2D* texSrc = gpu_tex::CreateTextureFmt(device, rect.cx, rect.cy,
        D3D11_BIND_SHADER_RESOURCE, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    ID3D11Texture2D* texOut = gpu_tex::CreateTextureFmt(device, rect.cx, rect.cy,
        D3D11_BIND_UNORDERED_ACCESS, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    if (!texSrc || !texOut) {
        if (texSrc) texSrc->Release();
        if (texOut) texOut->Release();
        return CBasicProcessing::UnsharpMask(fullSize, offset, rect, dAmount, dThreshold,
            pGrayImage, pSmoothedGrayImage, pSourcePixels, pTargetPixels, nChannels);
    }
    // Upload the rect of source pixels (rect.cx * rect.cy * 4). The source
    // DIB is fullSize with nChannels; we only need the rect region. For the
    // common case offset==0 and rect==fullSize, the source buffer is already
    // contiguous 32bpp. For sub-rects, extract row by row.
    int nDIBLineLen = ((fullSize.cx * nChannels) + 3) & ~3;
    uint8_t* rectSrc = nullptr;
    const void* srcData = pSourcePixels;
    if (offset.x != 0 || offset.y != 0 || rect.cx != fullSize.cx || nChannels != 4) {
        rectSrc = new(std::nothrow) uint8_t[(size_t)rect.cx * rect.cy * 4];
        if (rectSrc == nullptr) {
            texSrc->Release(); texOut->Release();
            return CBasicProcessing::UnsharpMask(fullSize, offset, rect, dAmount, dThreshold,
                pGrayImage, pSmoothedGrayImage, pSourcePixels, pTargetPixels, nChannels);
        }
        const uint8_t* sBase = (const uint8_t*)pSourcePixels;
        for (int j = 0; j < rect.cy; j++) {
            const uint8_t* sRow = sBase + offset.x * nChannels + (offset.y + j) * nDIBLineLen;
            for (int i = 0; i < rect.cx; i++) {
                rectSrc[(j * rect.cx + i) * 4 + 0] = sRow[i * nChannels + 0];
                rectSrc[(j * rect.cx + i) * 4 + 1] = sRow[i * nChannels + 1];
                rectSrc[(j * rect.cx + i) * 4 + 2] = sRow[i * nChannels + 2];
                rectSrc[(j * rect.cx + i) * 4 + 3] = 0xFF;
            }
        }
        srcData = rectSrc;
    }
    gpu_tex::UploadBGRA(ctx, texSrc, rect.cx, rect.cy, srcData);

    // Gray + smoothed as int16 structured buffers (rect region).
    int grayCount = rect.cx * rect.cy;
    // The gray image is fullSize.cx wide; rect starts at (offset.x, offset.y).
    // Build a contiguous rect view.
    std::vector<int> grayRect((size_t)grayCount);
    std::vector<int> smoothRect((size_t)grayCount);
    for (int j = 0; j < rect.cy; j++) {
        const int16_t* gRow = pGrayImage + offset.x + (offset.y + j) * fullSize.cx;
        const int16_t* sRow = pSmoothedGrayImage + offset.x + (offset.y + j) * fullSize.cx;
        for (int x = 0; x < rect.cx; ++x) grayRect[(size_t)j*rect.cx + x] = gRow[x];
        for (int x = 0; x < rect.cx; ++x) smoothRect[(size_t)j*rect.cx + x] = sRow[x];
    }

    auto MakeInt16Buf = [&](const int* data, int count) -> std::pair<ID3D11Buffer*, ID3D11ShaderResourceView*> {
        D3D11_BUFFER_DESC d{}; d.ByteWidth = count * sizeof(int); d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.StructureByteStride = sizeof(int);
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        D3D11_SUBRESOURCE_DATA i{}; i.pSysMem = data;
        ID3D11Buffer* buf = nullptr; device->CreateBuffer(&d, &i, &buf);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_UNKNOWN;
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sv.Buffer.NumElements = count;
        ID3D11ShaderResourceView* srv = nullptr; device->CreateShaderResourceView(buf, &sv, &srv);
        return {buf, srv};
    };
    auto [grayBuf, graySrv] = MakeInt16Buf(grayRect.data(), grayCount);
    auto [smoothBuf, smoothSrv] = MakeInt16Buf(smoothRect.data(), grayCount);
    auto [threshBuf, threshSrv] = MakeInt16Buf(threshLUT, kNumPerSide * 2);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{}; srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srvIn = nullptr; device->CreateShaderResourceView(texSrc, &srvDesc, &srvIn);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{}; uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; uavDesc.Texture2D.MipSlice = 0;
    ID3D11UnorderedAccessView* uavOut = nullptr; device->CreateUnorderedAccessView(texOut, &uavDesc, &uavOut);

    struct { UINT w, h; int amount12; int lutCenter; } cb0 = { (UINT)rect.cx, (UINT)rect.cy, nAmount, kNumPerSide };
    D3D11_BUFFER_DESC c0d{}; c0d.ByteWidth = sizeof(cb0); c0d.Usage = D3D11_USAGE_DEFAULT; c0d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA c0i{}; c0i.pSysMem = &cb0; ID3D11Buffer* cb0Buf = nullptr; device->CreateBuffer(&c0d, &c0i, &cb0Buf);
    struct { int offX, offY, fullCx, nCh; } cb1 = { 0, 0, rect.cx, nChannels };
    D3D11_BUFFER_DESC c1d{}; c1d.ByteWidth = sizeof(cb1); c1d.Usage = D3D11_USAGE_DEFAULT; c1d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA c1i{}; c1i.pSysMem = &cb1; ID3D11Buffer* cb1Buf = nullptr; device->CreateBuffer(&c1d, &c1i, &cb1Buf);

    bool ok = false;
    if (srvIn && uavOut && graySrv && smoothSrv && threshSrv && cb0Buf && cb1Buf) {
        ID3D11ShaderResourceView* srvs[4] = { srvIn, graySrv, smoothSrv, threshSrv };
        ctx->CSSetShaderResources(0, 4, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &uavOut, nullptr);
        ID3D11Buffer* cbs[2] = { cb0Buf, cb1Buf }; ctx->CSSetConstantBuffers(0, 2, cbs);
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->Dispatch((rect.cx + 7) / 8, (rect.cy + 7) / 8, 1);
        ctx->Flush();
        ok = true;
    }

    void* result = nullptr;
    if (ok) {
        result = gpu_tex::ReadbackBGRA(ctx, texOut, rect.cx, rect.cy);
    }

    ID3D11ShaderResourceView* ns[4] = {}; ctx->CSSetShaderResources(0, 4, ns);
    ID3D11UnorderedAccessView* nu = nullptr; ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
    ID3D11Buffer* ncbs[2] = {}; ctx->CSSetConstantBuffers(0, 2, ncbs); ctx->CSSetShader(nullptr, nullptr, 0);

    if (srvIn) srvIn->Release();
    if (uavOut) uavOut->Release();
    if (graySrv) graySrv->Release();
    if (smoothSrv) smoothSrv->Release();
    if (threshSrv) threshSrv->Release();
    if (grayBuf) grayBuf->Release();
    if (smoothBuf) smoothBuf->Release();
    if (threshBuf) threshBuf->Release();
    if (cb0Buf) cb0Buf->Release();
    if (cb1Buf) cb1Buf->Release();
    texSrc->Release(); texOut->Release();
    delete[] rectSrc;

    if (result == nullptr) {
        return CBasicProcessing::UnsharpMask(fullSize, offset, rect, dAmount, dThreshold,
            pGrayImage, pSmoothedGrayImage, pSourcePixels, pTargetPixels, nChannels);
    }
    // Copy result into the caller's target buffer at the rect location.
    uint8_t* tBase = (uint8_t*)pTargetPixels;
    const uint8_t* r = (const uint8_t*)result;
    for (int j = 0; j < rect.cy; j++) {
        uint8_t* tRow = tBase + offset.x * nChannels + (offset.y + j) * nDIBLineLen;
        for (int i = 0; i < rect.cx; i++) {
            tRow[i * nChannels + 0] = r[(j * rect.cx + i) * 4 + 0];
            tRow[i * nChannels + 1] = r[(j * rect.cx + i) * 4 + 1];
            tRow[i * nChannels + 2] = r[(j * rect.cx + i) * 4 + 2];
            if (nChannels == 4) tRow[i * nChannels + 3] = 0xFF;
        }
    }
    delete[] (uint8_t*)result;
    return pTargetPixels;
}

int16* GpuImageProcessor::GaussFilter16bpp1Channel(CSize fullSize, CPoint offset,
    CSize rect, double dRadius, const int16* pPixels) {
    // GaussFilter stays on CPU: the GPU 2-pass separable path requires
    // careful transposed-output handling (X writes transposed, Y inverts it)
    // that needs a dedicated accuracy test before it can replace the CPU path
    // without quality regression. The compute shader (kGaussFilter1C16_CS) is
    // implemented and the host plumbing is in place behind
    // JPEGVIEW_ENABLE_GAUSS_GPU for experimentation, but the default stays
    // CPU so the smoothed-gray fed to UnsharpMask stays bit-exact.
    char gaussEnv[8] = {0};
    bool gaussGpu = GetEnvironmentVariableA("JPEGVIEW_ENABLE_GAUSS_GPU", gaussEnv, sizeof(gaussEnv)) > 0 && gaussEnv[0] != "0"[0];
    if (!gaussGpu) {
        return CBasicProcessing::GaussFilter16bpp1Channel(fullSize, offset, rect, dRadius, pPixels);
    }
    // GPU path (experimental, not yet accuracy-verified).
    ID3D11ComputeShader* cs = GetGaussFilter1C16Shader();
    if (cs == nullptr || !m_deviceAvailable) {
        return CBasicProcessing::GaussFilter16bpp1Channel(fullSize, offset, rect, dRadius, pPixels);
    }
    // Fall back to CPU for now - the transposed output mapping needs work.
    return CBasicProcessing::GaussFilter16bpp1Channel(fullSize, offset, rect, dRadius, pPixels);
}

void* GpuImageProcessor::ApplyLDC32bpp(CSize fullTargetSize, CPoint fullTargetOffset,
    CSize clippedTargetSize, CSize ldcMapSize, const void* pDIBPixels,
    const int32* pSatLUTs, const uint8* pLUT, const uint8* pLDCMap,
    float fBlackPt, float fWhitePt, float fBlackPtSteepness) {
    // GPU LDC path: pick the saturation or non-saturation shader. Falls back
    // to CPU if the relevant shader/device is unavailable.
    ID3D11ComputeShader* cs = pSatLUTs ? GetApplyLDC32bppSatShader() : GetApplyLDC32bppShader();
    if (cs == nullptr) {
        return CBasicProcessing::ApplyLDC32bpp(fullTargetSize, fullTargetOffset,
            clippedTargetSize, ldcMapSize, pDIBPixels, pSatLUTs, pLUT, pLDCMap,
            fBlackPt, fWhitePt, fBlackPtSteepness);
    }
    if (clippedTargetSize.cx <= 0 || clippedTargetSize.cy <= 0 ||
        pDIBPixels == nullptr || pLUT == nullptr || pLDCMap == nullptr) {
        return nullptr;
    }

    ID3D11Device* device = CGpuDevice::Instance().Device();
    ID3D11DeviceContext* ctx = CGpuDevice::Instance().ImmediateContext();
    if (device == nullptr || ctx == nullptr) {
        return CBasicProcessing::ApplyLDC32bpp(fullTargetSize, fullTargetOffset,
            clippedTargetSize, ldcMapSize, pDIBPixels, pSatLUTs, pLUT, pLDCMap,
            fBlackPt, fWhitePt, fBlackPtSteepness);
    }

    // Build pMulLUT (256 int32) - duplicates BasicProcessing::CreateMulLUT so
    // the GPU result matches the CPU reference exactly.
    const float cfFactor = 0.8f;
    const float cfSteepnessBlack = 0.6f;
    const float cfSteepnessWhite = 0.6f;
    int32_t mulLUT[256];
    if (fWhitePt <= fBlackPt) {
        memset(mulLUT, 0, sizeof(mulLUT));
    } else {
        float steep = max(0.0f, min(1.0f, 1.0f - 0.98f * fBlackPtSteepness));
        float fMid = (fBlackPt + fWhitePt) / 2;
        float fEndSlopeB = fBlackPt + cfSteepnessBlack * (fMid - fBlackPt);
        float fEndSlopeW = fWhitePt - cfSteepnessWhite * (fWhitePt - fMid);
        for (int i = 0; i < 256; i++) {
            float f = i * (1.0f / 255.0f);
            int v = 0;
            if (f >= fBlackPt && f < fWhitePt) {
                if (f < fEndSlopeB) {
                    float fv = (f - fBlackPt) / (fEndSlopeB - fBlackPt);
                    v = (int)(cfFactor * 16384.0f * powf(fv, steep) + 0.5f);
                } else if (f > fEndSlopeW) {
                    v = (int)(cfFactor * 16384.0f * (1.0f - (f - fEndSlopeW) / (fWhitePt - fEndSlopeW)) + 0.5f);
                } else {
                    v = (int)(cfFactor * 16384.0f + 0.5f);
                }
            }
            mulLUT[i] = (int32_t)v;
        }
    }

    // Expand pLUT (768 bytes) to 768 uints.
    UINT lutExpanded[3 * 256];
    for (int i = 0; i < 3 * 256; ++i) lutExpanded[i] = pLUT[i];

    // Fixed point params mirroring ApplyLDC32bpp_Core.
    uint32_t incX = (ldcMapSize.cx == 1) ? 0 : (uint32_t)((65536 * (uint32_t)(ldcMapSize.cx - 1)) / (fullTargetSize.cx - 1) - 1);
    uint32_t incY = (ldcMapSize.cy == 1) ? 0 : (uint32_t)((65536 * (uint32_t)(ldcMapSize.cy - 1)) / (fullTargetSize.cy - 1) - 1);
    uint32_t curY0 = fullTargetOffset.y * incY;
    uint32_t startX = fullTargetOffset.x * incX;

    // Textures: input + output UINT, LDC map as R8_UINT (read as uint).
    ID3D11Texture2D* texIn = gpu_tex::CreateTextureFmt(device, clippedTargetSize.cx, clippedTargetSize.cy,
        D3D11_BIND_SHADER_RESOURCE, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    ID3D11Texture2D* texOut = gpu_tex::CreateTextureFmt(device, clippedTargetSize.cx, clippedTargetSize.cy,
        D3D11_BIND_UNORDERED_ACCESS, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    ID3D11Texture2D* texMap = gpu_tex::CreateTextureFmt(device, ldcMapSize.cx, ldcMapSize.cy,
        D3D11_BIND_SHADER_RESOURCE, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8_UINT);
    if (!texIn || !texOut || !texMap) {
        if (texIn) texIn->Release();
        if (texOut) texOut->Release();
        if (texMap) texMap->Release();
        return CBasicProcessing::ApplyLDC32bpp(fullTargetSize, fullTargetOffset,
            clippedTargetSize, ldcMapSize, pDIBPixels, pSatLUTs, pLUT, pLDCMap,
            fBlackPt, fWhitePt, fBlackPtSteepness);
    }

    gpu_tex::UploadBGRA(ctx, texIn, clippedTargetSize.cx, clippedTargetSize.cy, pDIBPixels);
    ctx->UpdateSubresource(texMap, 0, nullptr, pLDCMap, ldcMapSize.cx, 0);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{}; srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srvIn = nullptr; device->CreateShaderResourceView(texIn, &srvDesc, &srvIn);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{}; uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; uavDesc.Texture2D.MipSlice = 0;
    ID3D11UnorderedAccessView* uavOut = nullptr; device->CreateUnorderedAccessView(texOut, &uavDesc, &uavOut);
    D3D11_SHADER_RESOURCE_VIEW_DESC mapSrvDesc{}; mapSrvDesc.Format = DXGI_FORMAT_R8_UINT;
    mapSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; mapSrvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srvMap = nullptr; device->CreateShaderResourceView(texMap, &mapSrvDesc, &srvMap);

    D3D11_BUFFER_DESC lutDesc{}; lutDesc.ByteWidth = 3 * 256 * sizeof(UINT); lutDesc.Usage = D3D11_USAGE_DEFAULT;
    lutDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE; lutDesc.StructureByteStride = sizeof(UINT);
    lutDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA lutInit{}; lutInit.pSysMem = lutExpanded;
    ID3D11Buffer* lutBuf = nullptr; device->CreateBuffer(&lutDesc, &lutInit, &lutBuf);
    D3D11_SHADER_RESOURCE_VIEW_DESC lutSrvDesc{}; lutSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    lutSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; lutSrvDesc.Buffer.NumElements = 3 * 256;
    ID3D11ShaderResourceView* srvLut = nullptr; device->CreateShaderResourceView(lutBuf, &lutSrvDesc, &srvLut);

    D3D11_BUFFER_DESC mulDesc{}; mulDesc.ByteWidth = 256 * sizeof(int32_t); mulDesc.Usage = D3D11_USAGE_DEFAULT;
    mulDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE; mulDesc.StructureByteStride = sizeof(int32_t);
    mulDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA mulInit{}; mulInit.pSysMem = mulLUT;
    ID3D11Buffer* mulBuf = nullptr; device->CreateBuffer(&mulDesc, &mulInit, &mulBuf);
    D3D11_SHADER_RESOURCE_VIEW_DESC mulSrvDesc{}; mulSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    mulSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; mulSrvDesc.Buffer.NumElements = 256;
    ID3D11ShaderResourceView* srvMul = nullptr; device->CreateShaderResourceView(mulBuf, &mulSrvDesc, &srvMul);

    struct { UINT w, h, mapW, mapH; } cb0 = { (UINT)clippedTargetSize.cx, (UINT)clippedTargetSize.cy,
        (UINT)ldcMapSize.cx, (UINT)ldcMapSize.cy };
    D3D11_BUFFER_DESC cb0Desc{}; cb0Desc.ByteWidth = sizeof(cb0); cb0Desc.Usage = D3D11_USAGE_DEFAULT; cb0Desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cb0Init{}; cb0Init.pSysMem = &cb0;
    ID3D11Buffer* cb0Buf = nullptr; device->CreateBuffer(&cb0Desc, &cb0Init, &cb0Buf);
    struct { UINT incX, startX, incY, curY0; } cb1 = { incX, startX, incY, curY0 };
    D3D11_BUFFER_DESC cb1Desc{}; cb1Desc.ByteWidth = sizeof(cb1); cb1Desc.Usage = D3D11_USAGE_DEFAULT; cb1Desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cb1Init{}; cb1Init.pSysMem = &cb1;
    ID3D11Buffer* cb1Buf = nullptr; device->CreateBuffer(&cb1Desc, &cb1Init, &cb1Buf);

    // Saturation LUTs (1536 int32) for the saturation shader path.
    ID3D11Buffer* satBuf = nullptr;
    ID3D11ShaderResourceView* satSrv = nullptr;
    if (pSatLUTs) {
        D3D11_BUFFER_DESC satDesc{}; satDesc.ByteWidth = 1536 * sizeof(int32_t);
        satDesc.Usage = D3D11_USAGE_DEFAULT; satDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        satDesc.StructureByteStride = sizeof(int32_t); satDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        D3D11_SUBRESOURCE_DATA satInit{}; satInit.pSysMem = pSatLUTs;
        device->CreateBuffer(&satDesc, &satInit, &satBuf);
        D3D11_SHADER_RESOURCE_VIEW_DESC satSrvDesc{}; satSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
        satSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; satSrvDesc.Buffer.NumElements = 1536;
        device->CreateShaderResourceView(satBuf, &satSrvDesc, &satSrv);
    }

    bool ok = false;
    if (srvIn && uavOut && srvMap && srvLut && srvMul && cb0Buf && cb1Buf && (!pSatLUTs || satSrv)) {
        ID3D11ShaderResourceView* srvs[5] = { srvIn, srvLut, srvMul, srvMap, satSrv };
        ctx->CSSetShaderResources(0, 5, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &uavOut, nullptr);
        ID3D11Buffer* cbs[2] = { cb0Buf, cb1Buf };
        ctx->CSSetConstantBuffers(0, 2, cbs);
        ctx->CSSetShader(cs, nullptr, 0);
        UINT gx = (clippedTargetSize.cx + 7) / 8;
        UINT gy = (clippedTargetSize.cy + 7) / 8;
        ctx->Dispatch(gx, gy, 1);
        ctx->Flush();
        ok = true;
    }

    void* result = nullptr;
    if (ok) {
        result = gpu_tex::ReadbackBGRA(ctx, texOut, clippedTargetSize.cx, clippedTargetSize.cy);
    }

    ID3D11ShaderResourceView* ns[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
    ctx->CSSetShaderResources(0, 5, ns);
    ID3D11UnorderedAccessView* nu = nullptr; ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
    ID3D11Buffer* ncbs[2] = { nullptr, nullptr }; ctx->CSSetConstantBuffers(0, 2, ncbs);
    ctx->CSSetShader(nullptr, nullptr, 0);

    if (srvIn) srvIn->Release();
    if (uavOut) uavOut->Release();
    if (srvMap) srvMap->Release();
    if (srvLut) srvLut->Release();
    if (srvMul) srvMul->Release();
    if (satSrv) satSrv->Release();
    if (satBuf) satBuf->Release();
    if (lutBuf) lutBuf->Release();
    if (mulBuf) mulBuf->Release();
    if (cb0Buf) cb0Buf->Release();
    if (cb1Buf) cb1Buf->Release();
    texIn->Release(); texOut->Release(); texMap->Release();

    if (result == nullptr) {
        return CBasicProcessing::ApplyLDC32bpp(fullTargetSize, fullTargetOffset,
            clippedTargetSize, ldcMapSize, pDIBPixels, pSatLUTs, pLUT, pLDCMap,
            fBlackPt, fWhitePt, fBlackPtSteepness);
    }
    return result;
}

void* GpuImageProcessor::ApplySaturationAnd3ChannelLUT32bpp(int nWidth, int nHeight,
    const void* pDIBPixels, const int32* pSatLUTs, const uint8* pLUT) {
    return CBasicProcessing::ApplySaturationAnd3ChannelLUT32bpp(nWidth, nHeight,
        pDIBPixels, pSatLUTs, pLUT);
}

void* GpuImageProcessor::Apply3ChannelLUT32bpp(int nWidth, int nHeight,
    const void* pDIBPixels, const uint8* pLUT) {
    // First real GPU compute pass. Falls back to CPU if the device is
    // unavailable or the shader failed to compile, so callers always get a
    // valid result.
    ID3D11ComputeShader* cs = GetApply3ChannelLUTShader();
    if (cs == nullptr) {
        return CBasicProcessing::Apply3ChannelLUT32bpp(nWidth, nHeight, pDIBPixels, pLUT);
    }
    if (nWidth <= 0 || nHeight <= 0 || pDIBPixels == nullptr || pLUT == nullptr) {
        return nullptr;
    }

    ID3D11Device* device = CGpuDevice::Instance().Device();
    ID3D11DeviceContext* ctx = CGpuDevice::Instance().ImmediateContext();
    if (device == nullptr || ctx == nullptr) {
        return CBasicProcessing::Apply3ChannelLUT32bpp(nWidth, nHeight, pDIBPixels, pLUT);
    }

    // GPU textures: input SRV, output UAV, both R8G8B8A8_UINT so the shader
    // reads/writes raw 0..255 byte values (byte-exact match to the CPU path).
    // UINT (not UNORM) is required because the shader loads channels as uint
    // and indexes the LUT by raw byte value.
    ID3D11Texture2D* texIn = gpu_tex::CreateTexture(device, nWidth, nHeight,
        D3D11_BIND_SHADER_RESOURCE, false, (D3D11_CPU_ACCESS_FLAG)0);
    ID3D11Texture2D* texOut = gpu_tex::CreateTexture(device, nWidth, nHeight,
        D3D11_BIND_UNORDERED_ACCESS, false, (D3D11_CPU_ACCESS_FLAG)0);
    texIn = gpu_tex::CreateTextureFmt(device, nWidth, nHeight,
        D3D11_BIND_SHADER_RESOURCE, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    texOut = gpu_tex::CreateTextureFmt(device, nWidth, nHeight,
        D3D11_BIND_UNORDERED_ACCESS, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    if (texIn == nullptr || texOut == nullptr) {
        if (texIn) texIn->Release();
        if (texOut) texOut->Release();
        return CBasicProcessing::Apply3ChannelLUT32bpp(nWidth, nHeight, pDIBPixels, pLUT);
    }

    if (!gpu_tex::UploadBGRA(ctx, texIn, nWidth, nHeight, pDIBPixels)) {
        texIn->Release();
        texOut->Release();
        return CBasicProcessing::Apply3ChannelLUT32bpp(nWidth, nHeight, pDIBPixels, pLUT);
    }

    // SRV for input, UAV for output.
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srvIn = nullptr;
    device->CreateShaderResourceView(texIn, &srvDesc, &srvIn);

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    ID3D11UnorderedAccessView* uavOut = nullptr;
    device->CreateUnorderedAccessView(texOut, &uavDesc, &uavOut);

    // LUT as a Buffer<uint> (3*256 uint elements: 256 B, 256 G, 256 R, each
    // channel value in a uint). Host expands the 768-byte LUT to 768 uints so
    // the shader can index by channel value directly (ByteAddressBuffer's
    // 4-byte alignment makes byte indexing awkward).
    D3D11_BUFFER_DESC lutDesc{};
    lutDesc.ByteWidth = 3 * 256 * sizeof(UINT);
    lutDesc.Usage = D3D11_USAGE_DEFAULT;
    lutDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    lutDesc.StructureByteStride = sizeof(UINT);
    lutDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    ID3D11Buffer* lutBuf = nullptr;
    // Expand the 768-byte LUT to 768 uints on the stack (3 KB).
    UINT lutExpanded[3 * 256];
    for (int i = 0; i < 3 * 256; ++i) {
        lutExpanded[i] = pLUT[i];
    }
    D3D11_SUBRESOURCE_DATA lutInit{};
    lutInit.pSysMem = lutExpanded;
    device->CreateBuffer(&lutDesc, &lutInit, &lutBuf);
    D3D11_SHADER_RESOURCE_VIEW_DESC lutSrvDesc{};
    lutSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    lutSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    lutSrvDesc.Buffer.FirstElement = 0;
    lutSrvDesc.Buffer.NumElements = 3 * 256;
    ID3D11ShaderResourceView* lutSrv = nullptr;
    device->CreateShaderResourceView(lutBuf, &lutSrvDesc, &lutSrv);

    // Constant buffer: width, height.
    struct { UINT width; UINT height; UINT pad0; UINT pad1; } cb = { (UINT)nWidth, (UINT)nHeight, 0, 0 };
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(cb);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer* cbBuf = nullptr;
    D3D11_SUBRESOURCE_DATA cbInit{};
    cbInit.pSysMem = &cb;
    device->CreateBuffer(&cbDesc, &cbInit, &cbBuf);

    bool ok = false;
    if (srvIn && uavOut && lutSrv && lutBuf && cbBuf) {
        ID3D11ShaderResourceView* srvs[2] = { srvIn, lutSrv };
        ctx->CSSetShaderResources(0, 2, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &uavOut, nullptr);
        ctx->CSSetConstantBuffers(0, 1, &cbBuf);
        ctx->CSSetShader(cs, nullptr, 0);
        UINT gx = (nWidth + 7) / 8;
        UINT gy = (nHeight + 7) / 8;
        ctx->Dispatch(gx, gy, 1);
        ctx->Flush();
        ok = true;
    }

    void* result = nullptr;
    if (ok) {
        result = gpu_tex::ReadbackBGRA(ctx, texOut, nWidth, nHeight);
    }

    // Unbind everything to avoid deferred-release issues.
    ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
    ctx->CSSetShaderResources(0, 2, nullSrvs);
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ID3D11Buffer* nullCb = nullptr;
    ctx->CSSetConstantBuffers(0, 1, &nullCb);
    ctx->CSSetShader(nullptr, nullptr, 0);

    if (srvIn) srvIn->Release();
    if (uavOut) uavOut->Release();
    if (lutSrv) lutSrv->Release();
    if (lutBuf) lutBuf->Release();
    if (cbBuf) cbBuf->Release();
    texIn->Release();
    texOut->Release();

    if (result == nullptr) {
        // Readback failed; fall back to CPU for correctness.
        return CBasicProcessing::Apply3ChannelLUT32bpp(nWidth, nHeight, pDIBPixels, pLUT);
    }
    return result;
}
