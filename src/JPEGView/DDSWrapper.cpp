#include "stdafx.h"
#include "DDSWrapper.h"
#include "MaxImageDef.h"
#include <memory>

// DDS (DirectDraw Surface) reader.
// Supports uncompressed RGB/RGBA and BC1/BC2/BC3 block-compressed textures.
// Reference: Microsoft DDS Programming Guide.

#pragma pack(push,1)
struct DDS_PIXELFORMAT {
	DWORD dwSize;
	DWORD dwFlags;
	DWORD dwFourCC;
	DWORD dwRGBBitCount;
	DWORD dwRBitMask;
	DWORD dwGBitMask;
	DWORD dwBBitMask;
	DWORD dwABitMask;
};
struct DDS_HEADER {
	DWORD dwSize;
	DWORD dwFlags;
	DWORD dwHeight;
	DWORD dwWidth;
	DWORD dwPitchOrLinearSize;
	DWORD dwDepth;
	DWORD dwMipMapCount;
	DWORD dwReserved1[11];
	DDS_PIXELFORMAT ddspf;
	DWORD dwCaps;
	DWORD dwCaps2;
	DWORD dwCaps3;
	DWORD dwCaps4;
	DWORD dwReserved2;
};
struct DDS_HEADER_DXT10 {
	DWORD dxgiFormat;
	DWORD resourceDimension;
	DWORD miscFlag;
	DWORD arraySize;
	DWORD miscFlags2;
};
#pragma pack(pop)

#define DDS_MAGIC 0x20534444 // "DDS "
#define DDSD_HEIGHT 0x00000002
#define DDSD_WIDTH 0x00000004
#define DDPF_FOURCC 0x00000004
#define DDPF_RGB 0x00000040
#define DDPF_ALPHAPIXELS 0x00000001

// DXGI formats we care about
#define DXGI_FORMAT_UNKNOWN 0
#define DXGI_FORMAT_R8G8B8A8_UNORM 28
#define DXGI_FORMAT_B8G8R8A8_UNORM 87
#define DXGI_FORMAT_B8G8R8X8_UNORM 88
#define DXGI_FORMAT_R8G8B8G8_UNORM 68
#define DXGI_FORMAT_B8G8R8G8_UNORM 69

static inline DWORD ColorRGBA(BYTE r, BYTE g, BYTE b, BYTE a) {
	return (DWORD)(a << 24) | (DWORD)(r << 16) | (DWORD)(g << 8) | (DWORD)b; // BGRA
}

static void DecodeBCColor(const BYTE* block, DWORD* out, bool allowTransparentIndex) {
	DWORD c0 = block[0] | (block[1] << 8);
	DWORD c1 = block[2] | (block[3] << 8);
	BYTE r0 = static_cast<BYTE>((((c0 >> 11) & 0x1F) * 255 + 15) / 31);
	BYTE g0 = static_cast<BYTE>((((c0 >> 5) & 0x3F) * 255 + 31) / 63);
	BYTE b0 = static_cast<BYTE>(((c0 & 0x1F) * 255 + 15) / 31);
	BYTE r1 = static_cast<BYTE>((((c1 >> 11) & 0x1F) * 255 + 15) / 31);
	BYTE g1 = static_cast<BYTE>((((c1 >> 5) & 0x3F) * 255 + 31) / 63);
	BYTE b1 = static_cast<BYTE>(((c1 & 0x1F) * 255 + 15) / 31);
	DWORD colors[4];
	colors[0] = ColorRGBA(r0, g0, b0, 255);
	colors[1] = ColorRGBA(r1, g1, b1, 255);
	if (c0 > c1 || !allowTransparentIndex) {
		colors[2] = ColorRGBA((BYTE)((2*r0+r1)/3), (BYTE)((2*g0+g1)/3), (BYTE)((2*b0+b1)/3), 255);
		colors[3] = ColorRGBA((BYTE)((r0+2*r1)/3), (BYTE)((g0+2*g1)/3), (BYTE)((b0+2*b1)/3), 255);
	} else {
		colors[2] = ColorRGBA((BYTE)((r0+r1)/2), (BYTE)((g0+g1)/2), (BYTE)((b0+b1)/2), 255);
		colors[3] = ColorRGBA(0, 0, 0, 0);
	}
	DWORD idx = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24);
	for (int i = 0; i < 16; i++) {
		out[i] = colors[(idx >> (i*2)) & 3];
	}
}

