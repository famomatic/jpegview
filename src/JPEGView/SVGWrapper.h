#pragma once

class SvgReader
{
public:
	// Returns data in 4-byte BGRA. SVG is rasterized to the given target size.
	// If targetWidth/targetHeight is 0, the SVG's intrinsic size is used (fallback 1024x1024).
	static void* ReadImage(int& width,   // width of the rasterized image
		int& height,  // height of the rasterized image
		int& bpp,     // BYTES (not bits) PER PIXEL (always 4)
		bool& outOfMemory,
		LPCTSTR strFileName, // path to the SVG file
		int targetWidth = 0,  // 0 = use intrinsic size
		int targetHeight = 0); // 0 = use intrinsic size
};
