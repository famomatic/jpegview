// Standalone accuracy test for the ResampleX GPU compute pass. Builds a
// fixed kernel (box-ish [0.25, 0.5, 0.25]) and a small source image, runs the
// X pass, and compares against a CPU accumulation using the same kernel.
// Validates that the shader reads kernel descriptors/values and accumulates
// correctly. Full bicubic parity with SampleUp_HQ is checked separately via
// PSNR in Step 5; here we verify the kernel buffer plumbing.
#define NOMINMAX
#include "GpuDevice.h"
#include "GpuShaders.h"
#include "GpuTextureHelpers.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>

// CPU reference for the X pass with an explicit kernel set (one kernel per
// target column). Each kernel: offset, length, float weights. The source X
// for target column i is (startX_FP + i*incX_FP) >> 16, tap start = xInt - offset.
static void CpuRefX(const uint8_t* srcBGRA, uint8_t* outBGRA,
    int tgtW, int tgtH, int srcW,
    const int* offsets, const int* lengths, const float* vals, const int* valBases,
    uint32_t startX_FP, uint32_t incX_FP) {
    for (int y = 0; y < tgtH; ++y) {
        for (int i = 0; i < tgtW; ++i) {
            uint32_t xFP = startX_FP + (uint32_t)i * incX_FP;
            int xInt = (int)(xFP >> 16);
            int off = offsets[i], len = lengths[i], vb = valBases[i];
            int tapStart = xInt - off;
            float sumB = 0, sumG = 0, sumR = 0;
            for (int n = 0; n < len; ++n) {
                float w = vals[vb + n];
                int sx = std::max(0, std::min(srcW - 1, tapStart + n));
                const uint8_t* px = srcBGRA + ((size_t)y * srcW + sx) * 4;
                sumB += w * px[0];
                sumG += w * px[1];
                sumR += w * px[2];
            }
            outBGRA[(y * tgtW + i) * 4 + 0] = (uint8_t)std::max(0, std::min(255, (int)std::round(sumB)));
            outBGRA[(y * tgtW + i) * 4 + 1] = (uint8_t)std::max(0, std::min(255, (int)std::round(sumG)));
            outBGRA[(y * tgtW + i) * 4 + 2] = (uint8_t)std::max(0, std::min(255, (int)std::round(sumR)));
            outBGRA[(y * tgtW + i) * 4 + 3] = 0xFF;
        }
    }
}

