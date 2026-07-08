#include "GpuShaders.h"
#include <d3dcompiler.h>
#include <cstdio>

// Standalone-safe Windows headers (see GpuDevice.cpp for rationale).
#ifndef _WINDOWS_
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace gpu_shaders {

// Apply3ChannelLUT32bpp compute shader.
//
// The LUT is provided as a Buffer<uint> with 3*256 elements: 256 B, then
// 256 G, then 256 R, each channel value stored in a uint (only the low byte
// matters). This avoids ByteAddressBuffer's 4-byte alignment restriction and
// lets the shader index directly by channel value. The host expands the
// 768-byte LUT to 768 uints before upload.
//
// Input/output textures are R8G8B8A8_UINT so the shader works in raw 0..255
// byte values, byte-exact with the CPU path (no float normalization).
const char* kApply3ChannelLUT_CS = R"(
struct Constants {
    uint width;
    uint height;
    uint _pad0;
    uint _pad1;
};

cbuffer CB0 : register(b0) { Constants g; };

Texture2D<uint4>   gInput : register(t0);
Buffer<uint>      gLUT   : register(t1);
RWTexture2D<uint4> gOutput : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= g.width || dtid.y >= g.height) return;
    uint4 bgra = gInput[dtid.xy];
    uint b = bgra.x & 0xFF;
    uint gr = bgra.y & 0xFF;
    uint r = bgra.z & 0xFF;
    uint nb = gLUT[b];        // B block: indices 0..255
    uint ng = gLUT[256 + gr]; // G block: indices 256..511
    uint nr = gLUT[512 + r];  // R block: indices 512..767
    gOutput[dtid.xy] = uint4(nb, ng, nr, 0xFFu);
}
)";

// ApplyLDC32bpp (non-saturation path). Mirrors ApplyLDC32bpp_Core in
// BasicProcessing.cpp for the pSatLUTs == NULL case, bit-for-bit in 16.16
// fixed point: LUT lookup, then maskValue * pMulLUT[lutValue] >> 14 added.
const char* kApplyLDC32bpp_CS = R"(
struct Params { uint width; uint height; uint mapW; uint mapH; };
struct Fxp { uint incX; uint startX; uint incY; uint curY0; };
cbuffer CB0 : register(b0) { Params g; };
cbuffer CB1 : register(b1) { Fxp f; };

Texture2D<uint4>    gInput   : register(t0);
Buffer<uint>       gLUT     : register(t1);
Buffer<int>        gMulLUT  : register(t2);
Texture2D<uint>    gLDCMap  : register(t3);
RWTexture2D<uint4> gOutput  : register(u0);

int clamp255(int v) { return max(0, min(255, v)); }

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= g.width || dtid.y >= g.height) return;

    uint curY = f.curY0 + dtid.y * f.incY;
    uint curX = f.startX + dtid.x * f.incX;
    uint curYTrunc = curY >> 16;
    uint curYFrac = curY & 0xFFFFu;
    uint curXTrunc = curX >> 16;
    uint curXFrac = curX & 0xFFFFu;

    uint mTL = gLDCMap.Load(int3(curXTrunc,     curYTrunc,     0)).r;
    uint mTR = gLDCMap.Load(int3(curXTrunc + 1, curYTrunc,     0)).r;
    uint mBL = gLDCMap.Load(int3(curXTrunc,     curYTrunc + 1, 0)).r;
    uint mBR = gLDCMap.Load(int3(curXTrunc + 1, curYTrunc + 1, 0)).r;
    int nLeft  = (int)((int(curYFrac) * int(mBL - mTL)) >> 16) + int(mTL);
    int nRight = (int)((int(curYFrac) * int(mBR - mTR)) >> 16) + int(mTR);
    int nMaskValue = (int)((int(curXFrac) * (nRight - nLeft)) >> 16) + nLeft - 127;

    uint4 bgra = gInput[dtid.xy];
    uint b = bgra.x & 0xFF;
    uint gr = bgra.y & 0xFF;
    uint r = bgra.z & 0xFF;

    uint nb = gLUT[b];
    uint ng = gLUT[256 + gr];
    uint nr = gLUT[512 + r];

    int oB = int(nb) + ((nMaskValue * gMulLUT[nb]) >> 14);
    int oG = int(ng) + ((nMaskValue * gMulLUT[ng]) >> 14);
    int oR = int(nr) + ((nMaskValue * gMulLUT[nr]) >> 14);

    gOutput[dtid.xy] = uint4(clamp255(oB), clamp255(oG), clamp255(oR), 0xFFu);
}
)";

