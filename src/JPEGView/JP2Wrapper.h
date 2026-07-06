#pragma once

class Jp2Reader
{
public:
	// Returns data in 4-byte BGRA
	static void* ReadImage(int& width, int& height, int& bpp, bool& outOfMemory,
		const void* buffer, int sizebytes);
};
