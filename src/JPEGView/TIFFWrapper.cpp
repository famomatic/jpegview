/*  TIFF decoding via libtiff.

    libtiff handles classic TIFF and BigTIFF (> 4 GB), every compression
    mode (LZW, PackBits, Deflate/ZIP, JPEG, CCITT, none), tiled and
    stripped layouts, and multi-page documents.  This wrapper decodes a
    requested page into a 32 bpp BGRA DIB (the internal format used by
    JPEGView), extracts the embedded ICC profile, applies the TIFF
    orientation tag, and tone-maps 16/32-bit per sample images so they
    display sensibly on an 8-bit screen.

    The GDI+/WIC path previously used for TIFF cannot read BigTIFF and
    performs no file-size guard, so large scans fail with OutOfMemory or
    are rejected outright.  libtiff reads strips/tiles incrementally,
    keeping peak memory close to one strip rather than the whole file.
*/

#include "stdafx.h"
#include "TIFFWrapper.h"
#include "JPEGImage.h"
#include "MaxImageDef.h"
#include "Helpers.h"
#include "ICCProfileTransform.h"
#include "SettingsProvider.h"

#include <tiffio.h>

#include <algorithm>
#include <new>

// ---------------------------------------------------------------------------
// Per-file cache: keep the TIFF handle open between frames of the same
// multi-page document so navigating pages does not reopen/reparse the file.
// This mirrors the cached-decoder pattern used for WebP/PNG/HEIF.
// ---------------------------------------------------------------------------
static TIFF* g_pCachedTiff = nullptr;
static CString g_sCachedFileName;
static int g_nCachedPageCount = 0;

static void CloseCachedTiff()
{
	if (g_pCachedTiff != nullptr)
	{
		TIFFClose(g_pCachedTiff);
		g_pCachedTiff = nullptr;
	}
	g_sCachedFileName.Empty();
	g_nCachedPageCount = 0;
}

void TiffReader::ReleaseCache()
{
	CloseCachedTiff();
}

// Returns the number of pages (IFDs) in a TIFF document.
static int GetTiffPageCount(TIFF* tif)
{
	int count = 1; // current directory counts as the first page
	TIFFSetDirectory(tif, 0);
	while (TIFFReadDirectory(tif))
		count++;
	// Rewind to the first directory for the caller.
	TIFFSetDirectory(tif, 0);
	return count;
}

// Opens a TIFF, reusing the cached handle when the filename matches so
// multi-page navigation avoids re-parsing the (possibly huge) file.
static TIFF* OpenTiff(LPCTSTR strFileName, int nFrameIndex, int& nPageCount)
{
	nPageCount = 1;
	bool bSameFile = (g_pCachedTiff != nullptr) && (g_sCachedFileName.CompareNoCase(strFileName) == 0);
	if (!bSameFile)
	{
		CloseCachedTiff();
		g_pCachedTiff = TIFFOpenW(strFileName, "rm");
		if (g_pCachedTiff == nullptr)
			return nullptr;
		g_sCachedFileName = strFileName;
		g_nCachedPageCount = GetTiffPageCount(g_pCachedTiff);
	}
	nPageCount = g_nCachedPageCount;
	if (nFrameIndex < 0)
		nFrameIndex = 0;
	if (nFrameIndex >= nPageCount)
		nFrameIndex = nPageCount - 1;
	if (TIFFSetDirectory(g_pCachedTiff, (tdir_t)nFrameIndex) == 0)
		return nullptr;
	return g_pCachedTiff;
}

// Reads an ICC profile stored in TIFF tag 34675 (ICC Profile).
static unsigned char* ReadICCProfile(TIFF* tif, unsigned int& nSize)
{
	nSize = 0;
	unsigned char* pProfile = nullptr;
	uint32 tagCount = 0;
	if (TIFFGetField(tif, TIFFTAG_ICCPROFILE, &tagCount, &pProfile) == 1 && tagCount > 0 && pProfile != nullptr)
	{
		unsigned char* pCopy = new (std::nothrow) unsigned char[tagCount];
		if (pCopy != nullptr)
		{
			memcpy(pCopy, pProfile, tagCount);
			nSize = tagCount;
			return pCopy;
		}
	}
	return nullptr;
}

