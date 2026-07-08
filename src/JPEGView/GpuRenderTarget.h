#pragma once

// ===========================================================================
// Direct2D 1.1 output backend (Step 4 - infrastructure)
// ===========================================================================
//
// Provides a Direct2D device context + bitmap that can receive a 32 bpp BGRA
// DIB (the output of CJPEGImage::GetDIB()) and draw it to the screen via the
// GPU, avoiding the GDI BitBlt path. When unavailable or not requested, the
// caller falls back to the existing GDI BitBlt/AlphaBlend in PaintMemDCMgr.
//
// This class only owns the D2D device/context and a single staging bitmap.
// Integration into MainDlg::OnPaint is opt-in via JPEGVIEW_ENABLE_GPU_D2D;
// the default remains the GDI path so shipping behavior is unchanged.
// ===========================================================================

#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>

class CGpuRenderTarget {
public:
    static CGpuRenderTarget& Instance();
    ~CGpuRenderTarget();

    // True if the D2D device context was created successfully.
    bool IsAvailable() const { return m_pDCRT != nullptr; }

    // Draws a BGRA DIB (4 bytes/pixel, tightly packed) of (width,height) at
    // (dstX,dstY) on the bound DC. The DC is bound via BindDC. Returns false
    // if the D2D path is unavailable (caller should fall back to GDI).
    bool DrawBGRA(HDC hdc, int dstX, int dstY, int width, int height,
        const void* pBGRA, int srcWidth, int srcHeight);

private:
    CGpuRenderTarget();
    bool Init();

    ID2D1Factory1* m_pD2DFactory;
    ID2D1DCRenderTarget* m_pDCRT;
    // ID2D1DCRenderTarget binds to a GDI DC for drawing.
    ID2D1Bitmap* m_pBitmap;
    IDXGISurface* m_pSurface;
    int m_lastW, m_lastH;
};
