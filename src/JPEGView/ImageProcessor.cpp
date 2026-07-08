#include "StdAfx.h"
#include "ImageProcessor.h"
#include "BasicProcessing.h"
#include "SettingsProvider.h"
#include "Helpers.h"
#include "GpuDevice.h"
#include "GpuImageProcessor.h"

// Activates the GPU backend when the EnableGPUImageProcessing INI setting is
// true (or the legacy JPEGVIEW_ENABLE_GPU env var is set), AND a usable FL
// 11_0 adapter is present. Any init failure leaves the CPU backend in place
// so the viewer always produces a valid image.
namespace {
bool IsGpuRequested() {
    if (CSettingsProvider::This().EnableGPUImageProcessing()) return true;
    // Legacy opt-in via environment variable (kept for quick A/B testing).
    char buf[8] = {0};
    DWORD n = GetEnvironmentVariableA("JPEGVIEW_ENABLE_GPU", buf, sizeof(buf));
    return n > 0 && buf[0] != '0';
}
} // namespace

namespace {

// Mirrors the SupportsSIMD() helper that previously lived in JPEGImage.cpp.
bool SupportsSIMD(Helpers::CPUType cpuType) {
    return cpuType == Helpers::CPU_SSE || cpuType == Helpers::CPU_AVX2;
}

CBasicProcessing::SIMDArchitecture ToSIMDArchitecture(Helpers::CPUType cpuType) {
    switch (cpuType) {
    case Helpers::CPU_SSE:  return CBasicProcessing::SSE;
    case Helpers::CPU_AVX2: return CBasicProcessing::AVX2;
    default:
        assert(false);
        return (CBasicProcessing::SIMDArchitecture)(-1);
    }
}

// Configured CPU preference, after applying the AVX2 freeze workaround that
// used to live inline in CJPEGImage::Resample. Kept here so the CPU backend
// reproduces the exact legacy routing, behavior bit-for-bit.
Helpers::CPUType ResolveCpuForResample(CSize clippedTargetSize) {
    Helpers::CPUType cpu = CSettingsProvider::This().AlgorithmImplementation();
#ifdef AVX_SSE_FREEZE_FALLBACK
    // Obscure AVX2 freeze on widths > ~3224px in release builds; fall back
    // to SSE. See the original comment in CJPEGImage::Resample for history.
    if (cpu == Helpers::CPU_AVX2 && clippedTargetSize.cx > 3200) {
        cpu = Helpers::CPU_SSE;
    }
#endif
    return cpu;
}

} // namespace

// ===========================================================================
// CPU backend: thin delegate over the existing CBasicProcessing static API.
// Step 1 wires this in as the default so behavior is identical to before.
// ===========================================================================

class CpuImageProcessor : public IImageProcessor {
public:
    const char* BackendName() const override { return "CPU"; }

    void* ResampleHQ(CSize fullTargetSize, CPoint fullTargetOffset,
        CSize clippedTargetSize, CSize sourceSize, const void* pPixels,
        int nChannels, double dSharpen, EFilterType eFilter,
        bool bUpsampling) override {
        // Reproduces the SIMD/scalar routing that CJPEGImage::Resample and
        // InternalResize did inline, including the AVX2 freeze fallback.
        Helpers::CPUType cpu = ResolveCpuForResample(clippedTargetSize);
        if (SupportsSIMD(cpu)) {
            if (bUpsampling) {
                return CBasicProcessing::SampleUp_HQ_SIMD(fullTargetSize, fullTargetOffset,
                    clippedTargetSize, sourceSize, pPixels, nChannels, ToSIMDArchitecture(cpu));
            }
            return CBasicProcessing::SampleDown_HQ_SIMD(fullTargetSize, fullTargetOffset,
                clippedTargetSize, sourceSize, pPixels, nChannels, dSharpen, eFilter,
                ToSIMDArchitecture(cpu));
        }
        if (bUpsampling) {
            return CBasicProcessing::SampleUp_HQ(fullTargetSize, fullTargetOffset,
                clippedTargetSize, sourceSize, pPixels, nChannels);
        }
        return CBasicProcessing::SampleDown_HQ(fullTargetSize, fullTargetOffset,
            clippedTargetSize, sourceSize, pPixels, nChannels, dSharpen, eFilter);
    }

    void* UnsharpMask(CSize fullSize, CPoint offset, CSize rect,
        double dAmount, double dThreshold, const int16* pGrayImage,
        const int16* pSmoothedGrayImage, const void* pSourcePixels,
        void* pTargetPixels, int nChannels) override {
        return CBasicProcessing::UnsharpMask(fullSize, offset, rect, dAmount,
            dThreshold, pGrayImage, pSmoothedGrayImage, pSourcePixels,
            pTargetPixels, nChannels);
    }

    int16* GaussFilter16bpp1Channel(CSize fullSize, CPoint offset, CSize rect,
        double dRadius, const int16* pPixels) override {
        return CBasicProcessing::GaussFilter16bpp1Channel(fullSize, offset, rect,
            dRadius, pPixels);
    }

    void* ApplyLDC32bpp(CSize fullTargetSize, CPoint fullTargetOffset,
        CSize clippedTargetSize, CSize ldcMapSize, const void* pDIBPixels,
        const int32* pSatLUTs, const uint8* pLUT, const uint8* pLDCMap,
        float fBlackPt, float fWhitePt, float fBlackPtSteepness) override {
        return CBasicProcessing::ApplyLDC32bpp(fullTargetSize, fullTargetOffset,
            clippedTargetSize, ldcMapSize, pDIBPixels, pSatLUTs, pLUT, pLDCMap,
            fBlackPt, fWhitePt, fBlackPtSteepness);
    }

    void* ApplySaturationAnd3ChannelLUT32bpp(int nWidth, int nHeight,
        const void* pDIBPixels, const int32* pSatLUTs, const uint8* pLUT) override {
        return CBasicProcessing::ApplySaturationAnd3ChannelLUT32bpp(nWidth, nHeight,
            pDIBPixels, pSatLUTs, pLUT);
    }

    void* Apply3ChannelLUT32bpp(int nWidth, int nHeight,
        const void* pDIBPixels, const uint8* pLUT) override {
        return CBasicProcessing::Apply3ChannelLUT32bpp(nWidth, nHeight,
            pDIBPixels, pLUT);
    }
};

// ---- Factory ------------------------------------------------------------

std::unique_ptr<IImageProcessor>& CImageProcessorFactory::ActiveBackend() {
    static std::unique_ptr<IImageProcessor> s_pBackend;
    return s_pBackend;
}

IImageProcessor& CImageProcessorFactory::Get() {
    auto& pSlot = ActiveBackend();
    if (!pSlot) {
        // GPU backend is opt-in: selected when EnableGPUImageProcessing is set
        // (or the legacy JPEGVIEW_ENABLE_GPU env var is present) AND a usable
        // FL 11_0 adapter initializes. Any failure leaves the CPU backend in
        // place so the viewer always renders a correct image.
        if (IsGpuRequested() && CGpuDevice::Instance().IsAvailable()) {
            pSlot = std::make_unique<GpuImageProcessor>();
        } else {
            pSlot = std::make_unique<CpuImageProcessor>();
        }
    }
    return *pSlot;
}

void CImageProcessorFactory::SetBackend(std::unique_ptr<IImageProcessor> pBackend) {
    ActiveBackend() = std::move(pBackend);
}

void CImageProcessorFactory::Shutdown() {
    ActiveBackend().reset();
}