// Decode BC1 (DXT1) block into 4x4 BGRA pixels.
static void DecodeBC1(const BYTE* block, DWORD* out) {
	DecodeBCColor(block, out, true);
}

// Decode BC3 (DXT5) block into 4x4 BGRA pixels
static void DecodeBC3(const BYTE* block, DWORD* out) {
	BYTE a0 = block[0];
	BYTE a1 = block[1];
	uint64_t aIdx = 0;
	for (int i = 0; i < 6; ++i) aIdx |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
	BYTE alphas[8];
	alphas[0] = a0; alphas[1] = a1;
	if (a0 > a1) {
		for (int i = 1; i < 7; i++) alphas[i+1] = (BYTE)(((7-i)*a0 + i*a1) / 7);
		alphas[7] = 0;
	} else {
		for (int i = 1; i < 5; i++) alphas[i+1] = (BYTE)(((5-i)*a0 + i*a1) / 5);
		alphas[6] = 0; alphas[7] = 255;
	}
	// color block starts at offset 8
	DWORD colors[16];
	DecodeBCColor(block + 8, colors, false);
	for (int i = 0; i < 16; i++) {
		BYTE a = alphas[(aIdx >> (i * 3)) & 7];
		out[i] = (colors[i] & 0x00FFFFFF) | (DWORD)(a << 24);
	}
}

static void DecodeBC2(const BYTE* block, DWORD* out) {
	DecodeBCColor(block + 8, out, false);
	for (int i = 0; i < 16; ++i) {
		const BYTE alpha = static_cast<BYTE>(((block[i / 2] >> ((i & 1) * 4)) & 0x0F) * 17);
		out[i] = (out[i] & 0x00FFFFFF) | (static_cast<DWORD>(alpha) << 24);
	}
}

static void UnpremultiplyBlock(DWORD* pixels) {
	for (int i = 0; i < 16; ++i) {
		const DWORD pixel = pixels[i];
		const BYTE alpha = static_cast<BYTE>(pixel >> 24);
		if (alpha == 0 || alpha == 255) continue;
		const BYTE b = static_cast<BYTE>(min(255u, ((pixel & 0xFFu) * 255u + alpha / 2u) / alpha));
		const BYTE g = static_cast<BYTE>(min(255u, (((pixel >> 8) & 0xFFu) * 255u + alpha / 2u) / alpha));
		const BYTE r = static_cast<BYTE>(min(255u, (((pixel >> 16) & 0xFFu) * 255u + alpha / 2u) / alpha));
		pixels[i] = ColorRGBA(r, g, b, alpha);
	}
}

