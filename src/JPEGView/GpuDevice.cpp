#include "GpuDevice.h"
#include <cstdio>
#include <cstring>

// When built as part of JPEGView, StdAfx.h provides the Windows headers.
// The standalone GpuDeviceTest console target compiles this TU without the
// precompiled header, so pull in the Windows headers explicitly there. The
// include guard makes this safe for both contexts.
#ifndef _WINDOWS_
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// ===========================================================================
// D3D11 device creation for the GPU backend.
//
// Strategy: enumerate adapters via IDXGIFactory1, and try to create a FL 11_0
// device on each. The order prefers the iGPU:
//   pass 1 - Intel adapters (vendor 0x8086), which are iGPUs on consumer
//            systems, and AMD adapters whose dedicated video memory is small
//            (the iGPU part of an APU; a dGPU has multi-GB dedicated).
//   pass 2 - everything else in enumeration order, so a discrete-only box or
//            a box whose iGPU driver is broken still gets a usable device.
//
// Why prefer the iGPU: a light image viewer should not spin up the dGPU power
// state unless that is the only option, and the iGPU's near-zero upload cost
// (often shared memory) suits the single-image decode->process->show pattern.
// ===========================================================================

namespace {
// DXGI description strings can be wide; keep a static buffer for logging.
wchar_t g_descBuf[128] = L"none";

// Intel is the dominant iGPU vendor. AMD iGPUs are distinguished from dGPUs by
// small dedicated video memory; NVIDIA has no consumer iGPU.
bool IsLikelyIntegratedGPU(const DXGI_ADAPTER_DESC1& desc) {
    const UINT INTEL_VENDOR = 0x8086;
    const UINT MS_VENDOR = 0x1414;
    // The Microsoft Basic Render Driver (WARP) and software-only adapters are
    // not real GPUs; never treat them as the preferred iGPU - they are a
    // software fallback that should only be used as a last resort.
    if (desc.VendorId == MS_VENDOR) {
        return false;
    }
    if (desc.VendorId == INTEL_VENDOR) {
        return true;
    }
    // AMD APU iGPUs typically report little or no dedicated video memory
    // (they share system memory). Treat < 512 MB dedicated as "integrated".
    // This is a heuristic; a weak dGPU could match, but pass-2 ordering means
    // the worst case is using a weak dGPU, not a correctness problem.
    return desc.DedicatedVideoMemory < (512ull * 1024 * 1024);
}
} // namespace

CGpuDevice& CGpuDevice::Instance() {
    static CGpuDevice s_instance;
    return s_instance;
}

CGpuDevice::CGpuDevice()
    : m_pDevice(nullptr)
    , m_pImmediateContext(nullptr)
    , m_pAdapter(nullptr)
    , m_haveDesc(false) {
    m_adapterDesc = DXGI_ADAPTER_DESC{};
    Init();
}

CGpuDevice::~CGpuDevice() {
    if (m_pImmediateContext) { m_pImmediateContext->Release(); m_pImmediateContext = nullptr; }
    if (m_pDevice) { m_pDevice->Release(); m_pDevice = nullptr; }
    if (m_pAdapter) { m_pAdapter->Release(); m_pAdapter = nullptr; }
}

