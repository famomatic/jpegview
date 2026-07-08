// Highway-backed 1D FIR resampling filter. Replaces the three legacy SSE/MMX/AVX
// implementations with a single source compiled once; google/highway selects the
// best ISA (SSE2 on x86, AVX2 on x64 with runtime detection) at call time.
//
// The fixed-point arithmetic is preserved bit-for-bit: each source pixel value
// is shifted left by 1, multiplied by the 2.14 kernel via MulHigh, shifted left
// by 1 again, and accumulated with saturation (SaturatedAdd == _mm_adds_epi16),
// then clamped to [0, 16383-42]. This matches the original intrinsic sequence.
//
// The packed kernel format stores each tap repeated 8 times (128-bit int16). On
// AVX2 (16 lanes) LoadDup128 broadcasts that 128-bit element to both halves of
// the 256-bit vector, so a single kernel tap feeds all lanes uniformly.

#include "StdAfx.h"
#include "ApplyFilterHighway.h"
#include "XMMImage.h"
#include "ResizeFilter.h"

// MUST come before <hwy/highway.h>: tells Highway to re-include this very
// translation unit once per enabled target so HWY_DYNAMIC_DISPATCH can pick the
// best at runtime.
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "ApplyFilterHighway.cpp"
#include <hwy/foreach_target.h>  // NOLINT