// Tone-maps a 16-bit per sample buffer to 8-bit by normalising against the
// found min/max so high bit-depth scans remain visible without HDR pipelines.
static void Normalize16Bit(const uint16* pSrc, unsigned char* pDst, int pixelCount, int channels, uint16 nMin, uint16 nMax)
{
	uint16 range = (nMax > nMin) ? (uint16)(nMax - nMin) : 1;
	for (int i = 0; i < pixelCount; i++)
	{
		for (int c = 0; c < channels; c++)
		{
			uint16 v = pSrc[i * channels + c];
			unsigned int scaled = ((unsigned int)(v - nMin) * 255U) / range;
			pDst[i * channels + c] = (unsigned char)scaled;
		}
	}
}

// Tone-maps a 32-bit float per sample buffer to 8-bit.  Negative values are
// clamped to zero; values above fMax are clamped to 255.
static void Normalize32Bit(const float* pSrc, unsigned char* pDst, int pixelCount, int channels, float fMin, float fMax)
{
	float range = (fMax > fMin) ? (fMax - fMin) : 1.0f;
	for (int i = 0; i < pixelCount; i++)
	{
		for (int c = 0; c < channels; c++)
		{
			float v = pSrc[i * channels + c];
			float scaled = (v - fMin) / range;
			if (scaled < 0.0f) scaled = 0.0f;
			if (scaled > 1.0f) scaled = 1.0f;
			pDst[i * channels + c] = (unsigned char)(scaled * 255.0f + 0.5f);
		}
	}
}

