#pragma once

// intfp: 플랫폼별 고정소수점 타입 (x64=64비트, x86=32비트)
#include "ImageProcessingTypes.h"

class CXMMImage;
struct XMMFilterKernelBlock;

// Applies a 1D resize filter in the Y direction using google/highway, which
// runtime-dispatches between SSE2 (x86 fallback) and AVX2. The filter is
// applied to all stored planes of pSourceImg (3 for BGR, 4 for BGRA), so the
// alpha channel is resampled with the same high-quality kernel as RGB.
//
// nSourceHeight: Height of source image (only present to match the legacy signature).
// nTargetHeight: Height of target image after resampling.
// nWidth: Width of source image (number of pixels per row to filter).
// nStartY_FP: 16.16 fixed point Y-start subpixel coordinate.
// nStartX: Start of filtering in X (not fixed point).
// nIncrementY_FP: 16.16 fixed point Y increment.
// filter: Packed filter kernels (128-bit int16 format, widened at runtime).
// nFilterOffset: Offset into filter.Indices.
// pSourceImg: Source image (planar 16-bit).
// Returns a new CXMMImage owned by the caller.
namespace hwy_ext {
CXMMImage* ApplyFilter_Highway(int nSourceHeight, int nTargetHeight, int nWidth,
	int nStartY_FP, int nStartX, intfp nIncrementY_FP,
	const XMMFilterKernelBlock& filter,
	int nFilterOffset, const CXMMImage* pSourceImg);
}  // namespace hwy_ext
