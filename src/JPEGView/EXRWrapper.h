#pragma once

class ExrReader
{
public:
	// Returns data in 4-byte BGRA. HDR values are tone-mapped to 8-bit.
	static void* ReadImage(int& width, int& height, int& bpp, bool& outOfMemory,
		const void* buffer, int sizebytes);

	// Returns interleaved linear RGBA float pixels (no tone mapping), 4 floats per pixel.
	// Used for HDR display output. Caller must delete[] the returned buffer.
	static float* ReadImageFloat(int& width, int& height, bool& outOfMemory,
		const void* buffer, int sizebytes);
};