// ApplyLDC32bpp saturation path. Mirrors ApplyLDC32bpp_Core for pSatLUTs != NULL.
// Applies the 6-row saturation matrix to (R,G,B), clamps to 16.16 fixed point
// (cnMax = 255<<16), >>16 for LUT index, then the LUT + density multiply.
//   CPU: nRed   = satLUTs[nSrcRed] + satLUTs[256+nSrcGreen] + satLUTs[512+nSrcBlue]
//        nGreen = satLUTs[768+nSrcRed] + satLUTs[1024+nSrcGreen] + satLUTs[512+nSrcBlue]
//        nBlue  = satLUTs[768+nSrcRed] + satLUTs[256+nSrcGreen] + satLUTs[1280+nSrcBlue]
//   then nBlue = pLUT[(clamp(nBlue,0,cnMax)>>16)] + (maskValue*pMulLUT[nBlue]>>14)
const char* kApplyLDC32bppSat_CS = R"(
struct Params { uint width; uint height; uint mapW; uint mapH; };
struct Fxp { uint incX; uint startX; uint incY; uint curY0; };
cbuffer CB0 : register(b0) { Params g; };
cbuffer CB1 : register(b1) { Fxp f; };

Texture2D<uint4>    gInput   : register(t0);
Buffer<uint>       gLUT     : register(t1);
Buffer<int>        gMulLUT  : register(t2);
Texture2D<uint>    gLDCMap  : register(t3);
StructuredBuffer<int> gSat  : register(t4);
RWTexture2D<uint4> gOutput  : register(u0);

int clamp255(int v) { return max(0, min(255, v)); }

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= g.width || dtid.y >= g.height) return;

    uint curY = f.curY0 + dtid.y * f.incY;
    uint curX = f.startX + dtid.x * f.incX;
    uint curYTrunc = curY >> 16;
    uint curYFrac = curY & 0xFFFFu;
    uint curXTrunc = curX >> 16;
    uint curXFrac = curX & 0xFFFFu;

    uint mTL = gLDCMap.Load(int3(curXTrunc,     curYTrunc,     0)).r;
    uint mTR = gLDCMap.Load(int3(curXTrunc + 1, curYTrunc,     0)).r;
    uint mBL = gLDCMap.Load(int3(curXTrunc,     curYTrunc + 1, 0)).r;
    uint mBR = gLDCMap.Load(int3(curXTrunc + 1, curYTrunc + 1, 0)).r;
    int nLeft  = (int)((int(curYFrac) * int(mBL - mTL)) >> 16) + int(mTL);
    int nRight = (int)((int(curYFrac) * int(mBR - mTR)) >> 16) + int(mTR);
    int nMaskValue = (int)((int(curXFrac) * (nRight - nLeft)) >> 16) + nLeft - 127;

    uint4 bgra = gInput[dtid.xy];
    int sb = int(bgra.x & 0xFF);
    int sg = int(bgra.y & 0xFF);
    int sr = int(bgra.z & 0xFF);

    // Saturation matrix (6 rows of 256). cnMax = 255<<16.
    int cnMax = 255 << 16;
    int nRed   = gSat[sr] + gSat[256 + sg] + gSat[512 + sb];
    int nGreen = gSat[768 + sr] + gSat[1024 + sg] + gSat[512 + sb];
    int nBlue  = gSat[768 + sr] + gSat[256 + sg] + gSat[1280 + sb];

    nBlue = int(gLUT[max(0, min(cnMax, nBlue)) >> 16]);
    nGreen = int(gLUT[(max(0, min(cnMax, nGreen)) >> 16) + 256]);
    nRed = int(gLUT[(max(0, min(cnMax, nRed)) >> 16) + 512]);

    int oB = nBlue + ((nMaskValue * gMulLUT[nBlue]) >> 14);
    int oG = nGreen + ((nMaskValue * gMulLUT[nGreen]) >> 14);
    int oR = nRed + ((nMaskValue * gMulLUT[nRed]) >> 14);

    gOutput[dtid.xy] = uint4(clamp255(oB), clamp255(oG), clamp255(oR), 0xFFu);
}
)";

// ResampleX: 1D FIR resample in the X direction using a per-target-column
// kernel. Float accumulation (the CPU path uses 2.14 fixed point; the GPU
// path trades bit-exactness for throughput, validated by PSNR in Step 5).
//
// Each target column i has its own kernel: an offset (signed, subtracted
// from the integer source position to get the tap start), a length, and a
// packed float[] of tap weights (summing to ~1.0). The kernel data is laid
// out as one big float[] buffer with an int[] of (offset, length, base)
// triples per target column.
//
// t0: input R8G8B8A8_UINT texture
// t1: kernel descriptors: int3 per target column (offset, length, valueBase)
// t2: kernel values: float[], indexed by descriptor.valueBase + tap
// u0: output R8G8B8A8_UINT texture
// CB0: srcW, srcH, tgtW, tgtH
// CB1: startX_FP (16.16 fixed point source X for target column 0),
//      incX_FP (16.16 increment per target column)
const char* kResampleX_CS = R"(
struct Params { uint srcW; uint srcH; uint tgtW; uint tgtH; };
struct Fxp { uint startX; uint incX; uint _p0; uint _p1; };
cbuffer CB0 : register(b0) { Params g; };
cbuffer CB1 : register(b1) { Fxp f; };

