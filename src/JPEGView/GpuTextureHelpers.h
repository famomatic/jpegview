#pragma once

// ===========================================================================
// Texture staging helpers for the GPU image processor.
//
// These wrap the repetitive upload -> dispatch -> readback cycle that every
// compute pixel pass needs:
//   - Create a staging texture (CPU-writable) from a BGRA pixel buffer.
//   - Create a GPU texture + shader resource view + unordered access view.
//   - Copy upload -> GPU.
//   - After dispatch, copy GPU -> staging readback and return a heap buffer
//     in the same BGRA layout the CPU callers expect.
//
// Keeping these here means each compute pass (LUT, resample, ...) only has
// to set its shaders and constant buffers.
// ===========================================================================

#include <d3d11.h>

namespace gpu_tex {

// Creates a R8G8B8A8_UNORM texture with the given bind flags. If
// bStaging is true, the texture is CPU-accessible (usage STAGING, no GPU
// bind) for upload/readback. Returns the texture or nullptr on failure.
ID3D11Texture2D* CreateTexture(ID3D11Device* device, int width, int height,
   UINT bindFlags, bool bStaging, D3D11_CPU_ACCESS_FLAG cpuAccess);

// Same as above but with an explicit format (e.g. R8G8B8A8_UINT for byte
// exact compute paths that read channels as integers).
ID3D11Texture2D* CreateTextureFmt(ID3D11Device* device, int width, int height,
    UINT bindFlags, bool bStaging, D3D11_CPU_ACCESS_FLAG cpuAccess,
    DXGI_FORMAT format);

// Uploads a BGRA pixel buffer (4 bytes/pixel, tightly packed) into a fresh
// staging texture and copies it to the GPU texture dstGPU. Call after
// creating dstGPU with CreateTexture(... bind=SRV|UAV ...). Returns true on
// success. srcPixels must point to width*height*4 bytes.
bool UploadBGRA(ID3D11DeviceContext* ctx, ID3D11Texture2D* dstGPU,
    int width, int height, const void* srcPixels);

// Reads a GPU texture back to a CPU heap buffer (width*height*4 bytes,
// caller must delete[]). Returns nullptr on failure. The returned buffer
// is tightly packed BGRA matching the original CPU layout.
void* ReadbackBGRA(ID3D11DeviceContext* ctx, ID3D11Texture2D* srcGPU,
    int width, int height);

} // namespace gpu_tex
