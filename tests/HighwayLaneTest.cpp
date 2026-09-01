// Verifies that the highway-based HQ resampling (SampleDown_HQ_SIMD /
// SampleUp_HQ_SIMD) produces correct output for EVERY SIMD lane width the
// runtime dispatcher can select, not just the widest one on the build machine.
//
// The pixel pipeline around ApplyFilter_Highway (CXMMImage padding, the
// Rotate/RotateToDIB stride math and the channel-block interleave format) is
// hard-wired to a 16-pixel granularity. ApplyFilterHighway caps its vector
// width at 16 int16 lanes and emits 16-pixel channel blocks regardless of the
// dispatched width; this test forces narrower targets (SSE2/SSE4: 8 lanes)
// via hwy::SetSupportedTargetsForTest and checks the output against a linear
// ramp, which any normalized FIR filter must preserve. A lane/granularity
// mismatch shows up as a large error (row shifts, channel swaps, seams).
//
// Also covers the scalar SampleDown_HQ path (regression test for the
// operator-precedence bug in the (src << 16) / tgt + 1 increments).
#include "StdAfx.h"
#include "BasicProcessing.h"
#include "Helpers.h"
#include "ResizeFilter.h"
#include "SettingsProvider.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <hwy/highway.h>
#include <hwy/targets.h>

CAppModule _Module;

// Link stub: CProcessingThreadPool's lazy thread creation reads
// CSettingsProvider::This() for the core count. A zeroed instance yields 0
// pool threads, i.e. single-threaded strip processing - exactly the
// deterministic behavior this test wants. The object is never dereferenced
// beyond trivially-readable POD members.
CSettingsProvider& CSettingsProvider::This() {
    static char buf[sizeof(CSettingsProvider)] = { 0 };
    return *reinterpret_cast<CSettingsProvider*>(buf);
}

struct Img {
    int w, h, ch;
    std::vector<uint8> px;
    int stride() const { return Helpers::DoPadding(w * ch, 4); }
};

static Img MakeSrc(int w, int h, int ch) {
    Img im{ w, h, ch };
    im.px.assign((size_t)im.stride() * h, 0);
    for (int y = 0; y < h; y++) {
        uint8* row = &im.px[(size_t)y * im.stride()];
        for (int x = 0; x < w; x++) {
            uint8 B = (uint8)(0.5 + 255.0 * x / (w - 1));
            uint8 G = (uint8)(0.5 + 255.0 * y / (h - 1));
            uint8 R = (uint8)(0.5 + 255.0 * (x + y) / (w + h - 2));
            if (ch == 3) { row[x * 3] = B; row[x * 3 + 1] = G; row[x * 3 + 2] = R; }
            else { row[x * 4] = B; row[x * 4 + 1] = G; row[x * 4 + 2] = R; row[x * 4 + 3] = (uint8)(255 - G); }
        }
    }
    return im;
}

// Fixed-point source positions used by the legacy sampling mappings.
static double DownPos(int t, int srcN, int fullN) {
    unsigned long long inc = (((unsigned long long)srcN << 16) / fullN) + 1;
    long long off = (long long)((inc - 65536) >> 1);
    return (off + (long long)t * (long long)inc) / 65536.0;
}
static double UpPos(int t, int srcN, int fullN) {
    unsigned long long inc = (65536ULL * (unsigned long long)(srcN - 1)) / (fullN - 1);
    return ((long long)t * (long long)inc) / 65536.0;
}

// Verify a resampled DIB against the ramp expectation. Any row/column shift,
// stride mismatch or channel-block interleave bug produces errors far above
// the filter tolerance.
static bool Verify(const char* name, const uint8* dib, int tw, int th,
    int sw, int sh, bool up, bool checkAlpha) {
    const int margin = 8;
    double maxE[4] = { 0,0,0,0 };
    for (int y = margin; y < th - margin; y++) {
        double syp = up ? UpPos(y, sh, th) : DownPos(y, sh, th);
        double expG = 255.0 * syp / (sh - 1);
        for (int x = margin; x < tw - margin; x++) {
            double sxp = up ? UpPos(x, sw, tw) : DownPos(x, sw, tw);
            double expB = 255.0 * sxp / (sw - 1);
            double expR = 255.0 * (sxp + syp) / (sw + sh - 2);
            const uint8* p = dib + ((size_t)y * tw + x) * 4;
            // windows headers define a max() macro, so track maxima manually
            double e0 = fabs(p[0] - expB); if (e0 > maxE[0]) maxE[0] = e0;
            double e1 = fabs(p[1] - expG); if (e1 > maxE[1]) maxE[1] = e1;
            double e2 = fabs(p[2] - expR); if (e2 > maxE[2]) maxE[2] = e2;
            if (checkAlpha) { double e3 = fabs(p[3] - (255.0 - expG)); if (e3 > maxE[3]) maxE[3] = e3; }
        }
    }
    bool ok = maxE[0] <= 4.0 && maxE[1] <= 4.0 && maxE[2] <= 4.0 && maxE[3] <= 4.0;
    printf("%-52s %s  maxErr B=%.1f G=%.1f R=%.1f A=%.1f\n",
        name, ok ? "PASS" : "FAIL", maxE[0], maxE[1], maxE[2], maxE[3]);
    return ok;
}

