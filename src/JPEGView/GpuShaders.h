#pragma once

// ===========================================================================
// Runtime-compiled HLSL shaders for the GPU image processor.
//
// For Step 3a we keep the shader source inline and compile at runtime via
// D3DCompile. This avoids a separate fxc build step while the compute
// pipeline is still being validated. A later step can switch to build-time
// compiled .cso blobs embedded as byte arrays for a smaller, faster cold
// start (no D3DCompiler dependency at runtime).
// ===========================================================================

#include <d3d11.h>

namespace gpu_shaders {

// Apply3ChannelLUT32bpp: for each pixel, look up B/G/R channels in a
// 3*256-byte LUT (B block, then G block, then R block) and write BGRA with
// alpha forced to 0xFF. Input and output are R8G8B8A8_UNORM textures.
// CB0: nWidth, nHeight (uint row pitch is derived from texture width).
extern const char* kApply3ChannelLUT_CS;

// ApplyLDC32bpp (non-saturation path): applies a 3-channel LUT (768 uints:
// 256 B/G/R) and then a local-density-correction offset computed from a
// bilinearly interpolated mask and a per-luminance multiplier LUT.
//   t0: input R8G8B8A8_UINT texture
//   t1: pLUT (3*256 uints)
//   t2: pMulLUT (256 int32s) - density multiplier per LUT value
//   t3: pLDCMap (R8 texture, ldcMapSize) - density mask
//   u0: output R8G8B8A8_UINT texture
//   CB0: width, height, ldcMapSizeX, ldcMapSizeY
//   CB1: nIncrementX, nStartX, nIncrementY, nCurY0 (16.16 fixed point params)
extern const char* kApplyLDC32bpp_CS;

// ApplyLDC32bpp (saturation path): same as kApplyLDC32bpp_CS but applies the
// 6x1536-int32 saturation matrix before the LUT lookup. Mirrors the
// pSatLUTs != NULL branch of ApplyLDC32bpp_Core.
//   t3: pSatLUTs structured buffer (1536 int32: 256*R, 256*G, 256*B, then
//       offsets 768/1024/1280 for the second/third matrix rows - see CPU code)
extern const char* kApplyLDC32bppSat_CS;

// ResampleX: 1D FIR resample in X (bicubic kernel, float accumulation).
// Each target column i uses its own kernel (from a structured buffer of
// kernel descriptors). Input is R8G8B8A8_UINT; output is R8G8B8A8_UINT.
//   t0: input texture
//   t1: kernel offsets buffer (int per target column)
//   t2: kernel values buffer (float per tap, packed per column)
//   t3: kernel lengths buffer (int per target column)
//   u0: output texture
//   CB0: srcWidth, srcHeight, tgtWidth, tgtHeight
//   CB1: nStartX_FP (16.16), nChannels
// The Y pass reuses this shader with swapped dimensions (caller transposes).
extern const char* kResampleX_CS;

// UnsharpMask: per-pixel sharpening. Reads gray + smoothed-gray (int16),
// a threshold LUT (int16, 2048 entries, centered), and the source BGRA, then
// writes BGRA with sharpening applied. Mirrors UnsharpMask_Core in fixed point.
//   t0: source R8G8B8A8_UINT texture
//   t1: gray int16 buffer (structured int16)
//   t2: smoothed-gray int16 buffer
//   t3: threshold LUT int16 buffer (nNumEntriesPerSide*2 entries, center at nNumEntriesPerSide)
//   u0: output R8G8B8A8_UINT texture
//   CB0: width, height, amount12 (dAmount*(1<<12)+0.5), lutCenter
//   CB1: grayOffsetX, grayOffsetY, fullCx, nChannels
extern const char* kUnsharpMask_CS;

// GaussFilter1C16: separable 1D Gauss on an int16 single-channel image.
// Reuses the same kernel descriptor/value structured-buffer layout as the
// resample shader. Output is int16 (one channel). One pass (X or Y) at a
// time; caller runs twice for 2D.
//   t0: source int16 structured buffer
//   t1: kernel descriptors (int3 packed as int[]*3)
//   t2: kernel values (float)
//   u0: output int16 structured buffer (RWStructuredBuffer<int16_t>)
//   CB0: srcLen, tgtLen, srcStride, startX_FP
//   CB1: incX_FP, kernelFilterOffset, _p0, _p1
extern const char* kGaussFilter1C16_CS;

// GaussFilter1C16 Y pass: filters along rows (height). Same buffer layout as
// the X shader but reads source vertically. Used for the second separable pass.
extern const char* kGaussFilter1C16Y_CS;

// Compiles the given HLSL source as a compute shader. Returns a new
// ID3D11ComputeShader (caller releases) or nullptr on failure.
ID3D11ComputeShader* CompileComputeShader(ID3D11Device* device,
    const char* hlsl, const char* entryPoint, const char* target);

} // namespace gpu_shaders
