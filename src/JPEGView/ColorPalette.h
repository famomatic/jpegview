#pragma once

#include <vector>

/// Computes a small palette of dominant colors from BGRA pixel data using a
/// simple median-cut quantization. Designed to be fast and dependency-free so
/// it can run on the current image without blocking the UI noticeably.
class CColorPalette
{
public:
	struct SColor {
		UINT8 b, g, r, a;
		int count;
	};

	/// Extract up to nMaxColors dominant colors from the given BGRA pixels.
	/// The image is internally downsampled before quantization to keep the
	/// cost bounded. Returns the palette sorted by frequency (most used first).
	static std::vector<SColor> Extract(const void* pPixelsBGRA, int nWidth, int nHeight, int nMaxColors = 6);

	/// Returns a "#RRGGBB" hex string for a color.
	static CString ToHex(const SColor& c);
};