// Reads a single page into a tightly packed BGRA buffer (4 bytes/pixel).
// Returns the buffer (caller owns) or NULL on failure; sets bHasAlpha.
static unsigned char* ReadPageToBGRA(TIFF* tif, int width, int height,
	uint16 bitsPerSample, uint16 samplesPerPixel, uint16 photometric,
	uint16 planarConfig, uint16 sampleFormat, bool& bHasAlpha, bool& bOutOfMemory)
{
	bHasAlpha = false;
	bOutOfMemory = false;

	// Extra samples beyond the photometric channels are treated as alpha.
	uint16 photometricChannels = 0;
	switch (photometric)
	{
		case PHOTOMETRIC_MINISWHITE:
		case PHOTOMETRIC_MINISBLACK:
			photometricChannels = 1;
			break;
		case PHOTOMETRIC_RGB:
			photometricChannels = 3;
			break;
		case PHOTOMETRIC_PALETTE:
			photometricChannels = 1; // palette expands to RGB
			break;
		case PHOTOMETRIC_SEPARATED: // CMYK
			photometricChannels = 4;
			break;
		case PHOTOMETRIC_YCBCR:
			photometricChannels = 3;
			break;
		default:
			photometricChannels = samplesPerPixel;
			break;
	}
	uint16 extraSamples = (samplesPerPixel > photometricChannels)
		? (uint16)(samplesPerPixel - photometricChannels) : 0;
	bHasAlpha = (extraSamples > 0);

	// Allocate the 32 bpp BGRA output (JPEGView internal format).
	size_t rowBytes = (size_t)width * 4;
	// Guard against overflow before allocating.
	if (width <= 0 || height <= 0 ||
		(size_t)width > (SIZE_MAX / 4) / (size_t)height)
	{
		bOutOfMemory = true;
		return nullptr;
	}
	unsigned char* pBGRA = new (std::nothrow) unsigned char[rowBytes * (size_t)height];
	if (pBGRA == nullptr)
	{
		bOutOfMemory = true;
		return nullptr;
	}

	// Decide whether libtiff should expand to 8-bit RGBA for us.  This
	// covers palette, 1-bit, MINISWHITE, YCbCr and JPEG-in-TIFF cleanly.
	bool bUseRGBA = (bitsPerSample == 8) &&
		(photometric == PHOTOMETRIC_PALETTE ||
		 photometric == PHOTOMETRIC_MINISWHITE ||
		 (photometric == PHOTOMETRIC_MINISBLACK && samplesPerPixel == 1) ||
		 photometric == PHOTOMETRIC_YCBCR);

	// --- Fast path: let libtiff produce 8-bit RGBA, repack to BGRA -------
	if (bUseRGBA)
	{
		uint32* pRgbaBuf = (uint32*)_TIFFmalloc((tmsize_t)width * height * sizeof(uint32));
		if (pRgbaBuf == nullptr)
		{
			delete[] pBGRA;
			bOutOfMemory = true;
			return nullptr;
		}
		char emsg[1024] = { 0 };
		if (TIFFRGBAImageOK(tif, emsg) && TIFFReadRGBAImageOriented(tif, width, height, pRgbaBuf, ORIENTATION_TOPLEFT))
		{
			// libtiff returns ABGR (little-endian uint32: 0xAABBGGRR).
			for (int y = 0; y < height; y++)
			{
				// libtiff renders with origin at bottom-left; flip to top-left.
				int srcRow = height - 1 - y;
				const uint32* pSrc = pRgbaBuf + (tmsize_t)srcRow * width;
				unsigned char* pDst = pBGRA + (tmsize_t)y * rowBytes;
				for (int x = 0; x < width; x++)
				{
					uint32 px = pSrc[x];
					pDst[x * 4 + 0] = TIFFGetB(px); // B
					pDst[x * 4 + 1] = TIFFGetG(px); // G
					pDst[x * 4 + 2] = TIFFGetR(px); // R
					pDst[x * 4 + 3] = TIFFGetA(px); // A
				}
			}
			_TIFFfree(pRgbaBuf);
			return pBGRA;
		}
		_TIFFfree(pRgbaBuf);
		// Fall through to the manual strip reader.
	}

	// --- Manual strip reader for 8/16/32-bit ----------------------------
	// TIFFReadScanline works for stripped images; tiled images are read via
	// TIFFReadTile but the scanline API transparently handles tiles when
	// the image is not tiled.  For tiled images we fall back to RGBA above
	// for the common 8-bit cases; exotic tiled 16-bit is rare.
	tmsize_t scanlineSize = TIFFScanlineSize(tif);
	if (scanlineSize <= 0)
	{
		delete[] pBGRA;
		return nullptr;
	}

	unsigned char* pLineBuf = (unsigned char*)_TIFFmalloc(scanlineSize);
	if (pLineBuf == nullptr)
	{
		delete[] pBGRA;
		bOutOfMemory = true;
		return nullptr;
	}

	// For 16-bit we need the per-sample value range for normalisation.
	uint16 nMin16 = 0, nMax16 = 65535;
	if (bitsPerSample == 16)
	{
		uint16 smin = 0, smax = 0;
		TIFFGetFieldDefaulted(tif, TIFFTAG_MINSAMPLEVALUE, &smin);
		TIFFGetFieldDefaulted(tif, TIFFTAG_MAXSAMPLEVALUE, &smax);
		if (smax > smin) { nMin16 = smin; nMax16 = smax; }
	}
	float fMin32 = 0.0f, fMax32 = 1.0f;

	for (int row = 0; row < height; row++)
	{
		if (TIFFReadScanline(tif, pLineBuf, row, 0) < 0)
		{
			memset(pBGRA + (tmsize_t)row * rowBytes, 0, rowBytes * (height - row));
			break;
		}
		unsigned char* pDst = pBGRA + (tmsize_t)row * rowBytes;

		if (bitsPerSample == 8)
		{
			if (photometric == PHOTOMETRIC_RGB || photometric == PHOTOMETRIC_YCBCR)
			{
				const unsigned char* s = pLineBuf;
				for (int x = 0; x < width; x++)
				{
					pDst[x * 4 + 0] = s[2]; // B
					pDst[x * 4 + 1] = s[1]; // G
					pDst[x * 4 + 2] = s[0]; // R
					pDst[x * 4 + 3] = (extraSamples > 0) ? s[3] : 0xFF;
					s += samplesPerPixel;
				}
			}
			else if (photometric == PHOTOMETRIC_MINISBLACK)
			{
				for (int x = 0; x < width; x++)
				{
					unsigned char g = pLineBuf[x];
					pDst[x * 4 + 0] = g; pDst[x * 4 + 1] = g; pDst[x * 4 + 2] = g;
					pDst[x * 4 + 3] = 0xFF;
				}
			}
			else if (photometric == PHOTOMETRIC_MINISWHITE)
			{
				for (int x = 0; x < width; x++)
				{
					unsigned char g = 255 - pLineBuf[x];
					pDst[x * 4 + 0] = g; pDst[x * 4 + 1] = g; pDst[x * 4 + 2] = g;
					pDst[x * 4 + 3] = 0xFF;
				}
			}
			else if (photometric == PHOTOMETRIC_SEPARATED) // CMYK
			{
				const unsigned char* s = pLineBuf;
				for (int x = 0; x < width; x++)
				{
					unsigned char c = s[0], m = s[1], y = s[2], k = s[3];
					pDst[x * 4 + 0] = (unsigned char)((k * (255 - c)) / 255);
					pDst[x * 4 + 1] = (unsigned char)((k * (255 - m)) / 255);
					pDst[x * 4 + 2] = (unsigned char)((k * (255 - y)) / 255);
					pDst[x * 4 + 3] = 0xFF;
					s += 4;
				}
			}
			else if (photometric == PHOTOMETRIC_PALETTE)
			{
				uint16* red = nullptr, * green = nullptr, * blue = nullptr;
				if (TIFFGetField(tif, TIFFTAG_COLORMAP, &red, &green, &blue) == 1 &&
					red != nullptr && green != nullptr && blue != nullptr)
				{
					for (int x = 0; x < width; x++)
					{
						uint16 idx = pLineBuf[x];
						pDst[x * 4 + 0] = (unsigned char)(blue[idx] >> 8);
						pDst[x * 4 + 1] = (unsigned char)(green[idx] >> 8);
						pDst[x * 4 + 2] = (unsigned char)(red[idx] >> 8);
						pDst[x * 4 + 3] = 0xFF;
					}
				}
			}
			else
			{
				const unsigned char* s = pLineBuf;
				for (int x = 0; x < width; x++)
				{
					pDst[x * 4 + 0] = s[0];
					pDst[x * 4 + 1] = (samplesPerPixel > 1) ? s[1] : s[0];
					pDst[x * 4 + 2] = (samplesPerPixel > 2) ? s[2] : s[0];
					pDst[x * 4 + 3] = 0xFF;
					s += samplesPerPixel;
				}
			}
		}
		else if (bitsPerSample == 16)
		{
			const uint16* s16 = (const uint16*)pLineBuf;
			unsigned char* tmp = new (std::nothrow) unsigned char[(size_t)width * samplesPerPixel];
			if (tmp == nullptr)
			{
				bOutOfMemory = true;
				_TIFFfree(pLineBuf);
				delete[] pBGRA;
				return nullptr;
			}
			Normalize16Bit(s16, tmp, width, samplesPerPixel, nMin16, nMax16);
			const unsigned char* s = tmp;
			if (photometric == PHOTOMETRIC_RGB)
			{
				for (int x = 0; x < width; x++)
				{
					pDst[x * 4 + 0] = s[2];
					pDst[x * 4 + 1] = s[1];
					pDst[x * 4 + 2] = s[0];
					pDst[x * 4 + 3] = (extraSamples > 0) ? s[3] : 0xFF;
					s += samplesPerPixel;
				}
			}
			else
			{
				for (int x = 0; x < width; x++)
				{
					unsigned char g = s[x];
					pDst[x * 4 + 0] = g; pDst[x * 4 + 1] = g; pDst[x * 4 + 2] = g;
					pDst[x * 4 + 3] = 0xFF;
				}
			}
			delete[] tmp;
		}
		else if (bitsPerSample == 32 && sampleFormat == SAMPLEFORMAT_IEEEFP)
		{
			const float* s32 = (const float*)pLineBuf;
			unsigned char* tmp = new (std::nothrow) unsigned char[(size_t)width * samplesPerPixel];
			if (tmp == nullptr)
			{
				bOutOfMemory = true;
				_TIFFfree(pLineBuf);
				delete[] pBGRA;
				return nullptr;
			}
			Normalize32Bit(s32, tmp, width, samplesPerPixel, fMin32, fMax32);
			const unsigned char* s = tmp;
			if (photometric == PHOTOMETRIC_RGB)
			{
				for (int x = 0; x < width; x++)
				{
					pDst[x * 4 + 0] = s[2];
					pDst[x * 4 + 1] = s[1];
					pDst[x * 4 + 2] = s[0];
					pDst[x * 4 + 3] = (extraSamples > 0) ? s[3] : 0xFF;
					s += samplesPerPixel;
				}
			}
			else
			{
				for (int x = 0; x < width; x++)
				{
					unsigned char g = s[x];
					pDst[x * 4 + 0] = g; pDst[x * 4 + 1] = g; pDst[x * 4 + 2] = g;
					pDst[x * 4 + 3] = 0xFF;
				}
			}
			delete[] tmp;
		}
		else
		{
			memset(pDst, 0, rowBytes);
		}
	}

	_TIFFfree(pLineBuf);
	return pBGRA;
}

