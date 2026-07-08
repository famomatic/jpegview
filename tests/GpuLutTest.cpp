// Standalone accuracy test for the Apply3ChannelLUT32bpp GPU compute pass.
// Generates a small BGRA image + LUT, runs the same shader pipeline used by
// GpuImageProcessor, and compares against a straightforward CPU reference.
// Not shipped.
#include "GpuDevice.h"
#include "GpuShaders.h"
#include "GpuTextureHelpers.h"
#include <cstdio>
#include <cstring>
#include <cstdint>

// CPU reference for Apply3ChannelLUT32bpp: matches CBasicProcessing layout
// (256 B, 256 G, 256 R) and forces alpha to 0xFF.
static void CpuReference(const uint8_t* inBGRA, uint8_t* outBGRA,
    int w, int h, const uint8_t* lut) {
    for (int i = 0; i < w * h; ++i) {
        uint8_t b = inBGRA[i * 4 + 0];
        uint8_t g = inBGRA[i * 4 + 1];
        uint8_t r = inBGRA[i * 4 + 2];
        outBGRA[i * 4 + 0] = lut[b];
        outBGRA[i * 4 + 1] = lut[256 + g];
        outBGRA[i * 4 + 2] = lut[512 + r];
        outBGRA[i * 4 + 3] = 0xFF;
    }
}

