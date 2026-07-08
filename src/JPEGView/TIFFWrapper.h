#pragma once

class CJPEGImage;

// TIFF reader backed by libtiff (supports classic TIFF and BigTIFF, all
// compression modes, 1/8/16/32-bit per sample, multi-page documents).
// Replaces the GDI+/WIC fallback which cannot decode BigTIFF (> 4 GB) files.
class TiffReader
{
public:
	// Reads a full TIFF document via libtiff.
	// Returns the decoded frame at nFrameIndex (0-based) as a CJPEGImage
	// owned by the caller, or NULL on failure / out of memory.
	// bOutOfMemory is set to true when the image exceeds the configured
	// dimension/pixel limits or memory could not be allocated.
	static CJPEGImage* ReadImage(LPCTSTR strFileName, int nFrameIndex, bool& bOutOfMemory);

	// Releases any per-file cached state (e.g. an open TIFF handle reused
	// across frames of the same multi-page document).
	static void ReleaseCache();
};