// Attempts a FL 11_0 device on the given adapter. On success, takes ownership
// of the adapter (caller must not release it), stores the device/context, and
// fills m_adapterDesc. Returns true on success.
bool CGpuDevice::TryCreateOnAdapter(IDXGIAdapter1* adapter1) {
    if (adapter1 == nullptr) {
        return false;
    }

    DXGI_ADAPTER_DESC1 desc1{};
    if (SUCCEEDED(adapter1->GetDesc1(&desc1))) {
        memset(m_adapterDesc.Description, 0, sizeof(m_adapterDesc.Description));
        for (int j = 0; j < 128 && desc1.Description[j] != 0; ++j) {
            m_adapterDesc.Description[j] = desc1.Description[j];
        }
        m_adapterDesc.VendorId = desc1.VendorId;
        m_adapterDesc.DeviceId = desc1.DeviceId;
        m_adapterDesc.DedicatedVideoMemory = desc1.DedicatedVideoMemory;
        m_adapterDesc.DedicatedSystemMemory = desc1.DedicatedSystemMemory;
        m_adapterDesc.SharedSystemMemory = desc1.SharedSystemMemory;
        m_haveDesc = true;
    }

    const D3D_FEATURE_LEVEL requestedLevels[] = { D3D_FEATURE_LEVEL_11_0 };
    UINT flags = 0;
    // BGRA support is required for Direct2D interop (Step 4 output path)
    // and is harmless for pure compute. Always request it.
    D3D_FEATURE_LEVEL obtainedLevel = D3D_FEATURE_LEVEL_9_1;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(
        adapter1,
        D3D_DRIVER_TYPE_UNKNOWN, // must be UNKNOWN when passing an adapter
        nullptr,                 // no software rasterizer
        flags,
        requestedLevels,
        1,
        D3D11_SDK_VERSION,
        &device,
        &obtainedLevel,
        &ctx);

    if (SUCCEEDED(hr) && obtainedLevel == D3D_FEATURE_LEVEL_11_0 && device && ctx) {
        m_pAdapter = adapter1; // keep ref (caller must not double-release)
        m_pDevice = device;
        m_pImmediateContext = ctx;
        return true;
    }
    // Failed on this adapter; release and let the caller try the next.
    if (device) device->Release();
    if (ctx) ctx->Release();
    m_haveDesc = false;
    return false;
}

bool CGpuDevice::Init() {
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr) || factory == nullptr) {
        return false;
    }

    // Collect all adapters first so we can make two ordered passes without
    // re-enumerating.
    const int MAX_ADAPTERS = 8;
    IDXGIAdapter1* adapters[MAX_ADAPTERS] = {};
    DXGI_ADAPTER_DESC1 descs[MAX_ADAPTERS] = {};
    int nAdapters = 0;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        if (nAdapters >= MAX_ADAPTERS) {
            a->Release();
            break;
        }
        adapters[nAdapters] = a;
        if (FAILED(a->GetDesc1(&descs[nAdapters]))) {
            descs[nAdapters] = DXGI_ADAPTER_DESC1{};
        }
        // Skip the Microsoft Basic Render Driver (WARP). It is a software
        // fallback, not a hardware GPU; using it would be slower than CPU and
        // defeats the purpose of the GPU backend. Only real adapters remain.
        if (descs[nAdapters].VendorId == 0x1414) {
            a->Release();
            adapters[nAdapters] = nullptr;
            continue;
        }
        nAdapters++;
    }

    bool ok = false;
    // Pass 1: integrated GPUs first (Intel, or small-dedicated-memory AMD).
    for (int i = 0; i < nAdapters && !ok; ++i) {
        if (IsLikelyIntegratedGPU(descs[i])) {
            ok = TryCreateOnAdapter(adapters[i]);
            if (ok) {
                // mark as taken so pass 2 / cleanup skips it
                adapters[i] = nullptr;
            }
        }
    }
    // Pass 2: everything else in enumeration order.
    for (int i = 0; i < nAdapters && !ok; ++i) {
        if (adapters[i] != nullptr) {
            ok = TryCreateOnAdapter(adapters[i]);
            if (ok) {
                adapters[i] = nullptr;
            }
        }
    }

    // Release any adapters we did not take.
    for (int i = 0; i < nAdapters; ++i) {
        if (adapters[i] != nullptr) {
            adapters[i]->Release();
            adapters[i] = nullptr;
        }
    }

    factory->Release();
    return ok;
}

const wchar_t* CGpuDevice::AdapterDescription() const {
    if (!m_haveDesc) {
        return L"none";
    }
    int n = 0;
    for (; n < 127 && m_adapterDesc.Description[n] != 0; ++n) {
        g_descBuf[n] = m_adapterDesc.Description[n];
    }
    g_descBuf[n] = 0;
    return g_descBuf;
}
