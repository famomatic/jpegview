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
// Integration into PaintMemDCMgr is gated by the EnableGPUImageProcessing INI
// setting (the same one that selects the compute backend), so enabling GPU
// processing enables both the compute shaders and the D2D screen blit. The
// legacy JPEGVIEW_ENABLE_GPU_D2D env var remains as an override for testing.
// The default (EnableGPUImageProcessing=false) keeps the GDI path.
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

    // Draws a BGRA source region of (width,height) pixels at (dstX,dstY) on
    // the bound DC. srcWidth/srcHeight are the source region dimensions and
    // srcStride is the byte stride of one full source row. The source may be a
    // sub-rectangle of a larger DIB, in which case srcStride > srcWidth*4;
    // passing the real row stride is what keeps each row aligned (a tightly-
    // packed assumption here shifts every row and paints a diagonal band).
    // Returns false if the D2D path is unavailable (caller falls back to GDI).
    bool DrawBGRA(HDC hdc, int dstX, int dstY, int width, int height,
        const void* pBGRA, int srcWidth, int srcHeight, int srcStride);

private:
    CGpuRenderTarget();
    bool Init();

    ID2D1Factory1* m_pD2DFactory;
    ID2D1DCRenderTarget* m_pDCRT;
    // ID2D1DCRenderTarget binds to a GDI DC for drawing.
    ID2D1Bitmap* m_pBitmap;
    IDXGISurface* m_pSurface;
    int m_lastW, m_lastH, m_lastStride;
};
