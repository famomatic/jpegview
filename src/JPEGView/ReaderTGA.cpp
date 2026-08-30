#include "StdAfx.h"
#include "ReaderTGA.h"
#include "JPEGImage.h"
#include "Helpers.h"
#include "BasicProcessing.h"
#include "MaxImageDef.h"
#include <memory>

// The TGA reader has been adapted and extended from the TGA reader used in an example of the BOINC project
// http://www.filewatcher.com/p/boinc-server-maker_7.0.27+dfsg-5_armhf.deb.5191030/usr/share/doc/boinc-server-maker/examples/tgalib.h.html
// The reader supports 8, 16, 24 and 32 bit TGAs

// The following types are supported
#define TGA_INDEXED	 1		// This tells us it's a indexed 8 bit file
#define TGA_RGB		 2		// This tells us it's a normal RGB (really BGR) file
#define TGA_MONO	 3		// This tells us it's a monochrome file
#define TGA_RLE_INDEXED		9   // This tells us it's a indexed 8 bit Run-Length Encoded (RLE) file
#define TGA_RLE_RGB		10		// This tells us that the targa is Run-Length Encoded (RLE) RGB file
#define TGA_RLE_MONO	11		// This tells us that the targa is Run-Length Encoded (RLE) monochrome file

#define ALPHA_OPAQUE 0xFF000000