int main() {
    const int srcW = 8, srcH = 4, tgtW = 4, tgtH = srcH;
    uint8_t* src = new uint8_t[srcW * srcH * 4];
    for (int i = 0; i < srcW * srcH; ++i) {
        src[i * 4 + 0] = (uint8_t)((i * 7) & 255);
        src[i * 4 + 1] = (uint8_t)((i * 13) & 255);
        src[i * 4 + 2] = (uint8_t)((i * 3) & 255);
        src[i * 4 + 3] = 0xFF;
    }

    // One kernel per target column: [0.25, 0.5, 0.25], offset 1, length 3.
    int offsets[4] = { 1, 1, 1, 1 };
    int lengths[4] = { 3, 3, 3, 3 };
    float allVals[12] = { 0.25f,0.5f,0.25f, 0.25f,0.5f,0.25f, 0.25f,0.5f,0.25f, 0.25f,0.5f,0.25f };
    int valBases[4] = { 0, 3, 6, 9 };

    uint8_t* ref = new uint8_t[tgtW * tgtH * 4];
    // startX_FP: target col 0 samples source col 0, increment = srcW-1 / tgtW * 65536.
    uint32_t incX = (uint32_t)(65536ULL * (srcW - 1) / (tgtW));
    uint32_t startX = 0;
    CpuRefX(src, ref, tgtW, tgtH, srcW, offsets, lengths, allVals, valBases, startX, incX);

    CGpuDevice& dev = CGpuDevice::Instance();
    if (!dev.IsAvailable()) { wprintf(L"SKIP: no GPU\n"); return 77; }
    ID3D11Device* device = dev.Device();
    ID3D11DeviceContext* ctx = dev.ImmediateContext();
    ID3D11ComputeShader* cs = gpu_shaders::CompileComputeShader(device, gpu_shaders::kResampleX_CS, "main", "cs_5_0");
    if (!cs) { wprintf(L"FAIL: shader compile\n"); return 3; }

    ID3D11Texture2D* texSrc = gpu_tex::CreateTextureFmt(device, srcW, srcH, D3D11_BIND_SHADER_RESOURCE, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    ID3D11Texture2D* texOut = gpu_tex::CreateTextureFmt(device, tgtW, tgtH, D3D11_BIND_UNORDERED_ACCESS, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    gpu_tex::UploadBGRA(ctx, texSrc, srcW, srcH, src);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{}; srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT; srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srvIn = nullptr; device->CreateShaderResourceView(texSrc, &srvDesc, &srvIn);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{}; uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT; uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; uavDesc.Texture2D.MipSlice = 0;
    ID3D11UnorderedAccessView* uavOut = nullptr; device->CreateUnorderedAccessView(texOut, &uavDesc, &uavOut);

    // Kernel descriptor buffer: 3 ints per column (offset, length, valueBase).
    int descs[12] = { offsets[0],lengths[0],valBases[0], offsets[1],lengths[1],valBases[1], offsets[2],lengths[2],valBases[2], offsets[3],lengths[3],valBases[3] };
    D3D11_BUFFER_DESC dd{}; dd.ByteWidth = sizeof(descs); dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_SHADER_RESOURCE; dd.StructureByteStride = sizeof(int); dd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA di{}; di.pSysMem = descs; ID3D11Buffer* descBuf = nullptr; device->CreateBuffer(&dd, &di, &descBuf);
    D3D11_SHADER_RESOURCE_VIEW_DESC dsrv{}; dsrv.Format = DXGI_FORMAT_UNKNOWN; dsrv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; dsrv.Buffer.NumElements = 12;
    ID3D11ShaderResourceView* srvDesc2 = nullptr; device->CreateShaderResourceView(descBuf, &dsrv, &srvDesc2);

    D3D11_BUFFER_DESC vd{}; vd.ByteWidth = sizeof(allVals); vd.Usage = D3D11_USAGE_DEFAULT; vd.BindFlags = D3D11_BIND_SHADER_RESOURCE; vd.StructureByteStride = sizeof(float); vd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA vi{}; vi.pSysMem = allVals; ID3D11Buffer* valBuf = nullptr; device->CreateBuffer(&vd, &vi, &valBuf);
    D3D11_SHADER_RESOURCE_VIEW_DESC vsrv{}; vsrv.Format = DXGI_FORMAT_UNKNOWN; vsrv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; vsrv.Buffer.NumElements = 12;
    ID3D11ShaderResourceView* srvVal = nullptr; device->CreateShaderResourceView(valBuf, &vsrv, &srvVal);

    struct { UINT sw, sh, tw, th; } cb0 = { (UINT)srcW,(UINT)srcH,(UINT)tgtW,(UINT)tgtH };
    D3D11_BUFFER_DESC c0d{}; c0d.ByteWidth = sizeof(cb0); c0d.Usage = D3D11_USAGE_DEFAULT; c0d.BindFlags = D3D11_BIND_CONSTANT_BUFFER; D3D11_SUBRESOURCE_DATA c0i{}; c0i.pSysMem = &cb0; ID3D11Buffer* cb0Buf = nullptr; device->CreateBuffer(&c0d, &c0i, &cb0Buf);
    struct { UINT sx, ix, p0, p1; } cb1 = { startX, incX, 0, 0 };
    D3D11_BUFFER_DESC c1d{}; c1d.ByteWidth = sizeof(cb1); c1d.Usage = D3D11_USAGE_DEFAULT; c1d.BindFlags = D3D11_BIND_CONSTANT_BUFFER; D3D11_SUBRESOURCE_DATA c1i{}; c1i.pSysMem = &cb1; ID3D11Buffer* cb1Buf = nullptr; device->CreateBuffer(&c1d, &c1i, &cb1Buf);

    ID3D11ShaderResourceView* srvs[3] = { srvIn, srvDesc2, srvVal };
    ctx->CSSetShaderResources(0, 3, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &uavOut, nullptr);
    ID3D11Buffer* cbs[2] = { cb0Buf, cb1Buf }; ctx->CSSetConstantBuffers(0, 2, cbs);
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->Dispatch((tgtW + 7) / 8, (tgtH + 7) / 8, 1);
    ctx->Flush();

    uint8_t* gpuOut = (uint8_t*)gpu_tex::ReadbackBGRA(ctx, texOut, tgtW, tgtH);
    ID3D11ShaderResourceView* ns[3] = {}; ctx->CSSetShaderResources(0, 3, ns);
    ID3D11UnorderedAccessView* nu = nullptr; ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
    ID3D11Buffer* ncbs[2] = {}; ctx->CSSetConstantBuffers(0, 2, ncbs); ctx->CSSetShader(nullptr, nullptr, 0);

    int mism = 0;
    for (int i = 0; i < tgtW * tgtH * 4; ++i) if (gpuOut[i] != ref[i]) { if (mism < 5) wprintf(L"  mismatch @%d: gpu=%3d ref=%3d\n", i, gpuOut[i], ref[i]); ++mism; }
    if (mism == 0) wprintf(L"PASS: %dx%d->%dx%d X-pass matches CPU reference.\n", srcW, srcH, tgtW, tgtH);
    else wprintf(L"FAIL: %d mismatches out of %d\n", mism, tgtW * tgtH * 4);

    cs->Release(); srvIn->Release(); uavOut->Release(); srvDesc2->Release(); srvVal->Release();
    descBuf->Release(); valBuf->Release(); cb0Buf->Release(); cb1Buf->Release(); texSrc->Release(); texOut->Release();
    delete[] gpuOut; delete[] src; delete[] ref;
    return mism == 0 ? 0 : 1;
}
