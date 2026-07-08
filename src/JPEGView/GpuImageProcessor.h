#pragma once

// ===========================================================================
// GPU image processor backend (Step 2 skeleton)
// ===========================================================================
//
// GpuImageProcessor owns (via CGpuDevice) a D3D11 device and implements the
// IImageProcessor interface. In Step 2 every method still routes to the CPU
// backend (CBasicProcessing) - the device is created and verified, but no
// compute shader runs yet. This isolates "can we get a device?" from "can we
// run a compute pass?", so a failure in adapter enumeration degrades cleanly
// to the existing CPU path without touching pixel output.
//
// Step 3 replaces the hot methods (ResampleHQ, UnsharpMask, ApplyLDC32bpp)
// one at a time with D3D11 compute implementations, keeping the rest on CPU.

#include "ImageProcessor.h"
#include <d3d11.h>

// uintfp: 플랫폼별 고정소수점 타입 (ImageProcessingTypes.h, x64=64비트/x86=32비트)
struct FilterKernelBlock;

class GpuImageProcessor : public IImageProcessor {
public:
    // Constructs the processor. If no FL 11_0 adapter is available the device
    // stays NULL and every call transparently falls back to CPU.
    GpuImageProcessor();
    ~GpuImageProcessor() override;

    const char* BackendName() const override {
        return m_deviceAvailable ? "GPU" : "GPU(unavailable->CPU)";
    }

    // IImageProcessor. Each routes to CPU until its GPU pass is implemented.
    void* ResampleHQ(CSize fullTargetSize, CPoint fullTargetOffset,
        CSize clippedTargetSize, CSize sourceSize, const void* pPixels,
        int nChannels, double dSharpen, EFilterType eFilter,
        bool bUpsampling) override;

    void* UnsharpMask(CSize fullSize, CPoint offset, CSize rect,
        double dAmount, double dThreshold, const int16* pGrayImage,
        const int16* pSmoothedGrayImage, const void* pSourcePixels,
        void* pTargetPixels, int nChannels) override;

    int16* GaussFilter16bpp1Channel(CSize fullSize, CPoint offset, CSize rect,
        double dRadius, const int16* pPixels) override;

    void* ApplyLDC32bpp(CSize fullTargetSize, CPoint fullTargetOffset,
        CSize clippedTargetSize, CSize ldcMapSize, const void* pDIBPixels,
        const int32* pSatLUTs, const uint8* pLUT, const uint8* pLDCMap,
        float fBlackPt, float fWhitePt, float fBlackPtSteepness) override;

    void* ApplySaturationAnd3ChannelLUT32bpp(int nWidth, int nHeight,
        const void* pDIBPixels, const int32* pSatLUTs, const uint8* pLUT) override;

    void* Apply3ChannelLUT32bpp(int nWidth, int nHeight,
        const void* pDIBPixels, const uint8* pLUT) override;

private:
    bool m_deviceAvailable;
    // Lazily-compiled compute shaders. Released in the dtor.
    ID3D11ComputeShader* m_pApply3ChannelLUT_CS;
    ID3D11ComputeShader* m_pApplyLDC32bpp_CS;
    ID3D11ComputeShader* m_pApplyLDC32bppSat_CS;
    ID3D11ComputeShader* m_pApplySaturationAnd3ChannelLUT_CS;
    ID3D11ComputeShader* m_pResampleX_CS;
    ID3D11ComputeShader* m_pUnsharpMask_CS;
    ID3D11ComputeShader* m_pGaussFilter1C16_CS;
    ID3D11ComputeShader* m_pGaussFilter1C16Y_CS;

    ID3D11ComputeShader* GetApply3ChannelLUTShader();
    ID3D11ComputeShader* GetApplyLDC32bppShader();
    ID3D11ComputeShader* GetApplyLDC32bppSatShader();
    ID3D11ComputeShader* GetApplySaturationAnd3ChannelLUTShader();
    ID3D11ComputeShader* GetResampleXShader();
    ID3D11ComputeShader* GetUnsharpMaskShader();
    ID3D11ComputeShader* GetGaussFilter1C16Shader();
    ID3D11ComputeShader* GetGaussFilter1C16YShader();

    // Runs one separable FIR pass. Returns false on failure (caller falls
    // back to CPU). X pass: src(tx=x,ty=row) -> out(tx=col,ty=row).
    bool RunResamplePass(ID3D11DeviceContext* ctx, ID3D11Device* device,
        ID3D11ComputeShader* cs, ID3D11Texture2D* texSrc, ID3D11Texture2D* texOut,
        const FilterKernelBlock& kernels, int tgtW, int tgtH,
        int srcW, int srcH, uintfp startX_FP, uintfp incX_FP);
    // Y pass: filters along the "row" dimension. Reads texX as transposed.
    bool RunResamplePassY(ID3D11DeviceContext* ctx, ID3D11Device* device,
        ID3D11ComputeShader* cs, ID3D11Texture2D* texSrc, ID3D11Texture2D* texOut,
        const FilterKernelBlock& kernels, int tgtW, int tgtH,
        int srcW, int srcH, uintfp startX_FP, uintfp incX_FP);

    // Runs one separable Gauss 1D pass on a single-channel int image via
    // StructuredBuffer<int> source and RWStructuredBuffer<int> output. The
    // output is written transposed (column i -> nTargetWidth*i + row j),
    // exactly like ApplyFilter1C16bpp, so two passes restore orientation.
    // On success returns a heap int[] of size nTargetWidth*nRunXCount holding
    // the transposed result; on failure returns nullptr (caller falls back).
    //   srcLen   - source width for tap clamping (== nSourceWidth)
    //   srcStride- source row pitch (elements per row)
    //   nRunX    - number of target columns filtered (== kernel count)
    //   nTargetWidth - transposed output pitch
    //   rowCount - number of source rows processed
    //   startX   - first target column index into the kernel block
    int* RunGaussPass(ID3D11DeviceContext* ctx, ID3D11Device* device,
        ID3D11ComputeShader* cs, const int* pSrc, int srcLen, int srcStride,
        int nRunX, int nTargetWidth, int rowCount, int startX,
        const FilterKernelBlock& kernels);
};
