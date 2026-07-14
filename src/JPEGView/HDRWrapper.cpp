#include "stdafx.h"
#include "HDRWrapper.h"
#include "MaxImageDef.h"
#include "Helpers.h"
#include <memory>

// Radiance RGBE (.hdr / .pic) reader.
// Implements the legacy RLE encoding used by Radiance.
// Reference: http://www.graphics.cornell.edu/~bjw/rgbe.html

struct RGBE { unsigned char r, g, b, e; };

static inline unsigned char tonemap(float v) {
	float mapped = v / (1.0f + v);
	float gamma = powf(mapped, 1.0f / 2.2f);
	int iv = (int)(gamma * 255.0f + 0.5f);
	if (iv < 0) iv = 0; if (iv > 255) iv = 255;
	return (unsigned char)iv;
}

static inline void rgbeToFloat(const RGBE& c, float& rf, float& gf, float& bf) {
	if (c.e == 0) { rf = gf = bf = 0; return; }
	float f = ldexpf(1.0f, c.e - (128 + 8));
	rf = c.r * f; gf = c.g * f; bf = c.b * f;
}

void* HdrReader::ReadImage(int& width, int& height, int& bpp, bool& outOfMemory,
	const void* buffer, int sizebytes)
{
	bpp = 4;
	float* pFloatPixels = ReadImageFloat(width, height, outOfMemory, buffer, sizebytes);
	if (pFloatPixels == NULL) {
		return NULL;
	}
	unsigned char* pPixelData = new(std::nothrow) unsigned char[(size_t)width * height * 4];
	if (pPixelData == NULL) {
		outOfMemory = true;
		delete[] pFloatPixels;
		width = height = 0;
		return NULL;
	}
	for (__int64 i = 0; i < (__int64)width * height; i++) {
		pPixelData[i * 4 + 0] = tonemap(pFloatPixels[i * 4 + 2]); // B
		pPixelData[i * 4 + 1] = tonemap(pFloatPixels[i * 4 + 1]); // G
		pPixelData[i * 4 + 2] = tonemap(pFloatPixels[i * 4 + 0]); // R
		pPixelData[i * 4 + 3] = 255;
	}
	delete[] pFloatPixels;
	return (void*)pPixelData;
}

float* HdrReader::ReadImageFloat(int& width, int& height, bool& outOfMemory,
	const void* buffer, int sizebytes)
{
	outOfMemory = false;
	width = 0;
	height = 0;

	const unsigned char* p = (const unsigned char*)buffer;
	const unsigned char* end = p + sizebytes;

	// Parse header: look for "#?RADIANCE" or "#?RGBE"
	if (sizebytes < 10 || p[0] != '#') return NULL;
	// Scan header lines until empty line
	const unsigned char* line = p;
	int w = 0, h = 0;
	bool formatRLE = false;
	while (line < end) {
		const unsigned char* eol = (const unsigned char*)memchr(line, '\n', end - line);
		if (eol == NULL) return NULL;
		size_t len = eol - line;
		if (len == 0) { line = eol + 1; break; } // empty line = end of header
		if (len >= 10 && memcmp(line, "FORMAT=", 7) == 0) {
			if (len >= 21 && memcmp(line + 7, "32-bit_rle_rgbe", 15) == 0) formatRLE = true;
		}
		if (len >= 2 && line[0] == '-' && line[1] == 'Y') {
			// -Y height +X width
			const char* s = (const char*)(line + 2);
			h = atoi(s);
			const char* plus = strchr(s, '+');
			if (plus) w = atoi(plus + 2);
		}
		line = eol + 1;
	}
	if (w <= 0 || h <= 0 || w > (int)MAX_IMAGE_DIMENSION || h > (int)MAX_IMAGE_DIMENSION) return NULL;
	if ((double)w * h > MAX_IMAGE_PIXELS) { outOfMemory = true; return NULL; }

	const unsigned char* data = line;
	if (data >= end) return NULL;

	// Allocate scanline storage
	RGBE* pScanline = new(std::nothrow) RGBE[w];
	if (pScanline == NULL) { outOfMemory = true; return NULL; }

	float* pPixelData = new(std::nothrow) float[(size_t)w * h * 4];
	if (pPixelData == NULL) { delete[] pScanline; outOfMemory = true; return NULL; }

	for (int y = 0; y < h; y++) {
		// Check for RLE marker (new encoding): first 4 bytes are 2,2,w>>8,w&0xff
		if (data + 4 <= end && data[0] == 2 && data[1] == 2) {
			int scanlen = (data[2] << 8) | data[3];
			if (scanlen != w) { delete[] pScanline; delete[] pPixelData; return NULL; }
			data += 4;
			// Read 4 channels, each RLE-encoded
			for (int ch = 0; ch < 4; ch++) {
				unsigned char* dst = (unsigned char*)pScanline + ch;
				int remaining = w;
				while (remaining > 0 && data < end) {
					int code = *data++;
					if (code > 128) {
						int count = code - 128;
						if (data >= end || remaining < count) { delete[] pScanline; delete[] pPixelData; return NULL; }
						unsigned char val = *data++;
						for (int i = 0; i < count; i++) dst[i * 4] = val;
						remaining -= count;
						dst += count * 4;
					} else {
						int count = code;
						if (data + count > end || remaining < count) { delete[] pScanline; delete[] pPixelData; return NULL; }
						for (int i = 0; i < count; i++) dst[i * 4] = data[i];
						data += count;
						remaining -= count;
						dst += count * 4;
					}
				}
			}
		} else {
			// Old encoding: raw RGBE per pixel
			if (data + (size_t)w * 4 > end) { delete[] pScanline; delete[] pPixelData; return NULL; }
			memcpy(pScanline, data, (size_t)w * 4);
			data += (size_t)w * 4;
		}

		for (int x = 0; x < w; x++) {
			float r, g, b;
			rgbeToFloat(pScanline[x], r, g, b);
			pPixelData[((size_t)y * w + x) * 4 + 0] = r;
			pPixelData[((size_t)y * w + x) * 4 + 1] = g;
			pPixelData[((size_t)y * w + x) * 4 + 2] = b;
			pPixelData[((size_t)y * w + x) * 4 + 3] = 1.0f;
		}
	}

	delete[] pScanline;
	width = w;
	height = h;
	return pPixelData;
}