static bool RunDown(const char* name, int sw, int sh, int tw, int th, int ch, double sharpen) {
    Img src = MakeSrc(sw, sh, ch);
    void* out = CBasicProcessing::SampleDown_HQ_SIMD(CSize(tw, th), CPoint(0, 0), CSize(tw, th),
        CSize(sw, sh), src.px.data(), ch, sharpen, Filter_Downsampling_Best_Quality,
        CBasicProcessing::AVX2);
    if (out == NULL) { printf("%-52s FAIL (returned NULL)\n", name); return false; }
    bool ok = Verify(name, (const uint8*)out, tw, th, sw, sh, false, ch == 4);
    delete[](uint8*)out;
    return ok;
}

static bool RunUp(const char* name, int sw, int sh, int tw, int th, int ch) {
    Img src = MakeSrc(sw, sh, ch);
    void* out = CBasicProcessing::SampleUp_HQ_SIMD(CSize(tw, th), CPoint(0, 0), CSize(tw, th),
        CSize(sw, sh), src.px.data(), ch, CBasicProcessing::AVX2);
    if (out == NULL) { printf("%-52s FAIL (returned NULL)\n", name); return false; }
    bool ok = Verify(name, (const uint8*)out, tw, th, sw, sh, true, ch == 4);
    delete[](uint8*)out;
    return ok;
}

static bool RunDownScalar(const char* name, int sw, int sh, int tw, int th, int ch, double sharpen) {
    Img src = MakeSrc(sw, sh, ch);
    void* out = CBasicProcessing::SampleDown_HQ(CSize(tw, th), CPoint(0, 0), CSize(tw, th),
        CSize(sw, sh), src.px.data(), ch, sharpen, Filter_Downsampling_Best_Quality);
    if (out == NULL) { printf("%-52s FAIL (returned NULL)\n", name); return false; }
    bool ok = Verify(name, (const uint8*)out, tw, th, sw, sh, false, false);
    delete[](uint8*)out;
    return ok;
}

// Runs the core down/up cases under whatever highway target is currently
// allowed. Called once per forced target.
static bool RunSuite(const char* tag) {
    char name[128];
    bool ok = true;
    sprintf_s(name, "[%s] down 424x284 -> 293x220 ch4", tag);
    ok &= RunDown(name, 424, 284, 293, 220, 4, 0.3);
    sprintf_s(name, "[%s] down 424x284 -> 293x220 ch3", tag);
    ok &= RunDown(name, 424, 284, 293, 220, 3, 0.3);
    sprintf_s(name, "[%s] down pano 1096x112 -> 300x74 ch4", tag);
    ok &= RunDown(name, 1096, 112, 300, 74, 4, 0.3);
    sprintf_s(name, "[%s] down tall 112x1120 -> 24x240 ch4", tag);
    ok &= RunDown(name, 112, 1120, 24, 240, 4, 0.3);
    sprintf_s(name, "[%s] up 424x284 -> 600x400 ch4", tag);
    ok &= RunUp(name, 424, 284, 600, 400, 4);
    return ok;
}

static bool RunLargeDimensionKernelTest() {
    bool ok = true;
    {
        CResizeFilter filter(93761, 1920, 0.15,
            Filter_Downsampling_Best_Quality, FilterSIMDType_SSE);
        const FilterKernelBlock& scalar = filter.GetFilterKernels();
        const XMMFilterKernelBlock& simd = filter.GetXMMFilterKernels();
        ok &= scalar.NumKernels > 0 && scalar.Indices != nullptr && scalar.Kernels != nullptr;
        ok &= simd.NumKernels > 0 && simd.Indices != nullptr && simd.Kernels != nullptr;
        for (int i = 0; ok && i < 1920; ++i) {
            ok &= scalar.Indices[i] != nullptr && scalar.Indices[i]->FilterLen > 0 &&
                scalar.Indices[i]->FilterLen <= MAX_FILTER_LEN && simd.Indices[i] != nullptr;
        }
    }
    {
        CResizeFilter filter(1920, 93761, 0.0,
            Filter_Upsampling_Bicubic, FilterSIMDType_SSE);
        const FilterKernelBlock& scalar = filter.GetFilterKernels();
        ok &= scalar.NumKernels > 0 && scalar.Indices != nullptr && scalar.Kernels != nullptr;
        ok &= scalar.Indices[0] != nullptr && scalar.Indices[93760] != nullptr;
    }
    printf("%-52s %s\n", "resize kernels across the 65535 boundary", ok ? "PASS" : "FAIL");
    return ok;
}

