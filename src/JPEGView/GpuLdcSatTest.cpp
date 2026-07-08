#define NOMINMAX
#include "GpuDevice.h"
#include "GpuShaders.h"
#include "GpuTextureHelpers.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>

// CPU reference for the saturation LDC path. Matches ApplyLDC32bpp_Core
// pSatLUTs != NULL branch.
static void CpuRefLDCSat(const uint8_t* inBGRA, uint8_t* outBGRA,
    int dibW, int dibH, SIZE fullTarget, POINT fullOffset,
    SIZE mapSize, const int32_t* satLUTs, const uint8_t* lut, const uint8_t* ldcMap,
    float fBlackPt, float fWhitePt, float fBlackPtSteepness) {
    const float cfFactor = 0.8f, cfSB = 0.6f, cfSW = 0.6f;
    int32_t mulLUT[256];
    if (fWhitePt <= fBlackPt) { memset(mulLUT, 0, sizeof(mulLUT)); }
    else {
        float steep = std::max(0.0f, std::min(1.0f, 1.0f - 0.98f * fBlackPtSteepness));
        float fMid = (fBlackPt + fWhitePt) / 2;
        float fESB = fBlackPt + cfSB * (fMid - fBlackPt);
        float fESW = fWhitePt - cfSW * (fWhitePt - fMid);
        for (int i = 0; i < 256; i++) {
            float f = i * (1.0f / 255.0f); int v = 0;
            if (f >= fBlackPt && f < fWhitePt) {
                if (f < fESB) { float fv = (f - fBlackPt) / (fESB - fBlackPt); v = (int)(cfFactor * 16384.0f * powf(fv, steep) + 0.5f); }
                else if (f > fESW) { v = (int)(cfFactor * 16384.0f * (1.0f - (f - fESW) / (fWhitePt - fESW)) + 0.5f); }
                else { v = (int)(cfFactor * 16384.0f + 0.5f); }
            }
            mulLUT[i] = v;
        }
    }
    const int cnMax = 255 << 16;
    uint32_t incX = (uint32_t)((65536 * (uint32_t)(mapSize.cx - 1)) / (fullTarget.cx - 1) - 1);
    uint32_t incY = (uint32_t)((65536 * (uint32_t)(mapSize.cy - 1)) / (fullTarget.cy - 1) - 1);
    uint32_t curY = fullOffset.y * incY;
    uint32_t startX = fullOffset.x * incX;
    for (int j = 0; j < dibH; j++) {
        uint32_t curYTrunc = curY >> 16;
        uint32_t curYFrac = curY & 0xFFFF;
        const uint8_t* pMap = ldcMap + mapSize.cx * curYTrunc;
        uint32_t curX = startX;
        for (int i = 0; i < dibW; i++) {
            uint32_t curXTrunc = curX >> 16;
            uint32_t curXFrac = curX & 0xFFFF;
            uint32_t mTL = pMap[curXTrunc], mTR = pMap[curXTrunc + 1];
            uint32_t mBL = pMap[curXTrunc + mapSize.cx], mBR = pMap[curXTrunc + mapSize.cx + 1];
            int32_t nLeft = ((int32_t)curYFrac * (int32_t)(mBL - mTL) >> 16) + mTL;
            int32_t nRight = ((int32_t)curYFrac * (int32_t)(mBR - mTR) >> 16) + mTR;
            int32_t nMaskValue = ((int32_t)curXFrac * (int32_t)(nRight - nLeft) >> 16) + nLeft - 127;
            uint32_t src = *(const uint32_t*)(inBGRA + (j * dibW + i) * 4);
            int32_t nSrcBlue = src & 0xFF;
            int32_t nSrcGreen = (src >> 8) & 0xFF;
            int32_t nSrcRed = (src >> 16) & 0xFF;
            int32_t nRed = satLUTs[nSrcRed] + satLUTs[256 + nSrcGreen] + satLUTs[512 + nSrcBlue];
            int32_t nGreen = satLUTs[768 + nSrcRed] + satLUTs[1024 + nSrcGreen] + satLUTs[512 + nSrcBlue];
            int32_t nBlue = satLUTs[768 + nSrcRed] + satLUTs[256 + nSrcGreen] + satLUTs[1280 + nSrcBlue];
            nBlue = lut[std::max(0, std::min(cnMax, nBlue)) >> 16];
            nBlue = nBlue + ((nMaskValue * mulLUT[nBlue] >> 14));
            nGreen = lut[(std::max(0, std::min(cnMax, nGreen)) >> 16) + 256];
            nGreen = nGreen + ((nMaskValue * mulLUT[nGreen] >> 14));
            nRed = lut[(std::max(0, std::min(cnMax, nRed)) >> 16) + 512];
            nRed = nRed + ((nMaskValue * mulLUT[nRed] >> 14));
            uint32_t out = (uint32_t)(std::max(0, std::min(255, (int)nBlue)))
                         + (uint32_t)(std::max(0, std::min(255, (int)nGreen))) * 256
                         + (uint32_t)(std::max(0, std::min(255, (int)nRed))) * 65536 + 0xFF000000u;
            *(uint32_t*)(outBGRA + (j * dibW + i) * 4) = out;
            curX += incX;
        }
        curY += incY;
    }
}

