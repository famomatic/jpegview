#pragma once

class HdrReader
{
public:
	// Returns data in 4-byte BGRA. HDR values are tone-mapped to 8-bit.
	static void* ReadImage(int& width, int& height, int& bpp, bool& outOfMemory,
		const void* buffer, int sizebytes);
};
