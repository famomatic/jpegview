#pragma once

// Reads and writes the XMP rating (xmp:Rating, 0..5 stars) of image files.
// For JPEG files the rating is stored in the embedded XMP APP1 segment,
// for all other formats in an XMP sidecar file (<name>.xmp) next to the image.
class CXMPRating
{
public:
	// Reads the rating of the given file: 0..5, or -1 if no rating is stored.
	// JPEG: embedded XMP first, sidecar as fallback. Other formats: sidecar only.
	static int GetRating(LPCTSTR sFileName);

	// Same as GetRating() but caches results per file path; used during navigation
	// filtering where the same files are queried repeatedly.
	static int GetCachedRating(LPCTSTR sFileName);

	// Writes the rating (1..5) or explicitly sets it to 0 (unrated).
	// Returns false if the file could not be written.
	static bool SetRating(LPCTSTR sFileName, int nRating);

private:
	CXMPRating();
};
