#include "GpuTextureHelpers.h"
#include <cstring>
#include <new>

// Standalone-safe Windows headers (see GpuDevice.cpp for rationale).
#ifndef _WINDOWS_
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace gpu_tex {

ID3D11Texture2D* CreateTexture(ID3D11Device* device, int width, int height,
   UINT bindFlags, bool bStaging, D3D11_CPU_ACCESS_FLAG cpuAccess) {
    return CreateTextureFmt(device, width, height, bindFlags, bStaging, cpuAccess,
        DXGI_FORMAT_R8G8B8A8_UNORM);
}

ID3D11Texture2D* CreateTextureFmt(ID3D11Device* device, int width, int height,
    UINT bindFlags, bool bStaging, D3D11_CPU_ACCESS_FLAG cpuAccess,
    DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = bStaging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    desc.BindFlags = bStaging ? 0 : bindFlags;
    desc.CPUAccessFlags = bStaging ? (UINT)cpuAccess : 0;
    desc.MiscFlags = 0;

    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &tex);
    if (FAILED(hr) || tex == nullptr) {
        return nullptr;
    }
    return tex;
}

bool UploadBGRA(ID3D11DeviceContext* ctx, ID3D11Texture2D* dstGPU,
    int width, int height, const void* srcPixels) {
    if (ctx == nullptr || dstGPU == nullptr || srcPixels == nullptr) {
        return false;
    }
    // Use UpdateSubresource for the simple case: it handles the internal
    // staging copy for DEFAULT-usage textures. The BGRA layout matches
    // R8G8B8A8_UNORM exactly (4 bytes/pixel, tightly packed).
    D3D11_BOX box{};
    box.left = 0;
    box.top = 0;
    box.front = 0;
    box.right = width;
    box.bottom = height;
    box.back = 1;
    // Row pitch must be a multiple of 512 for UpdateSubresource per the spec
    // when using a D3D11_BOX with a system-memory pointer, but the simpler
    // path via UpdateSubresource (no box) expects the full texture.
    ctx->UpdateSubresource(dstGPU, 0, &box, srcPixels, width * 4, 0);
    return true;
}

void* ReadbackBGRA(ID3D11DeviceContext* ctx, ID3D11Texture2D* srcGPU,
    int width, int height) {
    if (ctx == nullptr || srcGPU == nullptr) {
        return nullptr;
    }

    // Get the device from the context to create the staging texture.
    ID3D11Device* device = nullptr;
    srcGPU->GetDevice(&device);
    if (device == nullptr) {
        return nullptr;
    }

    ID3D11Texture2D* staging = CreateTexture(device, width, height, 0,
        true, D3D11_CPU_ACCESS_READ);
    device->Release();
    if (staging == nullptr) {
        return nullptr;
    }

    ctx->CopyResource(staging, srcGPU);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || mapped.pData == nullptr) {
        staging->Release();
        return nullptr;
    }

    // Copy into a tightly packed heap buffer (mapped may have a larger pitch).
    const int rowBytes = width * 4;
    uint8_t* out = new(std::nothrow) uint8_t[(size_t)rowBytes * height];
    if (out == nullptr) {
        ctx->Unmap(staging, 0);
        staging->Release();
        return nullptr;
    }
    const uint8_t* src = (const uint8_t*)mapped.pData;
    for (int y = 0; y < height; ++y) {
        memcpy(out + (size_t)y * rowBytes, src + (size_t)y * mapped.RowPitch, rowBytes);
    }
    ctx->Unmap(staging, 0);
    staging->Release();
    return out;
}

} // namespace gpu_tex
