#define NOMINMAX
#include "GpuDevice.h"
#include "GpuShaders.h"
#include "GpuTextureHelpers.h"
#include <cstdio>
#include <cstring>
#include <d3dcompiler.h>
int main() {
    CGpuDevice& dev = CGpuDevice::Instance();
    if (!dev.IsAvailable()) { printf("no gpu\n"); return 2; }
    ID3D11Device* device = dev.Device();
    const char* src = gpu_shaders::kUnsharpMask_CS;
    ID3DBlob* code=nullptr; ID3DBlob* err=nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main","cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    printf("hr=0x%08x\n", (unsigned)hr);
    if (err) { printf("ERR: %s\n", (const char*)err->GetBufferPointer()); err->Release(); }
    if (code) code->Release();
    return 0;
}
