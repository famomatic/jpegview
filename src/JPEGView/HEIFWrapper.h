#pragma once

#include "libheif/heif_cxx.h"

class HeifReader
{
public:
	// Returns data in the form 4-byte BGRA
	static void * ReadImage(int &width,   // width of the image loaded.
						 int &height,  // height of the image loaded.
						 int &bpp,     // BYTES (not bits) PER PIXEL.
						 int &frame_count, // number of top-level images
					     void* &exif_chunk, // Pointer to Exif data (must be freed by caller)
						 bool &outOfMemory, // set to true when no memory to read image
						 int frame_index, // index of requested frame
						 const void *buffer, // memory address containing heic compressed data.
						 int sizebytes); // size of heic compressed data.

	// Compress 24-bit BGR DIB (padded to 4-byte boundary) into HEIF/HEIC.
	// quality: 0-100. Returns malloc'd buffer (caller frees with free()).
	static void* Compress(const void* pBGRData, int nWidth, int nHeight, size_t& nSize, int nQuality);
};