int main() {
    const int W = 64, H = 48;

    // Build a test image: each pixel (b,g,r,a) = ((x+y)&255, (x*2)&255, (y*3)&255, 0x80)
    uint8_t* input = new uint8_t[W * H * 4];
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            input[(y * W + x) * 4 + 0] = (uint8_t)((x + y) & 255);
            input[(y * W + x) * 4 + 1] = (uint8_t)((x * 2) & 255);
            input[(y * W + x) * 4 + 2] = (uint8_t)((y * 3) & 255);
            input[(y * W + x) * 4 + 3] = 0x80;
        }
    }

    // LUT: invert each channel. lut[c] = 255 - c.
    uint8_t lut[768];
    for (int i = 0; i < 768; ++i) lut[i] = (uint8_t)(255 - (i & 255));

    // CPU reference.
    uint8_t* ref = new uint8_t[W * H * 4];
    CpuReference(input, ref, W, H, lut);

    // GPU path: mirror GpuImageProcessor::Apply3ChannelLUT32bpp inline.
    CGpuDevice& dev = CGpuDevice::Instance();
    if (!dev.IsAvailable()) {
        wprintf(L"SKIP: no GPU device\n");
        delete[] input; delete[] ref;
        return 77;
    }
    ID3D11Device* device = dev.Device();
    ID3D11DeviceContext* ctx = dev.ImmediateContext();

    ID3D11ComputeShader* cs = gpu_shaders::CompileComputeShader(
        device, gpu_shaders::kApply3ChannelLUT_CS, "main", "cs_5_0");
    if (!cs) { wprintf(L"FAIL: shader compile\n"); delete[] input; delete[] ref; return 3; }

    ID3D11Texture2D* texIn = gpu_tex::CreateTexture(device, W, H,
        D3D11_BIND_SHADER_RESOURCE, false, (D3D11_CPU_ACCESS_FLAG)0);
    ID3D11Texture2D* texOut = gpu_tex::CreateTexture(device, W, H,
        D3D11_BIND_UNORDERED_ACCESS, false, (D3D11_CPU_ACCESS_FLAG)0);
    // UINT format so the shader loads channels as raw 0..255 integers.
    if (texIn) { texIn->Release(); texIn = nullptr; }
    if (texOut) { texOut->Release(); texOut = nullptr; }
    texIn = gpu_tex::CreateTextureFmt(device, W, H,
        D3D11_BIND_SHADER_RESOURCE, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    texOut = gpu_tex::CreateTextureFmt(device, W, H,
        D3D11_BIND_UNORDERED_ACCESS, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    if (!texIn || !texOut) { wprintf(L"FAIL: tex create\n"); return 4; }

    gpu_tex::UploadBGRA(ctx, texIn, W, H, input);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{}; srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    // UINT format matches the shader's uint4 load (raw byte indexing).
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srvIn = nullptr; device->CreateShaderResourceView(texIn, &srvDesc, &srvIn);

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{}; uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; uavDesc.Texture2D.MipSlice = 0;
    ID3D11UnorderedAccessView* uavOut = nullptr; device->CreateUnorderedAccessView(texOut, &uavDesc, &uavOut);

    // LUT as a Buffer<uint> (768 uints): 256 B, 256 G, 256 R. Host expands
    // the 768-byte LUT so the shader can index by channel value directly.
    UINT lutExpanded[3 * 256];
    for (int i = 0; i < 3 * 256; ++i) lutExpanded[i] = lut[i];
    D3D11_BUFFER_DESC lutDesc{}; lutDesc.ByteWidth = 3 * 256 * sizeof(UINT); lutDesc.Usage = D3D11_USAGE_DEFAULT;
    lutDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE; lutDesc.StructureByteStride = sizeof(UINT);
    lutDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA lutInit{}; lutInit.pSysMem = lutExpanded;
    ID3D11Buffer* lutBuf = nullptr; device->CreateBuffer(&lutDesc, &lutInit, &lutBuf);
    D3D11_SHADER_RESOURCE_VIEW_DESC lutSrvDesc{}; lutSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    lutSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; lutSrvDesc.Buffer.NumElements = 3 * 256;
    ID3D11ShaderResourceView* lutSrv = nullptr; device->CreateShaderResourceView(lutBuf, &lutSrvDesc, &lutSrv);

    struct { UINT w, h, p0, p1; } cb = { (UINT)W, (UINT)H, 0, 0 };
    D3D11_BUFFER_DESC cbDesc{}; cbDesc.ByteWidth = sizeof(cb); cbDesc.Usage = D3D11_USAGE_DEFAULT; cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cbInit{}; cbInit.pSysMem = &cb;
    ID3D11Buffer* cbBuf = nullptr; device->CreateBuffer(&cbDesc, &cbInit, &cbBuf);

    ID3D11ShaderResourceView* srvs[2] = { srvIn, lutSrv };
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &uavOut, nullptr);
    ctx->CSSetConstantBuffers(0, 1, &cbBuf);
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
    ctx->Flush();

    uint8_t* gpuOut = (uint8_t*)gpu_tex::ReadbackBGRA(ctx, texOut, W, H);

    ID3D11ShaderResourceView* ns[2] = { nullptr, nullptr };
    ctx->CSSetShaderResources(0, 2, ns);
    ID3D11UnorderedAccessView* nu = nullptr; ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
    ID3D11Buffer* ncb = nullptr; ctx->CSSetConstantBuffers(0, 1, &ncb);
    ctx->CSSetShader(nullptr, nullptr, 0);

    if (!gpuOut) { wprintf(L"FAIL: readback\n"); return 5; }

    // Compare. Allow exact match for a pure 8-bit LUT (no interpolation).
    int mism = 0;
    for (int i = 0; i < W * H * 4; ++i) {
        if (gpuOut[i] != ref[i]) {
            if (mism < 5) wprintf(L"  mismatch @%d: gpu=%3d ref=%3d\n", i, gpuOut[i], ref[i]);
            ++mism;
        }
    }

    if (mism == 0) {
        wprintf(L"PASS: %dx%d LUT apply matches CPU reference exactly.\n", W, H);
    } else {
        wprintf(L"FAIL: %d byte mismatches out of %d\n", mism, W * H * 4);
    }

    cs->Release(); srvIn->Release(); uavOut->Release(); lutSrv->Release();
    lutBuf->Release(); cbBuf->Release(); texIn->Release(); texOut->Release();
    delete[] gpuOut; delete[] input; delete[] ref;
    return mism == 0 ? 0 : 1;
}