void* DdsReader::ReadImage(int& width, int& height, int& bpp, bool& outOfMemory,
	const void* buffer, int sizebytes)
{
	outOfMemory = false;
	bpp = 4;
	width = 0;
	height = 0;

	if (buffer == NULL || sizebytes < static_cast<int>(4 + sizeof(DDS_HEADER))) return NULL;
	const BYTE* p = static_cast<const BYTE*>(buffer);
	DWORD magic = 0;
	memcpy(&magic, p, sizeof(magic));
	if (magic != DDS_MAGIC) return NULL;

	DDS_HEADER header{};
	memcpy(&header, p + 4, sizeof(header));
	const DDS_HEADER* hdr = &header;
	if (hdr->dwSize != sizeof(DDS_HEADER) || hdr->ddspf.dwSize != sizeof(DDS_PIXELFORMAT)) return NULL;

	int w = (int)hdr->dwWidth;
	int h = (int)hdr->dwHeight;
	if (w <= 0 || h <= 0 || w > (int)MAX_IMAGE_DIMENSION || h > (int)MAX_IMAGE_DIMENSION) return NULL;
	if ((double)w * h > MAX_IMAGE_PIXELS) { outOfMemory = true; return NULL; }

	size_t dataOffset = 4 + sizeof(DDS_HEADER);
	DWORD dxgiFormat = DXGI_FORMAT_UNKNOWN;
	int blockSize = 0; // bytes per 4x4 block

	// Check for DX10 header
	if ((hdr->ddspf.dwFlags & DDPF_FOURCC) && hdr->ddspf.dwFourCC == '01XD') { // "DX10"
		if (static_cast<size_t>(sizebytes) < dataOffset + sizeof(DDS_HEADER_DXT10)) return NULL;
		DDS_HEADER_DXT10 dx10{};
		memcpy(&dx10, p + dataOffset, sizeof(dx10));
		if (dx10.arraySize == 0 || dx10.resourceDimension == 0) return NULL;
		dxgiFormat = dx10.dxgiFormat;
		dataOffset += sizeof(DDS_HEADER_DXT10);
	}
	const BYTE* pImageData = p + dataOffset;
	const size_t nDataBytes = static_cast<size_t>(sizebytes) - dataOffset;

	const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
	if (pixelCount > SIZE_MAX / sizeof(DWORD)) { outOfMemory = true; return NULL; }
	DWORD* pPixelData = new(std::nothrow) DWORD[pixelCount];
	if (pPixelData == NULL) { outOfMemory = true; return NULL; }
	// Initialize to transparent
	memset(pPixelData, 0, pixelCount * sizeof(DWORD));

	auto putBlock = [&](DWORD* blockPixels, int bx, int by) {
		for (int y = 0; y < 4; y++) {
			for (int x = 0; x < 4; x++) {
				int px = bx + x;
				int py = by + y;
				if (px < w && py < h) {
					pPixelData[py * w + px] = blockPixels[y * 4 + x];
				}
			}
		}
	};

	if (dxgiFormat == DXGI_FORMAT_R8G8B8A8_UNORM || dxgiFormat == DXGI_FORMAT_B8G8R8A8_UNORM ||
		dxgiFormat == DXGI_FORMAT_B8G8R8X8_UNORM) {
		// Uncompressed 32-bit
		const size_t required = pixelCount * 4;
		if (required > nDataBytes) { delete[] pPixelData; return NULL; }
		const int srcBpp = 4;
		bool bgr = (dxgiFormat != DXGI_FORMAT_R8G8B8A8_UNORM);
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				const BYTE* s = pImageData + (y * w + x) * srcBpp;
				BYTE r = bgr ? s[2] : s[0];
				BYTE g = s[1];
				BYTE b = bgr ? s[0] : s[2];
				BYTE a = (dxgiFormat == DXGI_FORMAT_B8G8R8X8_UNORM) ? 255 : s[3];
				pPixelData[y * w + x] = ColorRGBA(r, g, b, a);
			}
		}
	} else if (hdr->ddspf.dwFlags & DDPF_FOURCC) {
		DWORD fourcc = hdr->ddspf.dwFourCC;
		if (fourcc == '1TXD') { blockSize = 8; }       // DXT1 / BC1
		else if (fourcc == '2TXD' || fourcc == '3TXD' || fourcc == '4TXD' || fourcc == '5TXD') { blockSize = 16; }
		else {
			// BC4-BC7 and other DX10 formats are rejected until a complete decoder is available.
			delete[] pPixelData;
			return NULL;
		}

		const size_t blocksX = (static_cast<size_t>(w) + 3) / 4;
		const size_t blocksY = (static_cast<size_t>(h) + 3) / 4;
		if (blocksX > SIZE_MAX / blocksY || blocksX * blocksY > nDataBytes / static_cast<size_t>(blockSize)) {
			delete[] pPixelData;
			return NULL;
		}
		const BYTE* pBlock = pImageData;
		DWORD blockPixels[16];
		for (size_t by = 0; by < blocksY; by++) {
			for (size_t bx = 0; bx < blocksX; bx++) {
				if (fourcc == '1TXD') DecodeBC1(pBlock, blockPixels);
				else if (fourcc == '2TXD' || fourcc == '3TXD') DecodeBC2(pBlock, blockPixels);
				else DecodeBC3(pBlock, blockPixels);
				if (fourcc == '2TXD' || fourcc == '4TXD') UnpremultiplyBlock(blockPixels);
				putBlock(blockPixels, static_cast<int>(bx * 4), static_cast<int>(by * 4));
				pBlock += blockSize;
			}
		}
	} else if (hdr->ddspf.dwFlags & DDPF_RGB) {
		// Uncompressed RGB with bit masks
		if (hdr->ddspf.dwRGBBitCount != 8 && hdr->ddspf.dwRGBBitCount != 16 &&
			hdr->ddspf.dwRGBBitCount != 24 && hdr->ddspf.dwRGBBitCount != 32) {
			delete[] pPixelData;
			return NULL;
		}
		const size_t srcBpp = hdr->ddspf.dwRGBBitCount / 8;
		DWORD rMask = hdr->ddspf.dwRBitMask;
		DWORD gMask = hdr->ddspf.dwGBitMask;
		DWORD bMask = hdr->ddspf.dwBBitMask;
		DWORD aMask = (hdr->ddspf.dwFlags & DDPF_ALPHAPIXELS) ? hdr->ddspf.dwABitMask : 0;
		auto getShift = [](DWORD mask) -> int { int s = 0; while (mask && (mask & 1) == 0) { mask >>= 1; s++; } return s; };
		auto getBits = [](DWORD mask) -> int { int b = 0; while (mask) { mask &= mask - 1; b++; } return b; };
		int rShift = getShift(rMask), gShift = getShift(gMask), bShift = getShift(bMask), aShift = getShift(aMask);
		int rBits = getBits(rMask), gBits = getBits(gMask), bBits = getBits(bMask), aBits = getBits(aMask);
		auto scale = [](DWORD v, int bits) -> BYTE {
			if (bits == 0) return 0;
			if (bits >= 8) return static_cast<BYTE>(v >> (bits - 8));
			const DWORD maxValue = (1u << bits) - 1u;
			return static_cast<BYTE>((v * 255u + maxValue / 2u) / maxValue);
		};
		const size_t rowBytes = (static_cast<size_t>(w) * srcBpp + 3) & ~static_cast<size_t>(3);
		if (rowBytes > SIZE_MAX / static_cast<size_t>(h) || rowBytes * static_cast<size_t>(h) > nDataBytes) {
			delete[] pPixelData;
			return NULL;
		}
		for (int y = 0; y < h; y++) {
			const BYTE* row = pImageData + y * rowBytes;
			for (int x = 0; x < w; x++) {
				DWORD pix = 0;
				memcpy(&pix, row + static_cast<size_t>(x) * srcBpp, srcBpp);
				BYTE r = scale((pix & rMask) >> rShift, rBits);
				BYTE g = scale((pix & gMask) >> gShift, gBits);
				BYTE b = scale((pix & bMask) >> bShift, bBits);
				BYTE a = aMask ? scale((pix & aMask) >> aShift, aBits) : 255;
				pPixelData[y * w + x] = ColorRGBA(r, g, b, a);
			}
		}
	} else {
		delete[] pPixelData;
		return NULL;
	}

	width = w;
	height = h;
	return (void*)pPixelData;
}
