#include "stdafx.h"
#include "ColorPalette.h"
#include <algorithm>
#include <cstring>

// One sampled pixel in 32-bit BGRA format.
struct PixelBGRA {
	UINT8 b, g, r, a;
};

// A bounding box of colors in the quantization, plus the pixels it contains.
struct ColorBox {
	std::vector<PixelBGRA> pixels;
	int minR, maxR, minG, maxG, minB, maxB;

	void ComputeBounds() {
		minR = 255; maxR = 0; minG = 255; maxG = 0; minB = 255; maxB = 0;
		for (const auto& p : pixels) {
			if (p.r < minR) minR = p.r; if (p.r > maxR) maxR = p.r;
			if (p.g < minG) minG = p.g; if (p.g > maxG) maxG = p.g;
			if (p.b < minB) minB = p.b; if (p.b > maxB) maxB = p.b;
		}
	}

	// The longest channel range determines which axis to split along.
	int LongestChannel() const {
		int dr = maxR - minR, dg = maxG - minG, db = maxB - minB;
		if (dr >= dg && dr >= db) return 0; // R
		if (dg >= db) return 1;             // G
		return 2;                          // B
	}

	// Average color of all pixels in the box, weighted by count (each sampled pixel = 1).
	PixelBGRA Average() const {
		PixelBGRA avg = { 0, 0, 0, 0 };
		if (pixels.empty()) return avg;
		long sumR = 0, sumG = 0, sumB = 0, sumA = 0;
		for (const auto& p : pixels) {
			sumR += p.r; sumG += p.g; sumB += p.b; sumA += p.a;
		}
		size_t n = pixels.size();
		avg.r = (UINT8)(sumR / n);
		avg.g = (UINT8)(sumG / n);
		avg.b = (UINT8)(sumB / n);
		avg.a = (UINT8)(sumA / n);
		return avg;
	}
};

std::vector<CColorPalette::SColor> CColorPalette::Extract(const void* pPixelsBGRA, int nWidth, int nHeight, int nMaxColors) {
	std::vector<SColor> result;
	if (pPixelsBGRA == NULL || nWidth <= 0 || nHeight <= 0 || nMaxColors <= 0) return result;

	const PixelBGRA* pSrc = static_cast<const PixelBGRA*>(pPixelsBGRA);

	// Downsample so we never process more than ~10000 pixels.
	int nTotalPixels = nWidth * nHeight;
	int nMaxSamples = 10000;
	int nStep = 1;
	if (nTotalPixels > nMaxSamples) {
		nStep = (int)((double)nTotalPixels / nMaxSamples + 0.5);
		if (nStep < 1) nStep = 1;
	}

	std::vector<PixelBGRA> samples;
	samples.reserve(nTotalPixels / nStep + 1);
	for (int i = 0; i < nTotalPixels; i += nStep) {
		PixelBGRA p = pSrc[i];
		// Skip fully transparent pixels — they carry no visible color.
		if (p.a == 0) continue;
		samples.push_back(p);
	}
	if (samples.empty()) return result;

	// Median-cut: start with one box containing all samples, repeatedly
	// split the box with the largest channel range until we reach nMaxColors.
	std::vector<ColorBox> boxes;
	ColorBox initial;
	initial.pixels = samples;
	initial.ComputeBounds();
	boxes.push_back(initial);

	while ((int)boxes.size() < nMaxColors) {
		// Find the box with the largest range to split.
		int nBestIdx = -1;
		int nBestRange = 0;
		for (int i = 0; i < (int)boxes.size(); i++) {
			int dr = boxes[i].maxR - boxes[i].minR;
			int dg = boxes[i].maxG - boxes[i].minG;
			int db = boxes[i].maxB - boxes[i].minB;
			int range = (dr >= dg) ? ((dr >= db) ? dr : db) : ((dg >= db) ? dg : db);
			if (range > nBestRange && boxes[i].pixels.size() > 1) {
				nBestRange = range;
				nBestIdx = i;
			}
		}
		if (nBestIdx < 0) break; // no splittable box left

		ColorBox& box = boxes[nBestIdx];
		int channel = box.LongestChannel();

		// Sort by the longest channel and split at the median.
		if (channel == 0) {
			std::sort(box.pixels.begin(), box.pixels.end(), [](const PixelBGRA& a, const PixelBGRA& b) { return a.r < b.r; });
		} else if (channel == 1) {
			std::sort(box.pixels.begin(), box.pixels.end(), [](const PixelBGRA& a, const PixelBGRA& b) { return a.g < b.g; });
		} else {
			std::sort(box.pixels.begin(), box.pixels.end(), [](const PixelBGRA& a, const PixelBGRA& b) { return a.b < b.b; });
		}

		size_t mid = box.pixels.size() / 2;
		ColorBox left, right;
		left.pixels.assign(box.pixels.begin(), box.pixels.begin() + mid);
		right.pixels.assign(box.pixels.begin() + mid, box.pixels.end());
		left.ComputeBounds();
		right.ComputeBounds();

		// Replace the original box with the two halves.
		boxes[nBestIdx] = left;
		boxes.push_back(right);
	}

	// Build the result: average color of each box + the box's pixel count.
	for (const auto& box : boxes) {
		PixelBGRA avg = box.Average();
		SColor sc;
		sc.b = avg.b; sc.g = avg.g; sc.r = avg.r; sc.a = avg.a;
		sc.count = (int)box.pixels.size();
		result.push_back(sc);
	}

	// Sort by frequency (most pixels first).
	std::sort(result.begin(), result.end(), [](const SColor& a, const SColor& b) { return a.count > b.count; });
	return result;
}

CString CColorPalette::ToHex(const SColor& c) {
	CString s;
	s.Format(_T("#%02X%02X%02X"), c.r, c.g, c.b);
	return s;
}