static bool RunLargeDimensionResampleTest() {
#ifdef _WIN64
	// Exercise the filter, not only kernel construction. During the horizontal
	// pass the 16.16 source position reaches about 6.1 billion, which overflowed
	// ApplyFilterKernel's int accumulator and produced an out-of-bounds AVX2
	// load for the 93761-pixel-wide TIFF from the crash report.
	return RunDown("down 93761x64 -> 1920x32 ch4", 93761, 64, 1920, 32, 4, 0.15);
#else
	printf("%-52s %s\n", "down 93761x64 -> 1920x32 ch4", "SKIP (x64 regression)");
	return true;
#endif
}

static bool RunMoveRectTestCase(const char* name, CRect sourceRect, CPoint targetTopLeft) {
	const CSize size(7, 6);
	std::vector<uint32> original((size_t)size.cx * size.cy);
	for (int y = 0; y < size.cy; ++y) {
		for (int x = 0; x < size.cx; ++x) {
			original[(size_t)y * size.cx + x] = (uint32)(y * 100 + x);
		}
	}

	std::vector<uint32> expected = original;
	for (int y = 0; y < sourceRect.Height(); ++y) {
		for (int x = 0; x < sourceRect.Width(); ++x) {
			expected[(size_t)(targetTopLeft.y + y) * size.cx + targetTopLeft.x + x] =
				original[(size_t)(sourceRect.top + y) * size.cx + sourceRect.left + x];
		}
	}

	std::vector<uint32> actual = original;
	bool ok = CBasicProcessing::MoveRect32bpp(actual.data(), size, sourceRect, targetTopLeft) && actual == expected;
	printf("%-52s %s\n", name, ok ? "PASS" : "FAIL");
	return ok;
}

static bool RunMoveRectTests() {
	bool ok = true;
	ok &= RunMoveRectTestCase("in-place pan copy down-right", CRect(0, 0, 6, 5), CPoint(1, 1));
	ok &= RunMoveRectTestCase("in-place pan copy up-left", CRect(1, 1, 7, 6), CPoint(0, 0));
	ok &= RunMoveRectTestCase("in-place pan copy up-right", CRect(0, 1, 6, 6), CPoint(1, 0));
	ok &= RunMoveRectTestCase("in-place pan copy down-left", CRect(1, 0, 7, 5), CPoint(0, 1));
	std::vector<uint32> pixels(42, 0);
	bool rejected = !CBasicProcessing::MoveRect32bpp(pixels.data(), CSize(7, 6), CRect(0, 0, 7, 6), CPoint(1, 0));
	printf("%-52s %s\n", "in-place pan copy rejects out-of-bounds target", rejected ? "PASS" : "FAIL");
	return ok && rejected;
}

static bool RunNegativeOffsetValidationTests() {
	const CSize fullSize(4, 4);
	const CSize clippedSize(2, 2);
	std::vector<uint32> pixels(16, 0xFFFFFFFFu);
	const CPoint negativeY(0, -1);
	bool ok = CBasicProcessing::PointSample(fullSize, negativeY, clippedSize,
		fullSize, pixels.data(), 4) == NULL;
	ok &= CBasicProcessing::PointSampleWithRotation(fullSize, negativeY, clippedSize,
		fullSize, 0.1, pixels.data(), 4, RGB(0, 0, 0)) == NULL;
	const CTrapezoid trapezoid(0, 3, 0, 0, 3, 3);
	ok &= CBasicProcessing::PointSampleTrapezoid(fullSize, trapezoid, negativeY,
		clippedSize, fullSize, pixels.data(), 4, RGB(0, 0, 0)) == NULL;
	printf("%-52s %s\n", "point samplers reject a negative Y offset", ok ? "PASS" : "FAIL");
	return ok;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    int64_t sup = hwy::SupportedTargets();
    int64_t best = sup & (-sup);
    printf("hwy best target: %s\n", hwy::TargetName(best));

	bool all = true;
	all &= RunLargeDimensionKernelTest();
	all &= RunLargeDimensionResampleTest();
	all &= RunMoveRectTests();
	all &= RunNegativeOffsetValidationTests();

    // Default (best) dispatch.
    all &= RunSuite("best");

    // Scalar downsample path (no highway involved).
    all &= RunDownScalar("[scalar] down 424x284 -> 293x220", 424, 284, 293, 220, 4, 0.3);

    // Force each narrower compiled target: output must stay correct because
    // the buffer geometry and block interleave are fixed at 16 pixels.
    struct { int64_t target; const char* tag; } forced[] = {
        { HWY_SSE2, "forced SSE2 (8 lanes)" },
        { HWY_SSE4, "forced SSE4 (8 lanes)" },
    };
    for (const auto& f : forced) {
        if ((sup & f.target) == 0) {
            printf("%-52s SKIP (target unsupported)\n", f.tag);
            continue;
        }
        hwy::SetSupportedTargetsForTest(f.target);
        all &= RunSuite(f.tag);
        hwy::SetSupportedTargetsForTest(0);
    }

    printf("\n%s\n", all ? "ALL PASS" : "FAILURES PRESENT");
    return all ? 0 : 1;
}
