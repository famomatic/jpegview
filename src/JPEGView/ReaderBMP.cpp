#include "StdAfx.h"
#include "ReaderBMP.h"
#include "JPEGImage.h"
#include "Helpers.h"
#include "BasicProcessing.h"
#include "MaxImageDef.h"
#include <memory>

//////////////////////////////////////////////////////////////////////////////////
// BITMAP reading

struct BMHEADER {
	unsigned short int type;                 /* Magic identifier            */
	unsigned int size;                       /* File size in bytes          */
	unsigned short int reserved1, reserved2;
	unsigned int offset;                     /* Offset to image data, bytes */
};

struct BMINFOHEADER {
	unsigned int size;               /* Header size in bytes      */
	int width,height;                /* Width and height of image */
	unsigned short int planes;       /* Number of color planes    */
	unsigned short int bits;         /* Bits per pixel            */
	unsigned int compression;        /* Compression type          */
	unsigned int imagesize;          /* Image size in bytes       */
	int xresolution,yresolution;     /* Pixels per meter          */
	unsigned int ncolors;            /* Number of colors          */
	unsigned int importantcolors;    /* Important colors          */
};

static bool ReadUShort(FILE* file, uint16* pUShort) {
	return fread(pUShort, sizeof(uint16), 1, file) == 1;
}

static bool ReadUInt(FILE* file, uint32* pUInt) {
	return fread(pUInt, sizeof(uint32), 1, file) == 1;
}

CJPEGImage* CReaderBMP::ReadBmpImage(LPCTSTR strFileName, bool& bOutOfMemory) {
	BMHEADER header;
	BMINFOHEADER infoheader;
	FILE *fptr;

	bOutOfMemory = false;

	/* Open file */
	if ((fptr = _tfopen(strFileName,_T("rb"))) == NULL) {
		return NULL;
	}
	std::unique_ptr<FILE, decltype(&fclose)> fileGuard(fptr, &fclose);
	if (_fseeki64(fptr, 0, SEEK_END) != 0) return NULL;
	const __int64 actualFileSize = _ftelli64(fptr);
	if (actualFileSize < 14 + static_cast<__int64>(sizeof(BMINFOHEADER)) || _fseeki64(fptr, 0, SEEK_SET) != 0) return NULL;

	/* Read the header */
	if (!ReadUShort(fptr, &header.type) || !ReadUInt(fptr, &header.size) ||
		!ReadUShort(fptr, &header.reserved1) || !ReadUShort(fptr, &header.reserved2) ||
		!ReadUInt(fptr, &header.offset) || header.type != 0x4D42) return NULL;

	/* Read and check the information header */
	if (fread(&infoheader,sizeof(BMINFOHEADER),1,fptr) != 1) {
		return NULL;
	}
	if (infoheader.size != sizeof(BMINFOHEADER) || infoheader.planes != 1 || infoheader.compression != 0 ||
		header.offset < 14 + infoheader.size || header.offset > static_cast<uint64_t>(actualFileSize)) return NULL;
	/* Only 24 and 32 bpp */
	if (infoheader.bits != 24 && infoheader.bits != 32 && infoheader.bits != 8) {
		return NULL;
	}
	/* Not too big files */
	if (infoheader.height == INT_MIN || infoheader.width > static_cast<int>(MAX_IMAGE_DIMENSION) || infoheader.width <= 0 ||
		abs(infoheader.height) > static_cast<int>(MAX_IMAGE_DIMENSION) || infoheader.height == 0) {
		return NULL;
	}
	if ((double)infoheader.width * abs(infoheader.height) > MAX_IMAGE_PIXELS) {
		bOutOfMemory = true;
		return NULL;
	}

	// read palette for 8 bpp DIBs
	uint8 palette[4*256]{};
	if (infoheader.bits == 8) {
		const uint32 paletteEntries = infoheader.ncolors == 0 ? 256u : infoheader.ncolors;
		if (paletteEntries > 256 || 14ULL + infoheader.size + static_cast<uint64_t>(paletteEntries) * 4 > header.offset ||
			fseek(fptr, infoheader.size + 14, SEEK_SET) != 0 ||
			fread(palette, 4, paletteEntries, fptr) != paletteEntries) return NULL;
	}

	/* Seek to the start of the image data */
	if (_fseeki64(fptr, header.offset, SEEK_SET) != 0) return NULL;

	// DIBs are normally stored flipped vertically (meaning they are stored bottom-up)
	bool bFlipped;
	if (infoheader.height < 0) {
		infoheader.height = -infoheader.height;
		bFlipped = false;
	} else {
		bFlipped = true;
	}

	int bytesPerPixel = infoheader.bits/8;
	const size_t rowBytes = static_cast<size_t>(infoheader.width) * bytesPerPixel;
	const size_t paddedWidth = (rowBytes + 3) & ~static_cast<size_t>(3);
	const size_t fileSizeBytes = static_cast<size_t>(infoheader.height) * paddedWidth;
	if (fileSizeBytes == 0 || fileSizeBytes > MAX_BMP_FILE_SIZE ||
		fileSizeBytes > static_cast<uint64_t>(actualFileSize) - header.offset) {
		bOutOfMemory = fileSizeBytes > MAX_BMP_FILE_SIZE;
		return NULL; // corrupt or manipulated header
	}

	uint8* pDest = new(std::nothrow) uint8[fileSizeBytes];
	if (pDest == NULL) {
		bOutOfMemory = true;
		return NULL;
	}
	uint8* pStart = bFlipped ? pDest + paddedWidth*(infoheader.height-1) : pDest;
	for (int nLine = 0; nLine < infoheader.height; nLine++) {
		if (paddedWidth != fread(pStart, 1, paddedWidth, fptr)) {
			delete[] pDest;
			return NULL;
		}
		pStart = pStart + (bFlipped ? -paddedWidth : paddedWidth);
	}

	// Convert 8 bpp DIBs
	if (infoheader.bits == 8) {
		uint8* pTemp = (uint8*)CBasicProcessing::Convert8bppTo32bppDIB(infoheader.width, infoheader.height, pDest, palette);
		delete[] pDest;
		pDest = pTemp;
		infoheader.bits = 32;
	}

	// The CJPEGImage object gets ownership of the memory in pDest
	CJPEGImage* pImage = (pDest == NULL) ? NULL : new CJPEGImage(infoheader.width, infoheader.height, pDest, NULL, infoheader.bits/8, 
		0, IF_WindowsBMP, false, 0, 1, 0);

	bOutOfMemory = pImage == NULL;

	return pImage;
}