#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace hwy_ext {
namespace HWY_NAMESPACE {

static const int16 FP_ONE_MINUS_ROUND = 16383 - 42;

CXMMImage* ApplyFilterKernel(int /*nSourceHeight*/, int nTargetHeight, int nWidth,
	int nStartY_FP, int nStartX, intfp nIncrementY_FP,
	const XMMFilterKernelBlock& filter,
	int nFilterOffset, const CXMMImage* pSourceImg) {

	const hwy::HWY_NAMESPACE::ScalableTag<int16> d;
	const size_t N = HWY_LANES(int16);

	const int nChannels = pSourceImg->GetNumChannels();

	int nStartXAligned = nStartX & ~static_cast<int>((N - 1));
	int nEndXAligned = (nStartX + nWidth + static_cast<int>(N - 1)) & ~static_cast<int>((N - 1));
	// NOTE: CXMMImage has a 4-arg overload CXMMImage(w,h,bool bPadHeight,int padding); passing the
	// channel count as the 3rd arg would be silently coerced to bool and allocate only 3 planes,
	// so the 4-plane (alpha) write below runs off the end of the buffer. Use the explicit
	// (width,height,nChannels,bPadHeight,padding) ctor with the same padding the legacy code used.
	CXMMImage* tempImage = new CXMMImage(nEndXAligned - nStartXAligned, nTargetHeight, nChannels, false, static_cast<int>(N));
	if (tempImage->AlignedPtr() == NULL) {
		delete tempImage;
		return NULL;
	}

	int nCurY = nStartY_FP;
	int nChannelLenBytes = pSourceImg->GetPaddedWidth() * sizeof(int16);
	int nRowLenBytes = nChannelLenBytes * nChannels;
	int nNumberOfBlocksX = (nEndXAligned - nStartXAligned) >> static_cast<int>(hwy::FloorLog2(N));
	const uint8* pSourceStart = (const uint8*)pSourceImg->AlignedPtr() + nStartXAligned * sizeof(int16);
	XMMFilterKernel** pKernelIndexStart = filter.Indices;

	const hwy::HWY_NAMESPACE::Vec<decltype(d)> vOne = Set(d, FP_ONE_MINUS_ROUND);
	const hwy::HWY_NAMESPACE::Vec<decltype(d)> vZero = Zero(d);
	int16* pDest = (int16*)tempImage->AlignedPtr();

	for (int y = 0; y < nTargetHeight; y++) {
		uint32 nCurYInt = (uint32)(nCurY >> 16);
		int filterIndex = y + nFilterOffset;
		XMMFilterKernel* pKernel = pKernelIndexStart[filterIndex];
		int filterLen = pKernel->FilterLen;
		int filterOffset = pKernel->FilterOffset;
		const uint8* pFilterStart = (const uint8*)&(pKernel->Kernel);
		const uint8* pSourceRow = pSourceStart + ((int)nCurYInt - filterOffset) * nRowLenBytes;

		for (int x = 0; x < nNumberOfBlocksX; x++) {
			const uint8* pSource = pSourceRow;
			const uint8* pFilter = pFilterStart;

			hwy::HWY_NAMESPACE::Vec<decltype(d)> vSum0 = Zero(d);
			hwy::HWY_NAMESPACE::Vec<decltype(d)> vSum1 = Zero(d);
			hwy::HWY_NAMESPACE::Vec<decltype(d)> vSum2 = Zero(d);
			hwy::HWY_NAMESPACE::Vec<decltype(d)> vSum3 = Zero(d);

			for (int i = 0; i < filterLen; i++) {
				const hwy::HWY_NAMESPACE::Vec<decltype(d)> vFilter = LoadDup128(d, (const int16*)(pFilter + i * 16));

				hwy::HWY_NAMESPACE::Vec<decltype(d)> v0 = Load(d, (const int16*)pSource);
				v0 = ShiftLeft<1>(v0);
				v0 = MulHigh(v0, vFilter);
				v0 = ShiftLeft<1>(v0);
				v0 = SaturatedAdd(v0, vSum0);
				vSum0 = v0;
				pSource += nChannelLenBytes;

				hwy::HWY_NAMESPACE::Vec<decltype(d)> v1 = Load(d, (const int16*)pSource);
				v1 = ShiftLeft<1>(v1);
				v1 = MulHigh(v1, vFilter);
				v1 = ShiftLeft<1>(v1);
				v1 = SaturatedAdd(v1, vSum1);
				vSum1 = v1;
				pSource += nChannelLenBytes;

				hwy::HWY_NAMESPACE::Vec<decltype(d)> v2 = Load(d, (const int16*)pSource);
				v2 = ShiftLeft<1>(v2);
				v2 = MulHigh(v2, vFilter);
				v2 = ShiftLeft<1>(v2);
				v2 = SaturatedAdd(v2, vSum2);
				vSum2 = v2;
				pSource += nChannelLenBytes;

				if (nChannels == 4) {
					hwy::HWY_NAMESPACE::Vec<decltype(d)> v3 = Load(d, (const int16*)pSource);
					v3 = ShiftLeft<1>(v3);
					v3 = MulHigh(v3, vFilter);
					v3 = ShiftLeft<1>(v3);
					v3 = SaturatedAdd(v3, vSum3);
					vSum3 = v3;
					pSource += nChannelLenBytes;
				}
			}

			vSum0 = Min(Max(vSum0, vZero), vOne);
			vSum1 = Min(Max(vSum1, vZero), vOne);
			vSum2 = Min(Max(vSum2, vZero), vOne);

			Store(vSum0, d, pDest);
			pDest += N;
			Store(vSum1, d, pDest);
			pDest += N;
			Store(vSum2, d, pDest);
			pDest += N;

			if (nChannels == 4) {
				vSum3 = Min(Max(vSum3, vZero), vOne);
				Store(vSum3, d, pDest);
				pDest += N;
			}

			pSourceRow += N * sizeof(int16);
		}

		nCurY += nIncrementY_FP;
	};

	return tempImage;
}

// NOLINTNEXTLINE(google-readability-namespace-comments)
}  // namespace HWY_NAMESPACE
}  // namespace hwy_ext
HWY_AFTER_NAMESPACE();

#if HWY_ONCE

// Defines the per-target dispatch table. Must appear at namespace scope, after
// HWY_AFTER_NAMESPACE() and before any use of HWY_DYNAMIC_DISPATCH.
namespace hwy_ext {
HWY_EXPORT(ApplyFilterKernel);

// Runtime-dispatched entry point. Lives in the same namespace as the HWY_EXPORT
// table so the unqualified dispatch-table reference resolves.
CXMMImage* ApplyFilter_Highway(int nSourceHeight, int nTargetHeight, int nWidth,
	int nStartY_FP, int nStartX, intfp nIncrementY_FP,
	const XMMFilterKernelBlock& filter,
	int nFilterOffset, const CXMMImage* pSourceImg) {
	return HWY_DYNAMIC_DISPATCH(ApplyFilterKernel)(
		nSourceHeight, nTargetHeight, nWidth, nStartY_FP, nStartX, nIncrementY_FP,
		filter, nFilterOffset, pSourceImg);
}

}  // namespace hwy_ext

#endif  // HWY_ONCE