CJPEGImage* CReaderTGA::ReadTgaImage(LPCTSTR strFileName, COLORREF backgroundColor, bool& bOutOfMemory) {

	// backgroundColor is retained in the signature for ABI compatibility but no longer used:
	// transparency is preserved and composited at render time.
	(void)backgroundColor;

	bOutOfMemory = false;

	WORD width = 0, height = 0;			// The dimensions of the image
	WORD colormapStart = 0;             // First color-map entry index
	WORD colormapLen = 0;               // Number of color-map entries
	byte colormapBits = 0;              // Bits per color map entry
	byte length = 0;					// The length in bytes to the pixels
	byte imageType = 0;					// The image type (RLE, RGB, Alpha...)
	byte bits = 0;						// The bits per pixel for the image (16, 24, 32)
	byte attributes = 0;                // Image attributes
	FILE *pFile = NULL;					// The file pointer
	int channels = 0;					// The channels of the image (3 = RGA : 4 = RGBA)
	int stride = 0;						// The stride (channels * width)
	__int64 i = 0;						// A counter (64-bit: width*height can exceed INT_MAX for 65535x65535)

	// Open a file pointer to the targa file and check if it was found and opened 
	if((pFile = _tfopen(strFileName, _T("rb"))) == NULL) 
	{
		return NULL;
	}
	std::unique_ptr<FILE, decltype(&fclose)> fileGuard(pFile, &fclose);
	auto readExact = [pFile](void* target, size_t bytes) {
		return bytes == 0 || fread(target, 1, bytes, pFile) == bytes;
	};

	// Read in the length in bytes from the header to the pixel data
	if (!readExact(&length, sizeof(length))) return NULL;
	
	// Jump over one byte
	if (fseek(pFile, 1, SEEK_CUR) != 0) return NULL;

	// Read in the imageType (RLE, RGB, etc...)
	if (!readExact(&imageType, sizeof(imageType))) return NULL;

	bool isIndexed = imageType == TGA_INDEXED || imageType == TGA_RLE_INDEXED;

	// Read in palette info
	if (!readExact(&colormapStart, sizeof(colormapStart)) ||
		!readExact(&colormapLen, sizeof(colormapLen)) ||
		!readExact(&colormapBits, sizeof(colormapBits))) return NULL;
	
	// Skip past general information we don't care about
	if (fseek(pFile, 4, SEEK_CUR) != 0) return NULL;

	// Read the width, height and bits per pixel (16, 24 or 32)
	if (!readExact(&width, sizeof(width)) || !readExact(&height, sizeof(height)) ||
		!readExact(&bits, sizeof(bits)) || !readExact(&attributes, sizeof(attributes))) return NULL;

	bool flipVertically = ((attributes >> 5) & 1) == 0;
	bool flipHorizontally = (attributes & 0x10) != 0;

	// check preconditions: 
	// - image size valid
	// - supported bits per pixel - only 8, 15, 16, 24 and 32 bits supported
	// - no strange image attributes
	// - supported image types - only RGB, mono and indexed
	// - color map format must be 256 RGB entries
	if (width <= 0 || height <= 0 || width > MAX_IMAGE_DIMENSION || height > MAX_IMAGE_DIMENSION ||
		(bits != 8 && bits != 16 && bits != 15 && bits != 24 && bits != 32) ||
		(imageType != TGA_INDEXED && imageType != TGA_RGB && imageType != TGA_MONO && imageType != TGA_RLE_INDEXED && imageType != TGA_RLE_RGB && imageType != TGA_RLE_MONO) ||
		((imageType == TGA_MONO || imageType == TGA_RLE_MONO) && bits != 8) ||
		(isIndexed && (colormapStart != 0 || colormapLen != 256 || colormapBits != 24)))
	{
		return NULL;
	}

	// check memory footprint
	int targetChannels = (bits == 32) ? 4 : 3;
	int targetStride = Helpers::DoPadding(width * targetChannels, 4);
	// 64-bit byte count: on x64 dims may reach 65535 and the pixel guard allows
	// up to 1e12 pixels, so targetStride*height easily exceeds 32 bits. A 32-bit
	// product would wrap and under-allocate, causing a heap overflow below.
	__int64 numberOfBytesRequired = (__int64)targetStride * height;

	if ((double)width * height > MAX_IMAGE_PIXELS ||
		static_cast<unsigned long long>(numberOfBytesRequired) > SIZE_MAX)
	{
		bOutOfMemory = true;
		return NULL;
	}
	
	// Allocate memory which will hold our image data
	auto imageData = std::unique_ptr<byte[]>(new(std::nothrow) byte[static_cast<size_t>(numberOfBytesRequired)]);
	if (!imageData)
	{
		bOutOfMemory = true;
		return NULL;
	}
	byte* pImageData = imageData.get();
	memset(pImageData, 0, static_cast<size_t>(numberOfBytesRequired));

	// Now we move the file pointer to the pixel data
	if (fseek(pFile, length, SEEK_CUR) != 0)
	{
		return NULL;
	}

	// read the color map
	byte palette[768];
	if (isIndexed)
	{  
		if (!readExact(palette, sizeof(palette))) return NULL;
	}

	byte* pImage = pImageData;

	if(imageType == TGA_MONO)
	{
		// this is always 8 bpp (checked above)
		for(int y = 0; y < height; y++)
		{
			uint8* pLine = pImage;
			for(int x = 0; x < width; x++)
			{
				byte grey;
				if (!readExact(&grey, sizeof(grey))) return NULL;
				*pLine++ = grey;
				*pLine++ = grey;
				*pLine++ = grey;
			}
			pImage += targetStride;
		}
	}
	else if(imageType == TGA_INDEXED)
	{
		for(int y = 0; y < height; y++)
		{
			uint8* pLine = pImage;
			for(int x = 0; x < width; x++)
			{
				byte index;
				if (!readExact(&index, sizeof(index))) return NULL;
				*pLine++ = palette[index*3];
				*pLine++ = palette[index*3 + 1];
				*pLine++ = palette[index*3 + 2];
			}
			pImage += targetStride;
		}
	}
	else if(imageType == TGA_RGB)
	{
		// Check if the image is a 24 or 32-bit image
		if(bits == 24 || bits == 32)
		{
			// Calculate the channels (3 or 4)
			// Next, we calculate the stride and allocate enough memory for on pixel line.
			channels = bits / 8;
			stride = channels * width;

			// Load in all the pixel data line by line
			for(int y = 0; y < height; y++)
			{
				if (!readExact(pImage, static_cast<size_t>(stride))) return NULL;
				pImage += targetStride;
			}
		}
		// Check if the image is a 16 bit image (RGB stored in 1 unsigned short)
		else if(bits == 16 || bits == 15)
		{
			unsigned short pixel = 0;
			int r=0, g=0, b=0;

			// Since we convert 16-bit images to 24 bit, we hardcode the channels to 3.
			// We then calculate the stride and allocate memory for the pixels.
			channels = 3;
			stride = channels * width;

			// Load in all the pixel data pixel by pixel
			for(int y = 0; y < height; y++)
			{
				uint8* pLine = pImage;
				for(int i = 0; i < width; i++)
				{
					// Read in the current pixel
					if (!readExact(&pixel, sizeof(pixel))) return NULL;
				
					// To convert a 16-bit pixel into an R, G, B, we need to
					// do some masking and such to isolate each color value.
					// 0x1f = 11111 in binary, so since 5 bits are reserved in
					// each unsigned short for the R, G and B, we bit shift and mask
					// to find each value.  We then bit shift up by 3 to get the full color.
					b = (pixel & 0x1f) << 3;
					g = ((pixel >> 5) & 0x1f) << 3;
					r = ((pixel >> 10) & 0x1f) << 3;
				
					*pLine++ = b;
					*pLine++ = g;
					*pLine++ = r;
				}
				pImage += targetStride;
			}
		}
	}
	// Else, it must be Run-Length Encoded (RLE)
	else
	{
		// First, let me explain real quickly what RLE is.  
		// For further information, check out Paul Bourke's intro article at: 
		// http://astronomy.swin.edu.au/~pbourke/dataformats/rle/
		// 
		// Anyway, we know that RLE is a basic type compression.  It takes
		// colors that are next to each other and then shrinks that info down
		// into the color and a integer that tells how much of that color is used.
		// For instance:
		// aaaaabbcccccccc would turn into a5b2c8
		// Well, that's fine and dandy and all, but how is it down with RGB colors?
		// Simple, you read in an color count (rleID), and if that number is less than 128,
		// it does NOT have any optimization for those colors, so we just read the next
		// pixels normally.  Say, the color count was 28, we read in 28 colors like normal.
		// If the color count is over 128, that means that the next color is optimized and
		// we want to read in the same pixel color for a count of (colorCount - 127).
		// It's 127 because we add 1 to the color count, as you'll notice in the code.

		// Create some variables to hold the rleID, current colors read, channels, & stride.
		byte rleID = 0;
		// 64-bit byte offset into pImage: targetStride*height can exceed INT_MAX
		// for large images, so a 32-bit offset would wrap and write out of bounds.
		__int64 colorsRead = 0;
		channels = bits <= 16 ? 2 : bits / 8;
		int x = 0;
		int padding = targetStride - targetChannels * width;

		uint8 colors[4];
		byte* pColors = (byte*)colors;
		byte* pImage = pImageData;

		// Load in all the pixel data
		__int64 numPixels = (__int64)width*height;
		while(i < numPixels)
		{
			// Read in the current color count + 1
			if (!readExact(&rleID, sizeof(rleID))) return NULL;
			
			// Check if we don't have an encoded string of colors
			bool useSameColor;
			if(rleID < 128)
			{
				// Increase the count by 1
				rleID++;
				useSameColor = false;
			}
			// Else, let's read in a string of the same character
			else
			{
				// Minus the 128 ID + 1 (127) to get the color count that needs to be read
				rleID -= 127;
				useSameColor = true;

				// Read in the current color, which is the same for a while
				if (!readExact(pColors, static_cast<size_t>(channels))) return NULL;
			}

			// Go through and read all the unique colors found
			while(rleID && i < numPixels)
			{
				if (!useSameColor)
				{
					// Read in the current color
					if (!readExact(pColors, static_cast<size_t>(channels))) return NULL;
				}

				if(bits == 32)
				{
					memcpy(pImage + colorsRead, pColors, sizeof(uint32));
				}
				else
				{
					if (bits == 8)
					{
						if (isIndexed)
						{
							int index = pColors[0] * 3;
							pImage[colorsRead + 0] = palette[index];
							pImage[colorsRead + 1] = palette[index + 1];
							pImage[colorsRead + 2] = palette[index + 2];
						} else {
							pImage[colorsRead + 0] = pColors[0];
							pImage[colorsRead + 1] = pColors[0];
							pImage[colorsRead + 2] = pColors[0];
						}
					} else if (bits == 15 || bits == 16) {
						const uint16 pixel = static_cast<uint16>(pColors[0] | (pColors[1] << 8));
						pImage[colorsRead + 0] = static_cast<byte>(((pixel & 0x1F) * 255 + 15) / 31);
						pImage[colorsRead + 1] = static_cast<byte>((((pixel >> 5) & 0x1F) * 255 + 15) / 31);
						pImage[colorsRead + 2] = static_cast<byte>((((pixel >> 10) & 0x1F) * 255 + 15) / 31);
					} else {
						pImage[colorsRead + 0] = pColors[0];
						pImage[colorsRead + 1] = pColors[1];
						pImage[colorsRead + 2] = pColors[2];
					}

					// do padding target lines when 24 bpp
					x++;
					if (x == width)
					{
						x = 0;
						colorsRead += padding;
					}
				}

				// Increase the current pixels read, decrease the amount
				// of pixels left, and increase the starting index for the next pixel.
				i++;
				rleID--;
				colorsRead += targetChannels;
			}
				
		} // end of RLE pixel loop
	}

	// If image needs to be flipped, do this inplace
	if (flipVertically)
	{
		CBasicProcessing::MirrorVInplace(width, height, targetStride, pImageData);
	}
	if (flipHorizontally) {
		for (int y = 0; y < height; ++y) {
			byte* row = pImageData + static_cast<size_t>(y) * targetStride;
			for (int x = 0; x < width / 2; ++x) {
				for (int c = 0; c < targetChannels; ++c) std::swap(row[x * targetChannels + c], row[(width - 1 - x) * targetChannels + c]);
			}
		}
	}

	// 32 bit image - check alpha channel for validity and multiply RGB with alpha if valid
	if (targetChannels == 4)
	{
		uint32* pImage32 = (uint32*)pImageData;
		if ((attributes & 0x0F) != 0)
		{
			// Alpha channel is valid - preserve it. Background is composited at render time.
		}
		else
		{
			// no valid alpha channel - set all A to 255. Split the read-modify-
			// write from the increment: `*p++ = *p | X` reads and writes *p in one
			// unsequenced expression, which is undefined behavior.
			for (__int64 i = 0; i < (__int64)width*height; i++)
			{
				*pImage32 = *pImage32 | ALPHA_OPAQUE;
				pImage32++;
			}
		}
	}

	CJPEGImage* pTargetImage = new CJPEGImage(width, height, imageData.release(), NULL, targetChannels,
		0, IF_TGA, false, 0, 1, 0);

	return pTargetImage;
}