int main() {
    const int W = 64, H = 48;
    SIZE fullTarget = { (LONG)W, (LONG)H };
    POINT fullOffset = { 0, 0 };
    SIZE mapSize = { 8, 6 };

    uint8_t* input = new uint8_t[W * H * 4];
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        input[(y * W + x) * 4 + 0] = (uint8_t)((x + y) & 255);
        input[(y * W + x) * 4 + 1] = (uint8_t)((x * 2 + y) & 255);
        input[(y * W + x) * 4 + 2] = (uint8_t)((x + y * 3) & 255);
        input[(y * W + x) * 4 + 3] = 0x80;
    }
    uint8_t lut[768];
    for (int i = 0; i < 768; ++i) lut[i] = (uint8_t)(255 - (i & 255));
    uint8_t* ldcMap = new uint8_t[mapSize.cx * mapSize.cy + 8];
    for (int i = 0; i < mapSize.cx * mapSize.cy + 8; ++i) ldcMap[i] = (uint8_t)((i * 37) & 255);

    // Saturation LUTs: 1536 int32. Fill with a simple pattern.
    int32_t* satLUTs = new int32_t[1536];
    for (int i = 0; i < 1536; ++i) satLUTs[i] = (int32_t)((i * 53) % (1 << 16));

    uint8_t* ref = new uint8_t[W * H * 4];
    CpuRefLDCSat(input, ref, W, H, fullTarget, fullOffset, mapSize, satLUTs, lut, ldcMap, 0.1f, 0.9f, 0.5f);

    CGpuDevice& dev = CGpuDevice::Instance();
    if (!dev.IsAvailable()) { wprintf(L"SKIP: no GPU\n"); return 77; }
    ID3D11Device* device = dev.Device();
    ID3D11DeviceContext* ctx = dev.ImmediateContext();
    ID3D11ComputeShader* cs = gpu_shaders::CompileComputeShader(device, gpu_shaders::kApplyLDC32bppSat_CS, "main", "cs_5_0");
    if (!cs) { wprintf(L"FAIL: shader compile\n"); return 3; }

    ID3D11Texture2D* texIn = gpu_tex::CreateTextureFmt(device, W, H, D3D11_BIND_SHADER_RESOURCE, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    ID3D11Texture2D* texOut = gpu_tex::CreateTextureFmt(device, W, H, D3D11_BIND_UNORDERED_ACCESS, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    ID3D11Texture2D* texMap = gpu_tex::CreateTextureFmt(device, mapSize.cx, mapSize.cy, D3D11_BIND_SHADER_RESOURCE, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8_UINT);
    gpu_tex::UploadBGRA(ctx, texIn, W, H, input);
    ctx->UpdateSubresource(texMap, 0, nullptr, ldcMap, mapSize.cx, 0);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{}; srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT; srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srvIn = nullptr; device->CreateShaderResourceView(texIn, &srvDesc, &srvIn);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{}; uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT; uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; uavDesc.Texture2D.MipSlice = 0;
    ID3D11UnorderedAccessView* uavOut = nullptr; device->CreateUnorderedAccessView(texOut, &uavDesc, &uavOut);
    D3D11_SHADER_RESOURCE_VIEW_DESC mapSrv{}; mapSrv.Format = DXGI_FORMAT_R8_UINT; mapSrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; mapSrv.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srvMap = nullptr; device->CreateShaderResourceView(texMap, &mapSrv, &srvMap);

    UINT lutExp[768]; for (int i = 0; i < 768; ++i) lutExp[i] = lut[i];
    D3D11_BUFFER_DESC ld{}; ld.ByteWidth = 768 * sizeof(UINT); ld.Usage = D3D11_USAGE_DEFAULT; ld.BindFlags = D3D11_BIND_SHADER_RESOURCE; ld.StructureByteStride = sizeof(UINT); ld.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA li{}; li.pSysMem = lutExp; ID3D11Buffer* lutBuf = nullptr; device->CreateBuffer(&ld, &li, &lutBuf);
    D3D11_SHADER_RESOURCE_VIEW_DESC lsrv{}; lsrv.Format = DXGI_FORMAT_UNKNOWN; lsrv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; lsrv.Buffer.NumElements = 768;
    ID3D11ShaderResourceView* srvLut = nullptr; device->CreateShaderResourceView(lutBuf, &lsrv, &srvLut);

    int32_t mulLUT[256];
    const float cfF = 0.8f, cfSB = 0.6f, cfSW = 0.6f; float fBP = 0.1f, fWP = 0.9f, fSt = 0.5f;
    float steep = std::max(0.0f, std::min(1.0f, 1.0f - 0.98f * fSt)); float fMid = (fBP + fWP) / 2;
    float fESB = fBP + cfSB * (fMid - fBP); float fESW = fWP - cfSW * (fWP - fMid);
    for (int i = 0; i < 256; ++i) { float f = i / 255.0f; int v = 0; if (f >= fBP && f < fWP) { if (f < fESB) { float fv = (f - fBP) / (fESB - fBP); v = (int)(cfF * 16384.0f * powf(fv, steep) + 0.5f); } else if (f > fESW) { v = (int)(cfF * 16384.0f * (1.0f - (f - fESW) / (fWP - fESW)) + 0.5f); } else { v = (int)(cfF * 16384.0f + 0.5f); } } mulLUT[i] = v; }
    D3D11_BUFFER_DESC md{}; md.ByteWidth = 256 * sizeof(int32_t); md.Usage = D3D11_USAGE_DEFAULT; md.BindFlags = D3D11_BIND_SHADER_RESOURCE; md.StructureByteStride = sizeof(int32_t); md.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA mi{}; mi.pSysMem = mulLUT; ID3D11Buffer* mulBuf = nullptr; device->CreateBuffer(&md, &mi, &mulBuf);
    D3D11_SHADER_RESOURCE_VIEW_DESC msrv{}; msrv.Format = DXGI_FORMAT_UNKNOWN; msrv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; msrv.Buffer.NumElements = 256;
    ID3D11ShaderResourceView* srvMul = nullptr; device->CreateShaderResourceView(mulBuf, &msrv, &srvMul);

    // Saturation LUTs buffer (1536 int32).
    D3D11_BUFFER_DESC satd{}; satd.ByteWidth = 1536 * sizeof(int32_t); satd.Usage = D3D11_USAGE_DEFAULT; satd.BindFlags = D3D11_BIND_SHADER_RESOURCE; satd.StructureByteStride = sizeof(int32_t); satd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA sati{}; sati.pSysMem = satLUTs; ID3D11Buffer* satBuf = nullptr; device->CreateBuffer(&satd, &sati, &satBuf);
    D3D11_SHADER_RESOURCE_VIEW_DESC satsrv{}; satsrv.Format = DXGI_FORMAT_UNKNOWN; satsrv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; satsrv.Buffer.NumElements = 1536;
    ID3D11ShaderResourceView* srvSat = nullptr; device->CreateShaderResourceView(satBuf, &satsrv, &srvSat);

    struct { UINT w, h, mw, mh; } cb0 = { (UINT)W, (UINT)H, (UINT)mapSize.cx, (UINT)mapSize.cy };
    D3D11_BUFFER_DESC c0d{}; c0d.ByteWidth = sizeof(cb0); c0d.Usage = D3D11_USAGE_DEFAULT; c0d.BindFlags = D3D11_BIND_CONSTANT_BUFFER; D3D11_SUBRESOURCE_DATA c0i{}; c0i.pSysMem = &cb0; ID3D11Buffer* cb0Buf = nullptr; device->CreateBuffer(&c0d, &c0i, &cb0Buf);
    uint32_t incX = (uint32_t)((65536 * (uint32_t)(mapSize.cx - 1)) / (fullTarget.cx - 1) - 1);
    uint32_t incY = (uint32_t)((65536 * (uint32_t)(mapSize.cy - 1)) / (fullTarget.cy - 1) - 1);
    struct { UINT ix, sx, iy, cy0; } cb1Data = { incX, 0, incY, 0 };
    D3D11_BUFFER_DESC c1d{}; c1d.ByteWidth = sizeof(cb1Data); c1d.Usage = D3D11_USAGE_DEFAULT; c1d.BindFlags = D3D11_BIND_CONSTANT_BUFFER; D3D11_SUBRESOURCE_DATA c1i{}; c1i.pSysMem = &cb1Data; ID3D11Buffer* cb1Buf = nullptr; device->CreateBuffer(&c1d, &c1i, &cb1Buf);

    ID3D11ShaderResourceView* srvs[5] = { srvIn, srvLut, srvMul, srvMap, srvSat };
    ctx->CSSetShaderResources(0, 5, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &uavOut, nullptr);
    ID3D11Buffer* cbs[2] = { cb0Buf, cb1Buf }; ctx->CSSetConstantBuffers(0, 2, cbs);
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
    ctx->Flush();

    uint8_t* gpuOut = (uint8_t*)gpu_tex::ReadbackBGRA(ctx, texOut, W, H);
    ID3D11ShaderResourceView* ns[5] = {}; ctx->CSSetShaderResources(0, 5, ns);
    ID3D11UnorderedAccessView* nu = nullptr; ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
    ID3D11Buffer* ncbs[2] = {}; ctx->CSSetConstantBuffers(0, 2, ncbs); ctx->CSSetShader(nullptr, nullptr, 0);

    int mism = 0;
    for (int i = 0; i < W * H * 4; ++i) if (gpuOut[i] != ref[i]) { if (mism < 5) wprintf(L"  mismatch @%d: gpu=%3d ref=%3d\n", i, gpuOut[i], ref[i]); ++mism; }
    if (mism == 0) wprintf(L"PASS: %dx%d LDC saturation matches CPU reference.\n", W, H);
    else wprintf(L"FAIL: %d mismatches out of %d\n", mism, W * H * 4);

    cs->Release(); srvIn->Release(); uavOut->Release(); srvMap->Release(); srvLut->Release(); srvMul->Release(); srvSat->Release();
    lutBuf->Release(); mulBuf->Release(); satBuf->Release(); cb0Buf->Release(); cb1Buf->Release(); texIn->Release(); texOut->Release(); texMap->Release();
    delete[] gpuOut; delete[] input; delete[] ref; delete[] ldcMap; delete[] satLUTs;
    return mism == 0 ? 0 : 1;
}
