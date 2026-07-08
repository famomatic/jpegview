// Standalone console test for CGpuDevice. Built as a separate exe so we can
// verify device creation without launching the full WTL UI. Not shipped.
#include "GpuDevice.h"
#include <cstdio>
#include <cwchar>

int main() {
    CGpuDevice& dev = CGpuDevice::Instance();
    if (dev.IsAvailable()) {
        const wchar_t* desc = dev.AdapterDescription();
        wprintf(L"GPU device OK. Adapter: %s\n", desc ? desc : L"(unknown)");
        // Vendor id hint for logging.
        IDXGIAdapter* ad = dev.Adapter();
        if (ad) {
            DXGI_ADAPTER_DESC d{};
            if (SUCCEEDED(ad->GetDesc(&d))) {
                wprintf(L"  VendorId=0x%04x DeviceId=0x%04x DedicatedVideo=%llu MB SharedSystem=%llu MB\n",
                    d.VendorId, d.DeviceId,
                    (unsigned long long)(d.DedicatedVideoMemory / 1048576),
                    (unsigned long long)(d.SharedSystemMemory / 1048576));
            }
        }
        return 0;
    }
    wprintf(L"SKIP: no FL 11_0 GPU adapter available.\n");
    return 77;
}