StructuredBuffer<int> gKDesc : register(t1);
StructuredBuffer<float> gKVal  : register(t2);
Texture2D<uint4>    gInput   : register(t0);
RWTexture2D<uint4> gOutput  : register(u0);

float clamp255(float v) { return max(0.0f, min(255.0f, v)); }

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    // dtid.x = target column, dtid.y = source row (unchanged in X pass)
    if (dtid.x >= g.tgtW || dtid.y >= g.srcH) return;

    // Source X position for this target column (16.16 fixed point).
    uint xFP = f.startX + dtid.x * f.incX;
    int xInt = (int)(xFP >> 16);

    int kOffset = gKDesc[dtid.x * 3 + 0];
    int kLength = gKDesc[dtid.x * 3 + 1];
    int kValueBase = gKDesc[dtid.x * 3 + 2];
    int tapStart = xInt - kOffset;

    float sumB = 0, sumG = 0, sumR = 0;
    [loop]
    for (int n = 0; n < kLength; n++) {
        float w = gKVal[kValueBase + n];
        // Clamp source index to [0, srcW-1] (border handling).
        int sx = clamp(tapStart + n, 0, (int)g.srcW - 1);
        uint4 px = gInput[uint2(sx, dtid.y)];
        sumB += w * float(px.x);
        sumG += w * float(px.y);
        sumR += w * float(px.z);
    }

    // Round and clamp to 0..255. The CPU adds 255 (0.5 in 2.14) before >>14.
    uint b = (uint)clamp255(round(sumB));
    uint gr = (uint)clamp255(round(sumG));
    uint r = (uint)clamp255(round(sumR));
    gOutput[dtid.xy] = uint4(b, gr, r, 0xFFu);
}
)";

// UnsharpMask: per-pixel sharpening, fixed-point faithful to UnsharpMask_Core.
//   nDiff = thresholdLUT[(gray - smoothed) >> 4]
//   nSharpen = (nDiff * nAmount) >> 18
//   out = src + (nSharpen * src) >> 8, clamped 0..255
const char* kUnsharpMask_CS = R"(
struct Params { uint width; uint height; int amount12; int lutCenter; };
struct Geo { int offX; int offY; int fullCx; int nChannels; };
cbuffer CB0 : register(b0) { Params g; };
cbuffer CB1 : register(b1) { Geo f; };

Texture2D<uint4>    gInput   : register(t0);
StructuredBuffer<int> gGray    : register(t1);
StructuredBuffer<int> gSmooth  : register(t2);
StructuredBuffer<int> gThresh  : register(t3);
RWTexture2D<uint4> gOutput  : register(u0);

int clampi(int v, int lo, int hi) { return max(lo, min(hi, v)); }

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= g.width || dtid.y >= g.height) return;

    // Gray index: offset.x + (offset.y + y) * fullCx + x
    int grayIdx = f.offX + (f.offY + (int)dtid.y) * f.fullCx + (int)dtid.x;
    int diff = (int)gGray[grayIdx] - (int)gSmooth[grayIdx];
    // >> 4 (arithmetic shift on signed), then index into threshold LUT.
    int diffShifted = diff >> 4;
    int lutIdx = g.lutCenter + diffShifted;
    lutIdx = clamp(lutIdx, 0, (int)g.lutCenter * 2 - 1);
    int nDiff = (int)gThresh[lutIdx];
    int nSharpen = (nDiff * g.amount12) >> 18;

    uint4 px = gInput[dtid.xy];
    int b = (int)px.x;
    int gr = (int)px.y;
    int r = (int)px.z;
    b = b + ((nSharpen * b) >> 8);
    gr = gr + ((nSharpen * gr) >> 8);
    r = r + ((nSharpen * r) >> 8);

    gOutput[dtid.xy] = uint4(clampi(b,0,255), clampi(gr,0,255), clampi(r,0,255), 0xFFu);
}
)";
// GaussFilter1C16 X pass: 1D FIR over source width. Mirrors
// CBasicProcessing::ApplyFilter1C16bpp exactly, including the transposed
// output layout. dtid.x iterates target columns [0, nRunX); dtid.y iterates
// source rows [0, rowCount). The output is transposed: column dtid.x, source
// row dtid.y is written to gOut[dtid.x * nTargetWidth + dtid.y]. Two such
// passes (X then Y) rotate by 90 degrees each time, so the pair restores the
// original orientation - the same trick the CPU path uses.
//   CB0: srcLen, nRunX, srcStride, nStartX, nTargetWidth
//   CB1: incX(ignored), rowCount, nStartY, _p1
const char* kGaussFilter1C16_CS = R"(
struct Params { uint srcLen; uint nRunX; int srcStride; int startX; uint nTargetWidth; };
struct Fxp { uint incX; int rowCount; uint startY; uint _p1; };
cbuffer CB0 : register(b0) { Params g; };
cbuffer CB1 : register(b1) { Fxp f; };

