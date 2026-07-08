#pragma once

// ===========================================================================
// Image processor backend abstraction (Step 1: interface only, CPU default)
// ===========================================================================
//
// Why a backend layer now:
//   CBasicProcessing is a bag of static pixel routines. CJPEGImage calls them
//   directly at ~30 call sites. Inserting an IImageProcessor between the two
//   lets a future D3D11 compute backend swap in for the hot paths
//   (resampling, unsharp mask, LDC) without touching any caller.
//
// Migration rule for Step 1 (no behavior change):
//   - CBasicProcessing keeps its full static API and current implementation.
//   - CpuImageProcessor delegates every call straight back to CBasicProcessing.
//   - CBasicProcessing's own static methods are NOT yet rewritten to dispatch;
//     they stay as-is so callers that bypass the interface (legacy paths in
//     SaveImage, PrintImage, ThumbnailCache) keep working untouched.
//   - The single integration point is CJPEGImage: it holds an
//     IImageProcessor* and routes the hot-path calls through it. Everything
//     else keeps calling CBasicProcessing directly until later steps.
//
// Operation groups (drives which methods land on the interface):
//   A. Hot pixel transforms (GPU offload candidates): resample, unsharp,
//      gauss, LDC. These dominate cost on large images like a 238 MP
//      panorama and map well to compute shaders.
//   B. LUT-based correction (GPU candidate, bandwidth-bound): the 3-channel
//      LUT + saturation apply. Cheap per-pixel but touches every byte.
//   C. Format/copy/geometry primitives (CPU stays): converts, CopyRect,
//      Mirror, Crop, Rotate90, LUT creation. One-shot, tiny, or per-call
//      setup cost that would lose to upload overhead on a GPU.
// ===========================================================================

#include "ImageProcessingTypes.h"
#include "ProcessParams.h"

#include <memory>

class IImageProcessor {
public:
    virtual ~IImageProcessor() = default;

    // Identifies the backend for logging/fallback decisions.
    virtual const char* BackendName() const = 0;

    // ---- Group A: hot pixel transforms (GPU offload candidates) -----------

    // High-quality up/down resampling of a 32/24 bpp BGR(A) image.
    // bUpsampling selects the bicubic-up vs. sharpen-down kernel path.
    // Returns a 32 bpp BGRA DIB of clippedTargetSize, or NULL on failure.
    // Mirrors CBasicProcessing::Sample{Up,Down}_HQ[_SIMD].
    virtual void* ResampleHQ(CSize fullTargetSize, CPoint fullTargetOffset,
        CSize clippedTargetSize, CSize sourceSize, const void* pPixels,
        int nChannels, double dSharpen, EFilterType eFilter,
        bool bUpsampling) = 0;

    // Unsharp mask over a BGR(A) image, using precomputed gray + smoothed-gray
    // 16 bpp single-channel planes. Mirrors CBasicProcessing::UnsharpMask.
    virtual void* UnsharpMask(CSize fullSize, CPoint offset, CSize rect,
        double dAmount, double dThreshold, const int16* pGrayImage,
        const int16* pSmoothedGrayImage, const void* pSourcePixels,
        void* pTargetPixels, int nChannels) = 0;

    // Separable Gauss filter on a 16 bpp single-channel image.
    // Mirrors CBasicProcessing::GaussFilter16bpp1Channel.
    virtual int16* GaussFilter16bpp1Channel(CSize fullSize, CPoint offset,
        CSize rect, double dRadius, const int16* pPixels) = 0;

    // Local density correction + 3-channel LUT, 32 bpp BGRA. This is the
    // shadow/highlight + contrast/brightness/gamma + saturation pass that
    // runs on the resampled DIB. Mirrors CBasicProcessing::ApplyLDC32bpp.
    virtual void* ApplyLDC32bpp(CSize fullTargetSize, CPoint fullTargetOffset,
        CSize clippedTargetSize, CSize ldcMapSize, const void* pDIBPixels,
        const int32* pSatLUTs, const uint8* pLUT, const uint8* pLDCMap,
        float fBlackPt, float fWhitePt, float fBlackPtSteepness) = 0;

    // ---- Group B: LUT correction (GPU candidate, bandwidth-bound) --------

    // Saturation matrix + 3-channel LUT applied to a 32 bpp BGRA DIB.
    // Either pointer may combine with the LDC path on GPU.
    // Mirrors CBasicProcessing::ApplySaturationAnd3ChannelLUT32bpp.
    virtual void* ApplySaturationAnd3ChannelLUT32bpp(int nWidth, int nHeight,
        const void* pDIBPixels, const int32* pSatLUTs, const uint8* pLUT) = 0;

    // 3-channel LUT only (no saturation), 32 bpp BGRA. This is the lighter
    // sibling of ApplySaturationAnd3ChannelLUT32bpp; a GPU backend can route
    // both through one shader with the saturation buffer bound as NULL.
    // Mirrors CBasicProcessing::Apply3ChannelLUT32bpp.
    virtual void* Apply3ChannelLUT32bpp(int nWidth, int nHeight,
        const void* pDIBPixels, const uint8* pLUT) = 0;
};

// ---- Backend selection --------------------------------------------------
// Process-wide singleton. CPU backend is always available and is the
// default. A later step will add a GPU backend that replaces this when a
// D3D11 device initializes successfully.
class CImageProcessorFactory {
public:
    // Returns the active processor. Lazily initialized on first call; the
    // initial selection is CPU-only so Step 1 has zero behavior change.
    static IImageProcessor& Get();

    // Forced selection, used by tests and by the future GPU-init path to
    // override the lazy default once a device is ready.
    static void SetBackend(std::unique_ptr<IImageProcessor> pBackend);

    // Releases the active backend. Call at process shutdown (optional; the
    // singleton leak is acceptable for a viewer process).
    static void Shutdown();

private:
    // Owns the process-wide singleton slot. Defined in ImageProcessor.cpp.
    static std::unique_ptr<IImageProcessor>& ActiveBackend();
};