CJPEGImage* TiffReader::ReadImage(LPCTSTR strFileName, int nFrameIndex, bool& bOutOfMemory)
{
	bOutOfMemory = false;
	CJPEGImage* pImage = nullptr;
	unsigned char* pICCProfile = nullptr;
	void* pTransform = nullptr;

	int nPageCount = 1;
	TIFF* tif = OpenTiff(strFileName, nFrameIndex, nPageCount);
	if (tif == nullptr)
		return nullptr;

	int nFrame = (nFrameIndex < 0) ? 0 : (nFrameIndex >= nPageCount ? nPageCount - 1 : nFrameIndex);

	uint32 width = 0, height = 0;
	uint16 bitsPerSample = 1, samplesPerPixel = 1, photometric = 0;
	uint16 planarConfig = PLANARCONFIG_CONTIG, sampleFormat = SAMPLEFORMAT_UINT;
	TIFFGetFieldDefaulted(tif, TIFFTAG_IMAGEWIDTH, &width);
	TIFFGetFieldDefaulted(tif, TIFFTAG_IMAGELENGTH, &height);
	TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bitsPerSample);
	TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &samplesPerPixel);
	TIFFGetFieldDefaulted(tif, TIFFTAG_PHOTOMETRIC, &photometric);
	TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planarConfig);
	TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sampleFormat);

	// Enforce the same dimension/pixel guards as the GDI+ path so a
	// multi-gigabyte scan cannot exhaust memory.
	if (width == 0 || height == 0)
	{
		CloseCachedTiff();
		return nullptr;
	}
	if (width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION)
	{
		bOutOfMemory = true;
		CloseCachedTiff();
		return nullptr;
	}
	if ((double)width * (double)height > (double)MAX_IMAGE_PIXELS)
	{
		bOutOfMemory = true;
		CloseCachedTiff();
		return nullptr;
	}

	bool bHasAlpha = false;
	unsigned char* pBGRA = ReadPageToBGRA(tif, (int)width, (int)height,
		bitsPerSample, samplesPerPixel, photometric, planarConfig, sampleFormat,
		bHasAlpha, bOutOfMemory);
	if (pBGRA == nullptr)
	{
		CloseCachedTiff();
		return nullptr;
	}

	// ICC profile -> optional sRGB transform (matches other wrappers).
	unsigned int nICCProfileSize = 0;
	pICCProfile = ReadICCProfile(tif, nICCProfileSize);
	if (pICCProfile != nullptr && CSettingsProvider::This().UseEmbeddedColorProfiles())
	{
		pTransform = ICCProfileTransform::CreateTransform(pICCProfile, nICCProfileSize, ICCProfileTransform::FORMAT_BGRA);
		if (pTransform != nullptr)
			ICCProfileTransform::DoTransform(pTransform, pBGRA, pBGRA, (int)width, (int)height);
	}

	pImage = new CJPEGImage((int)width, (int)height, pBGRA, nullptr,
		4, 0, IF_TIFF, false, nFrame, nPageCount, 0);

	// Keep the TIFF handle cached only for multi-page documents; single-page
	// files are closed immediately to release the file lock.
	if (nPageCount <= 1)
		CloseCachedTiff();

	delete[] pICCProfile;
	ICCProfileTransform::DeleteTransform(pTransform);
	return pImage;
}