StructuredBuffer<int> gSrc    : register(t0);
StructuredBuffer<int> gKDesc  : register(t1);
StructuredBuffer<int> gKVal   : register(t2);
RWStructuredBuffer<int> gOut  : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= g.nRunX) return;
    if (dtid.y >= (uint)f.rowCount) return;
    // CPU: pSourcePixelLine = pSource + nStartX + nSourceWidth*(j+nStartY);
    //      pSourcePixel = pSourcePixelLine + i - kOffset;
    int kOffset = gKDesc[dtid.x * 3 + 0];
    int kLength = gKDesc[dtid.x * 3 + 1];
    int kValueBase = gKDesc[dtid.x * 3 + 2];
    int tapStart = ((int)g.startX + (int)dtid.x) - kOffset;
    int sum = 0;
    [loop]
    for (int n = 0; n < kLength; n++) {
        int w = gKVal[kValueBase + n];
        int sx = tapStart + n;
        sx = clamp(sx, 0, (int)g.srcLen - 1);
        sum += w * gSrc[((int)dtid.y + (int)f.startY) * g.srcStride + sx];
    }
    // Transposed output: out[col * nTargetWidth + row].
    gOut[(int)dtid.x * (int)g.nTargetWidth + (int)dtid.y] = sum >> 14;
}
)";

// GaussFilter1C16 Y pass: filters along the height dimension. Same shader as
// the X pass but the source is the (already transposed) X result, so the
// "width" it filters is the original height and the kernel set is indexed by
// that dimension. A second transposed output restores the original layout.
const char* kGaussFilter1C16Y_CS = R"(
struct Params { uint srcLen; uint nRunX; int srcStride; int startX; uint nTargetWidth; };
struct Fxp { uint incX; int rowCount; uint startY; uint _p1; };
cbuffer CB0 : register(b0) { Params g; };
cbuffer CB1 : register(b1) { Fxp f; };

StructuredBuffer<int> gSrc    : register(t0);
StructuredBuffer<int> gKDesc  : register(t1);
StructuredBuffer<int> gKVal   : register(t2);
RWStructuredBuffer<int> gOut  : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= g.nRunX) return;
    if (dtid.y >= (uint)f.rowCount) return;
    int kOffset = gKDesc[dtid.x * 3 + 0];
    int kLength = gKDesc[dtid.x * 3 + 1];
    int kValueBase = gKDesc[dtid.x * 3 + 2];
    int tapStart = ((int)g.startX + (int)dtid.x) - kOffset;
    int sum = 0;
    [loop]
    for (int n = 0; n < kLength; n++) {
        int w = gKVal[kValueBase + n];
        int sx = clamp(tapStart + n, 0, (int)g.srcLen - 1);
        sum += w * gSrc[((int)dtid.y + (int)f.startY) * g.srcStride + sx];
    }
    gOut[(int)dtid.x * (int)g.nTargetWidth + (int)dtid.y] = sum >> 14;
}
)";


ID3D11ComputeShader* CompileComputeShader(ID3D11Device* device,
    const char* hlsl, const char* entryPoint, const char* target) {
    if (device == nullptr || hlsl == nullptr) return nullptr;

    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr,
        entryPoint, target, flags, 0, &code, &errors);

    if (FAILED(hr)) {
        if (errors) {
            const char* msg = (const char*)errors->GetBufferPointer();
            char buf[512];
            _snprintf(buf, sizeof(buf) - 1, "D3DCompile failed: %s\n", msg);
            OutputDebugStringA(buf);
            errors->Release();
        }
        if (code) code->Release();
        return nullptr;
    }

    ID3D11ComputeShader* shader = nullptr;
    hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
        nullptr, &shader);
    code->Release();
    if (FAILED(hr)) {
        return nullptr;
    }
    return shader;
}

} // namespace gpu_shaders
