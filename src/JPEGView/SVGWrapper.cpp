#include "stdafx.h"
#include "SVGWrapper.h"
#include "MaxImageDef.h"
#include "Helpers.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

// Reads an SVG file and rasterizes it to BGRA pixels.
// SVG is a vector format; JPEGView rasterizes it once at load time at the
// intrinsic size (or a fallback). Zoom re-rasterization is handled by the
// caller requesting a new target size via the load thread cache invalidation.
void* SvgReader::ReadImage(int& width, int& height, int& bpp, bool& outOfMemory,
	LPCTSTR strFileName, int targetWidth, int targetHeight)
{
	outOfMemory = false;
	bpp = 4;
	width = 0;
	height = 0;

	// Load the SVG file into memory
	HANDLE hFile = ::CreateFile(strFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return NULL;

	LARGE_INTEGER fileSize;
	if (!::GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart > MAX_SVG_FILE_SIZE) {
		::CloseHandle(hFile);
		return NULL;
	}

	size_t nSize = (size_t)fileSize.QuadPart;
	char* pBuffer = new(std::nothrow) char[nSize + 1];
	if (pBuffer == NULL) {
		::CloseHandle(hFile);
		outOfMemory = true;
		return NULL;
	}

	DWORD nBytesRead = 0;
	BOOL bOk = ::ReadFile(hFile, pBuffer, (DWORD)nSize, &nBytesRead, NULL);
	::CloseHandle(hFile);
	if (!bOk || nBytesRead != nSize) {
		delete[] pBuffer;
		return NULL;
	}
	pBuffer[nSize] = 0; // null-terminate for NanoSVG

	// Parse the SVG. NSVG_IMAGE_KEEP_ASPECT preserves aspect ratio.
	NSVGimage* pImage = nsvgParse(pBuffer, "px", 96.0f);
	delete[] pBuffer;
	if (pImage == NULL) return NULL;

	// Determine rasterization size
	int w = (int)(pImage->width + 0.5f);
	int h = (int)(pImage->height + 0.5f);
	if (w <= 0 || h <= 0) {
		// No intrinsic size; use fallback
		w = 1024;
		h = 1024;
	}

	if (targetWidth > 0 && targetHeight > 0) {
		w = targetWidth;
		h = targetHeight;
	} else if (targetWidth > 0) {
		float scale = (float)targetWidth / (float)w;
		h = (int)(h * scale + 0.5f);
		w = targetWidth;
	} else if (targetHeight > 0) {
		float scale = (float)targetHeight / (float)h;
		w = (int)(w * scale + 0.5f);
		h = targetHeight;
	}

	// Clamp to max dimensions
	if (w > (int)MAX_IMAGE_DIMENSION) w = MAX_IMAGE_DIMENSION;
	if (h > (int)MAX_IMAGE_DIMENSION) h = MAX_IMAGE_DIMENSION;
	if (w < 1) w = 1;
	if (h < 1) h = 1;

	if ((double)w * h > MAX_IMAGE_PIXELS) {
		nsvgDelete(pImage);
		outOfMemory = true;
		return NULL;
	}

	// Rasterize
	NSVGrasterizer* pRast = nsvgCreateRasterizer();
	if (pRast == NULL) {
		nsvgDelete(pImage);
		return NULL;
	}

	// NanoSVG outputs RGBA; allocate BGRA buffer
	unsigned char* pPixelData = new(std::nothrow) unsigned char[w * h * 4];
	if (pPixelData == NULL) {
		nsvgDeleteRasterizer(pRast);
		nsvgDelete(pImage);
		outOfMemory = true;
		return NULL;
	}

	// NanoSVG uses a single uniform scale; use the smaller of X/Y to preserve aspect ratio.
	float scale = (float)w / pImage->width;
	nsvgRasterize(pRast, pImage, 0, 0, scale, pPixelData, w, h, w * 4);

	nsvgDeleteRasterizer(pRast);
	nsvgDelete(pImage);

	// Convert RGBA -> BGRA in place
	for (int i = 0; i < w * h; i++) {
		unsigned char r = pPixelData[i * 4 + 0];
		unsigned char a = pPixelData[i * 4 + 3];
		pPixelData[i * 4 + 0] = pPixelData[i * 4 + 2]; // B <- B
		pPixelData[i * 4 + 2] = r;                      // R <- R
		pPixelData[i * 4 + 3] = a;
	}

	width = w;
	height = h;
	return (void*)pPixelData;
}
