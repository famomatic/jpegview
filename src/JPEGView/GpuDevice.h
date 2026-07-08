#pragma once

// ===========================================================================
// D3D11 device manager for the GPU image processor backend (Step 2)
// ===========================================================================
//
// One D3D11 device per process, created lazily on first use. Device selection
// prefers adapter 0, which on typical consumer systems is the iGPU on an
// Intel/AMD APU or the primary GPU on a discrete-only box. The goal is to keep
// the light viewer off the dGPU power states unless that is the only adapter.
//
// A FL 11_0 (feature level) device is required for compute shaders + UAVs,
// which is the minimum the Step 3 GPU passes rely on. If the chosen adapter
// cannot provide FL 11_0, init fails and the caller falls back to the CPU
// backend without ever presenting a GPU surface.
//
// Thread safety: the device is created once and then treated as
// single-threaded from the caller's perspective (JPEGView processes images on
// its processing thread pool, not concurrently from the UI). The D3D11 device
// itself is thread-safe by default only with the D3D11_CREATE_DEVICE_BGRA_SUPPORT
// flag set for Direct2D interop, which is enabled here in preparation for the
// Step 4 output backend.

#include <d3d11.h>
#include <dxgi.h>

class CGpuDevice {
public:
    // Returns the process-wide device, or NULL if init failed (no FL 11_0
    // adapter available). The first call performs the (potentially slow)
    // adapter enumeration and device creation; subsequent calls are cheap.
    static CGpuDevice& Instance();

    ~CGpuDevice();

    bool IsAvailable() const { return m_pDevice != nullptr; }

    ID3D11Device* Device() { return m_pDevice; }
    ID3D11DeviceContext* ImmediateContext() { return m_pImmediateContext; }
    IDXGIAdapter* Adapter() { return m_pAdapter; }

    // Human-readable description of the selected adapter (or "none"), for
    // logging / the future backend-selection dialog.
    const wchar_t* AdapterDescription() const;

private:
    CGpuDevice();

    bool Init();
    // Tries to create a FL 11_0 device on the given adapter; on success takes
    // ownership of the adapter (does not release it). Returns true on success.
    bool TryCreateOnAdapter(IDXGIAdapter1* adapter1);

    ID3D11Device* m_pDevice;
    ID3D11DeviceContext* m_pImmediateContext;
    IDXGIAdapter* m_pAdapter;
    DXGI_ADAPTER_DESC m_adapterDesc;
    bool m_haveDesc;
};
