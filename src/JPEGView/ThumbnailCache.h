#pragma once

#include "Helpers.h"
#include <mutex>

// Forward declaration to avoid heavy include in header
class CJPEGImage;

// Persistent on-disk thumbnail cache.
//
// JPEGView creates a small "thumbnail" image (downsampled copy of the original
// pixels) on first request - used by the zoom navigator, print preview and
// save dialog. Creating it requires decoding the full image and running the
// local density correction, which is expensive for large RAW/EXR/PSD files.
//
// This cache stores those downsampled originals to disk so revisiting a file is
// instant. Cache files live under the user's TEMP directory
// (%TEMP%\JPEGView\thumbcache\) so they never pollute the image folder and are
// reclaimed by the OS on disk cleanup. A simple size-based LRU eviction keeps
// the cache bounded.
class CThumbnailCache
{
public:
	// Singleton access.
	static CThumbnailCache& This();

	// Returns true and fills ppThumbnail with a NEWLY allocated thumbnail
	// image (caller takes ownership) if a valid cache entry exists for the
	// given file signature. Returns false on miss / disabled / error.
	// nOrigWidth/nOrigHeight receive the original image dimensions stored
	// alongside the thumbnail (needed by callers to reconstruct geometry).
	bool TryGet(LPCTSTR sFilePath, __int64 nFileSize, const FILETIME& lastModTime,
		CJPEGImage*& ppThumbnail, int& nOrigWidth, int& nOrigHeight);

	// Stores the given thumbnail image (downsampled original pixels) for the
	// given file signature together with the original image dimensions.
	// Does nothing when the cache is disabled.
	void Put(LPCTSTR sFilePath, __int64 nFileSize, const FILETIME& lastModTime,
		CJPEGImage* pThumbnail, int nOrigWidth, int nOrigHeight);

	// Removes any cached entry for the given file (e.g. after the image was
	// edited in place). Safe to call when disabled.
	void Invalidate(LPCTSTR sFilePath);

private:
	CThumbnailCache();
	~CThumbnailCache();
	CThumbnailCache(const CThumbnailCache&);
	CThumbnailCache& operator=(const CThumbnailCache&);

	// Builds the full path to the cache file for the given key and returns it
	// in sOutCacheFile. Also ensures the cache directory exists.
	bool GetCacheFilePath(LPCTSTR sKey, CString& sOutCacheFile) const;

	// Returns the root cache directory, creating it on first call.
	LPCTSTR CacheDir() const;

	// Enforces the configured size limit by deleting least-recently-used
	// cache files until the total size is within the budget.
	void EnforceSizeLimit();

	// Computes the lookup key from file identity (path + size + mtime).
	// The key is a stable 16-char hex string, safe to use as a file name.
	CString MakeKey(LPCTSTR sFilePath, __int64 nFileSize, const FILETIME& lastModTime) const;

	mutable CString m_sCacheDir;
	bool m_bEnabled;
	__int64 m_nMaxBytes;
	// Guards all cache file I/O and m_sCacheDir lazy init: CreateThumbnailImage()
	// can be reached from both the UI thread and the read-ahead loader thread,
	// so Put/TryGet/Invalidate/EnforceSizeLimit must be serialized.
	mutable std::mutex m_csLock;
};
