
#pragma once

#include <vector>

class AvifReader
{
public:
	// Returns data in 4 byte BGRA
	static void* ReadImage(int& width,   // width of the image loaded.
		int& height,  // height of the image loaded.
		int& bpp,     // BYTES (not bits) PER PIXEL.
		bool& has_animation,     // if the image is animated
		int frame_index, // index of frame
		int& frame_count, // number of frames
		int& frame_time, // frame duration in milliseconds
		void*& exif_chunk, // Pointer to Exif data (must be freed by caller)
		bool& outOfMemory, // set to true when no memory to read image
		const void* buffer, // memory address containing jxl compressed data.
		int sizebytes); // size of jxl compressed data

	static void DeleteCache();

	// Compress 24-bit BGR DIB (padded to 4-byte boundary) into AVIF.
	// quality: 0-100. Returns malloc'd buffer (caller frees with free()).
	static void* Compress(const void* pBGRData, int nWidth, int nHeight, size_t& nSize, int nQuality);

private:
	struct avif_cache;
	static avif_cache cache;
};
