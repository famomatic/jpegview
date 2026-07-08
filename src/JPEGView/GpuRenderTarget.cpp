#include "StdAfx.h"
#include "GpuRenderTarget.h"
#include "GpuDevice.h"
#include <d2d1.h>
#include <cstdio>

namespace {
bool IsD2DRequestedByEnv() {
    char buf[8] = {0};
    DWORD n = GetEnvironmentVariableA("JPEGVIEW_ENABLE_GPU_D2D", buf, sizeof(buf));
    return n > 0 && buf[0] != '0';
}
} // namespace

CGpuRenderTarget& CGpuRenderTarget::Instance() {
    static CGpuRenderTarget s_instance;
    return s_instance;
}

CGpuRenderTarget::CGpuRenderTarget()
    : m_pD2DFactory(nullptr)
    , m_pDCRT(nullptr)
    , m_pBitmap(nullptr)
    , m_pSurface(nullptr)
    , m_lastW(0)
    , m_lastH(0) {
    if (IsD2DRequestedByEnv()) {
        Init();
    }
}

CGpuRenderTarget::~CGpuRenderTarget() {
    if (m_pBitmap) { m_pBitmap->Release(); m_pBitmap = nullptr; }
    if (m_pSurface) { m_pSurface->Release(); m_pSurface = nullptr; }
    if (m_pDCRT) { m_pDCRT->Release(); m_pDCRT = nullptr; }
    if (m_pD2DFactory) { m_pD2DFactory->Release(); m_pD2DFactory = nullptr; }
}

bool CGpuRenderTarget::Init() {
    // Use the D2D 1.1 factory (CreateFactory on ID2D1Factory1) so we can build
    // ID2D1Bitmap1, but the render target is an ID2D1DCRenderTarget which binds
    // to a GDI DC - this keeps the existing GDI-based paint path while letting
    // us push BGRA pixels through the GPU for the final blit.
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1), (void**)&m_pD2DFactory);
    if (FAILED(hr) || !m_pD2DFactory) {
        return false;
    }

    // DC render target properties: BGRA, premultiplied alpha, DPI aware.
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0, 0, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    hr = m_pD2DFactory->CreateDCRenderTarget(&props, &m_pDCRT);
    if (FAILED(hr) || !m_pDCRT) {
        return false;
    }

    return true;
}

bool CGpuRenderTarget::DrawBGRA(HDC hdc, int dstX, int dstY, int width, int height,
    const void* pBGRA, int srcWidth, int srcHeight) {
    if (!IsAvailable() || !hdc || !pBGRA || width <= 0 || height <= 0) {
        return false;
    }

    // Bind the DC for this draw.
    RECT rcDst = { dstX, dstY, dstX + width, dstY + height };
    HRESULT hr = m_pDCRT->BindDC(hdc, &rcDst);
    if (FAILED(hr)) {
        return false;
    }

    // Create or reuse the bitmap. ID2D1DCRenderTarget returns ID2D1Bitmap (not
    // Bitmap1); cast through CreateCompatibleRenderTarget if needed. Here we
    // create a standalone bitmap via the factory.
    if (m_lastW != srcWidth || m_lastH != srcHeight || !m_pBitmap) {
        if (m_pBitmap) { m_pBitmap->Release(); m_pBitmap = nullptr; }
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED));
        D2D1_SIZE_U sz = D2D1::SizeU(srcWidth, srcHeight);
        hr = m_pDCRT->CreateBitmap(sz, nullptr, srcWidth * 4, &props, &m_pBitmap);
        if (FAILED(hr) || !m_pBitmap) {
            return false;
        }
        m_lastW = srcWidth;
        m_lastH = srcHeight;
    }

    D2D1_RECT_U rect = D2D1::RectU(0, 0, srcWidth, srcHeight);
    hr = m_pBitmap->CopyFromMemory(&rect, pBGRA, srcWidth * 4);
    if (FAILED(hr)) {
        return false;
    }

    m_pDCRT->BeginDraw();
    m_pDCRT->Clear(D2D1::ColorF(0, 0, 0, 0));
    D2D1_RECT_F dstF = D2D1::RectF((float)0, (float)0, (float)width, (float)height);
    D2D1_RECT_F srcF = D2D1::RectF(0, 0, (float)srcWidth, (float)srcHeight);
    m_pDCRT->DrawBitmap(m_pBitmap, &dstF, 1.0f,
        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &srcF);
    HRESULT endHr = m_pDCRT->EndDraw();
    if (endHr == D2DERR_RECREATE_TARGET) {
        if (m_pBitmap) { m_pBitmap->Release(); m_pBitmap = nullptr; }
        return false;
    }
    return SUCCEEDED(endHr);
}
