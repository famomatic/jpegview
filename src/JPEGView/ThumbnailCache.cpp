#include "stdafx.h"
#include "ThumbnailCache.h"
#include "JPEGImage.h"
#include "LocalDensityCorr.h"
#include "SettingsProvider.h"
#include "png.h"
#include <zlib.h>
#include <MaxImageDef.h>
#include <vector>
#include <algorithm>
#include <mutex>

// FNV-1a 64-bit hash over a byte buffer, then truncated to 64 bits and
// rendered as 16 hex chars. Stable across builds and good enough as a
// collision-resistant cache key derived from file identity.
static uint64_t Fnv1a64(const void* data, size_t len) {
	uint64_t h = 14695981039346656037ULL;
	const uint8* p = (const uint8*)data;
	for (size_t i = 0; i < len; i++) {
		h ^= (uint64_t)p[i];
		h *= 1099511628211ULL;
	}
	return h;
}

// PNG tEXt chunk keys used to persist the original image dimensions so the
// caller can reconstruct the thumbnail image geometry without the source file.
static const char* kOrigWidthKey = "JPEGViewOrigWidth";
static const char* kOrigHeightKey = "JPEGViewOrigHeight";
static const char* kOrigChannelsKey = "JPEGViewOrigChannels";
static const char* kSignatureKey = "JPEGViewSig";
// Path-only hash (no size/mtime) so Invalidate() can find stale entries
// left by in-place edits that changed the file size/mtime.
static const char* kPathHashKey = "JPEGViewPathHash";
// Bumped whenever the on-disk format changes, so old cache files are ignored.
static const char* kSignatureValue = "JVTC1";

CThumbnailCache& CThumbnailCache::This() {
	static CThumbnailCache instance;
	return instance;
}

CThumbnailCache::CThumbnailCache()
	: m_bEnabled(false), m_nMaxBytes(0), m_bShutdown(false) {
	m_bEnabled = CSettingsProvider::This().ThumbnailCacheEnabled();
	// INI gives the limit in megabytes; convert to bytes. 0 means disabled.
	int nMB = CSettingsProvider::This().ThumbnailCacheMaxMB();
	m_nMaxBytes = static_cast<__int64>(nMB) * 1024 * 1024;
	if (m_nMaxBytes <= 0) m_bEnabled = false;
}

CThumbnailCache::~CThumbnailCache() {
	// Stop the async writer. Pending jobs are dropped (the cache is only an
	// optimization); a write already in progress completes so no torn file is
	// left behind (StoreEntry writes atomically via temp file + rename anyway).
	{
		std::lock_guard<std::mutex> lock(m_csJobs);
		m_bShutdown = true;
		m_jobs.clear();
	}
	m_cvJobs.notify_all();
	if (m_worker.joinable()) {
		m_worker.join();
	}
}

CString CThumbnailCache::MakeKey(LPCTSTR sFilePath, __int64 nFileSize, const FILETIME& lastModTime) const {
	CString sKeyInput(sFilePath);
	sKeyInput.MakeLower();

	// Build a compact identity blob: lowercased path + size + mtime.
	CString sIdentity;
	sIdentity.Format(_T("%s|%I64d|%u%u"), (LPCTSTR)sKeyInput, nFileSize,
		lastModTime.dwLowDateTime, lastModTime.dwHighDateTime);

	uint64_t h = Fnv1a64((LPCTSTR)sIdentity, sIdentity.GetLength() * sizeof(TCHAR));
	CString sHex;
	sHex.Format(_T("%016I64x"), h);
	return sHex;
}

// Path-only hash (lowercased, no size/mtime). Used as a PNG text chunk so
// Invalidate() can find and delete stale entries whose key (path+size+mtime)
// no longer matches after an in-place edit.
static CString MakePathHash(LPCTSTR sFilePath) {
	CString sPath(sFilePath);
	sPath.MakeLower();
	uint64_t h = Fnv1a64((LPCTSTR)sPath, sPath.GetLength() * sizeof(TCHAR));
	CString sHex;
	sHex.Format(_T("%016I64x"), h);
	return sHex;
}

LPCTSTR CThumbnailCache::CacheDir() const {
	// Lazily resolve + create the cache directory exactly once. This is
	// intentionally independent of m_csLock so that methods already holding
	// the lock can call CacheDir() without deadlocking on a non-reentrant
	// CCriticalSection.
	static std::once_flag s_once;
	std::call_once(s_once, [this]() {
	if (m_sCacheDir.IsEmpty()) {
		TCHAR szTemp[MAX_PATH];
		DWORD nLen = ::GetTempPath(MAX_PATH, szTemp);
		if (nLen == 0 || nLen >= MAX_PATH) {
			// Fallback to the per-user app data path if TEMP is unavailable.
			m_sCacheDir = CString(Helpers::JPEGViewAppDataPath()) + _T("thumbcache\\");
		} else {
			m_sCacheDir = CString(szTemp) + _T("JPEGView\\thumbcache\\");
		}
		// CreateDirectory is not recursive: create each path component in
		// turn so a missing intermediate dir (e.g. %TEMP%\JPEGView\) does not
		// make the final thumbcache\ creation fail with ERROR_PATH_NOT_FOUND.
		// CreateDirectory returns ERROR_ALREADY_EXISTS for existing dirs, which
		// we simply ignore.
		CString sBuild;
		for (int i = 0; m_sCacheDir[i] != 0; i++) {
			sBuild += m_sCacheDir[i];
			if (m_sCacheDir[i] == _T('\\')) {
				::CreateDirectory(sBuild, NULL);
			}
		}
	}
	});
	return m_sCacheDir;
}

bool CThumbnailCache::GetCacheFilePath(LPCTSTR sKey, CString& sOutCacheFile) const {
	if (sKey == NULL || *sKey == 0) return false;
	sOutCacheFile = CString(CacheDir()) + sKey + _T(".png");
	return true;
}

// ---- PNG encode/decode helpers (BGRA 32bpp) ------------------------------

static void PngWriteFromBGRA(png_structp png_ptr, png_infop info_ptr,
	int nWidth, int nHeight, const uint8* pBGRA) {
	png_set_IHDR(png_ptr, info_ptr, nWidth, nHeight, 8,
		PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png_ptr, info_ptr);
	png_set_bgr(png_ptr); // libpng stores RGB; we feed BGRA, swap R/B.

	// PNG expects per-row pointers. Allocate a small pointer table rather than
	// touching the caller's buffer layout.
	png_bytep* rows = (png_bytep*)malloc(nHeight * sizeof(png_bytep));
	if (rows == NULL) {
		png_error(png_ptr, "out of memory for row pointers");
		return;
	}
	for (int y = 0; y < nHeight; y++) {
		rows[y] = (png_bytep)(pBGRA + (size_t)y * nWidth * 4);
	}
	png_write_image(png_ptr, rows);
	png_write_end(png_ptr, info_ptr);
	free(rows);
}

// Custom write callback that accumulates into a growing malloc buffer, so we
// can encode the PNG fully in memory and then write it to disk in one pass
// (avoids leaving a partial cache file on disk if encoding fails).
struct MemWriteCtx {
	uint8* buffer;
	size_t size;
	size_t capacity;
};

static void PngMemWrite(png_structp png_ptr, png_bytep data, png_size_t len) {
	MemWriteCtx* ctx = (MemWriteCtx*)png_get_io_ptr(png_ptr);
	if (ctx == NULL) { png_error(png_ptr, "no write ctx"); return; }
	if (ctx->size + len > ctx->capacity) {
		size_t newCap = ctx->capacity * 2;
		if (newCap < ctx->size + len) newCap = ctx->size + len + 4096;
		uint8* p = (uint8*)realloc(ctx->buffer, newCap);
		if (p == NULL) { png_error(png_ptr, "out of memory"); return; }
		ctx->buffer = p;
		ctx->capacity = newCap;
	}
	memcpy(ctx->buffer + ctx->size, data, len);
	ctx->size += len;
}

static void PngMemFlush(png_structp) {}

static void PngReadFromMemory(png_structp png_ptr, png_bytep out, png_size_t len) {
	uint8** pp = (uint8**)png_get_io_ptr(png_ptr);
	memcpy(out, *pp, len);
	*pp += len;
}

// Decodes a PNG from memory into a freshly new[]'d BGRA buffer.
// Returns the pixel buffer (caller frees with delete[]) or NULL on failure.
// On success fills nWidth/nHeight. Also reads the original-dimension text
// chunks when present so the caller can reconstruct source geometry.
static uint8* DecodePngBGRAEx(const uint8* pBuffer, size_t nSize,
	int& nWidth, int& nHeight,
	int* pOrigWidth = NULL, int* pOrigHeight = NULL, int* pOrigChannels = NULL,
	CStringA* psSignature = NULL, CStringA* psPathHash = NULL) {
	if (png_sig_cmp((png_bytep)pBuffer, 0, 8) != 0) return NULL;

	png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (png_ptr == NULL) return NULL;
	png_infop info_ptr = png_create_info_struct(png_ptr);
	if (info_ptr == NULL) { png_destroy_read_struct(&png_ptr, NULL, NULL); return NULL; }

	uint8* pPixels = NULL;
	png_bytep* rows = NULL;

	if (setjmp(png_jmpbuf(png_ptr))) {
		delete[] pPixels; pPixels = NULL;
		free(rows); rows = NULL;
		png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
		return NULL;
	}

	const uint8* pCursor = pBuffer + 8; // skip signature
	png_set_read_fn(png_ptr, &pCursor, PngReadFromMemory);
	png_set_sig_bytes(png_ptr, 8);

	png_read_info(png_ptr, info_ptr);
	png_set_expand(png_ptr);
	png_set_strip_16(png_ptr);
	png_set_gray_to_rgb(png_ptr);
	png_set_add_alpha(png_ptr, 0xff, PNG_FILLER_AFTER);
	png_set_bgr(png_ptr);
	png_set_packing(png_ptr);
	(void)png_set_interlace_handling(png_ptr);
	png_read_update_info(png_ptr, info_ptr);

	nWidth = (int)png_get_image_width(png_ptr, info_ptr);
	nHeight = (int)png_get_image_height(png_ptr, info_ptr);
	if (nWidth <= 0 || nHeight <= 0) {
		png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
		return NULL;
	}
	if ((double)nWidth * nHeight > MAX_IMAGE_PIXELS) {
		png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
		return NULL;
	}

	size_t rowBytes = png_get_rowbytes(png_ptr, info_ptr);
	// Allocate with new[] so the buffer can be handed directly to CJPEGImage
	// (whose destructor uses delete[]) without an extra copy in TryGet().
	pPixels = new(std::nothrow) uint8[(size_t)nHeight * rowBytes];
	rows = (png_bytep*)malloc(nHeight * sizeof(png_bytep));
	if (pPixels == NULL || rows == NULL) {
		delete[] pPixels; free(rows);
		png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
		return NULL;
	}
	for (int y = 0; y < nHeight; y++) {
		rows[y] = pPixels + (size_t)y * rowBytes;
	}
	png_read_image(png_ptr, rows);
	png_read_end(png_ptr, info_ptr);

	// Pull the original-dimension metadata stored as text chunks.
	png_textp text = NULL;
	int num_text = 0;
	if (png_get_text(png_ptr, info_ptr, &text, &num_text) > 0) {
		for (int i = 0; i < num_text; i++) {
			if (text[i].key == NULL || text[i].text == NULL) continue;
			if (strcmp(text[i].key, kOrigWidthKey) == 0 && pOrigWidth) {
				*pOrigWidth = atoi(text[i].text);
			} else if (strcmp(text[i].key, kOrigHeightKey) == 0 && pOrigHeight) {
				*pOrigHeight = atoi(text[i].text);
			} else if (strcmp(text[i].key, kOrigChannelsKey) == 0 && pOrigChannels) {
				*pOrigChannels = atoi(text[i].text);
			} else if (strcmp(text[i].key, kSignatureKey) == 0 && psSignature) {
				*psSignature = CStringA(text[i].text);
			} else if (strcmp(text[i].key, kPathHashKey) == 0 && psPathHash) {
				*psPathHash = CStringA(text[i].text);
			}
		}
	}

	free(rows);
	png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
	return pPixels;
}

bool CThumbnailCache::TryGet(LPCTSTR sFilePath, __int64 nFileSize, const FILETIME& lastModTime,
	CJPEGImage*& ppThumbnail, int& nOrigWidth, int& nOrigHeight) {
	ppThumbnail = NULL;
	nOrigWidth = 0;
	nOrigHeight = 0;
	if (!m_bEnabled || sFilePath == NULL || *sFilePath == 0) return false;
	// Serialize against concurrent Put/Invalidate/EnforceSizeLimit from the
	// read-ahead loader thread.
	std::lock_guard<std::mutex> lock(m_csLock);

	CString sKey = MakeKey(sFilePath, nFileSize, lastModTime);
	CString sCacheFile;
	if (!GetCacheFilePath(sKey, sCacheFile)) return false;

	HANDLE hFile = ::CreateFile(sCacheFile, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return false;

	DWORD nSize = ::GetFileSize(hFile, NULL);
	if (nSize == INVALID_FILE_SIZE || nSize == 0) {
		::CloseHandle(hFile);
		return false;
	}
	uint8* pBuffer = (uint8*)malloc(nSize);
	if (pBuffer == NULL) {
		::CloseHandle(hFile);
		return false;
	}
	DWORD nRead = 0;
	BOOL bOk = ::ReadFile(hFile, pBuffer, nSize, &nRead, NULL);
	::CloseHandle(hFile);
	if (!bOk || nRead != nSize) {
		free(pBuffer);
		return false;
	}

	int nThumbW = 0, nThumbH = 0;
	int nOrigChannels = 4;
	CStringA sSignature;
	uint8* pPixels = DecodePngBGRAEx(pBuffer, nSize, nThumbW, nThumbH,
		&nOrigWidth, &nOrigHeight, &nOrigChannels, &sSignature);
	free(pBuffer);
	if (pPixels == NULL) return false;
	if (sSignature != kSignatureValue) {
		// Stale/incompatible cache entry - discard it.
		delete[] pPixels;
		::DeleteFile(sCacheFile);
		return false;
	}

	// Touch the file's last-access/write time so LRU eviction sees recent use.
	::SetFileAttributes(sCacheFile, FILE_ATTRIBUTE_NORMAL);
	HANDLE hTouch = ::CreateFile(sCacheFile, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hTouch != INVALID_HANDLE_VALUE) {
		FILETIME ftNow;
		::GetSystemTimeAsFileTime(&ftNow);
		::SetFileTime(hTouch, NULL, NULL, &ftNow);
		::CloseHandle(hTouch);
	}

	// DecodePngBGRA now allocates with new[], matching CJPEGImage's delete[]
	// ownership model, so the buffer can be handed over directly without a
	// copy.
	ppThumbnail = new CJPEGImage(nThumbW, nThumbH, pPixels, NULL, 4, 0,
		IF_CLIPBOARD, false, 0, 1, 0, NULL, true, NULL);

	// The decoded thumbnail is always 32bpp BGRA (png_set_add_alpha), so the
	// stored nOrigChannels is informational only. nOrigWidth/nOrigHeight are
	// returned for the caller but the cache key (path+size+mtime) already
	// guarantees they match the live image.
	return true;
}

void CThumbnailCache::Put(LPCTSTR sFilePath, __int64 nFileSize, const FILETIME& lastModTime,
	CJPEGImage* pThumbnail, int nOrigWidth, int nOrigHeight) {
	if (!m_bEnabled || pThumbnail == NULL) return;
	if (nOrigWidth <= 0 || nOrigHeight <= 0) return;

	int nThumbW = pThumbnail->OrigWidth();
	int nThumbH = pThumbnail->OrigHeight();
	if (nThumbW <= 0 || nThumbH <= 0) return;

	void* pSrc = pThumbnail->OriginalPixels();
	if (pSrc == NULL) return;
	int nChannels = pThumbnail->OriginalChannels();
	if (nChannels != 4) return; // thumbnail images are always 32bpp BGRA

	StoreEntry(sFilePath, nFileSize, lastModTime, (const unsigned char*)pSrc,
		nThumbW, nThumbH, nOrigWidth, nOrigHeight);
}

void CThumbnailCache::PutAsync(LPCTSTR sFilePath, int nThumbWidth, int nThumbHeight,
	const void* pBGRA, int nOrigWidth, int nOrigHeight) {
	if (!m_bEnabled || sFilePath == NULL || *sFilePath == 0 || pBGRA == NULL) return;
	if (nThumbWidth <= 0 || nThumbHeight <= 0 || nOrigWidth <= 0 || nOrigHeight <= 0) return;

	AsyncPutJob job;
	job.sFilePath = sFilePath;
	job.nThumbW = nThumbWidth;
	job.nThumbH = nThumbHeight;
	job.nOrigW = nOrigWidth;
	job.nOrigH = nOrigHeight;
	// Copy the pixels now - the caller's thumbnail can be freed at any time.
	size_t nBytes = (size_t)nThumbWidth * nThumbHeight * 4;
	job.pixels.assign((const unsigned char*)pBGRA, (const unsigned char*)pBGRA + nBytes);

	{
		std::lock_guard<std::mutex> lock(m_csJobs);
		if (m_bShutdown) return;
		// One pending entry per file is enough - a newer thumbnail for the
		// same path replaces the queued one.
		for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
			if (it->sFilePath.CompareNoCase(job.sFilePath) == 0) {
				*it = std::move(job);
				m_cvJobs.notify_one();
				return;
			}
		}
		m_jobs.push_back(std::move(job));
		if (!m_worker.joinable()) {
			m_worker = std::thread(&CThumbnailCache::WorkerLoop, this);
		}
	}
	m_cvJobs.notify_one();
}

void CThumbnailCache::WorkerLoop() {
	for (;;) {
		AsyncPutJob job;
		{
			std::unique_lock<std::mutex> lock(m_csJobs);
			m_cvJobs.wait(lock, [this]() { return m_bShutdown || !m_jobs.empty(); });
			if (m_bShutdown) return;
			job = std::move(m_jobs.front());
			m_jobs.pop_front();
		}
		// Resolve the source file identity (size + mtime) here on the worker,
		// so the paint thread never touches the file system.
		HANDLE hFile = ::CreateFile(job.sFilePath, GENERIC_READ, FILE_SHARE_READ, NULL,
			OPEN_EXISTING, 0, NULL);
		if (hFile == INVALID_HANDLE_VALUE) continue;
		FILETIME ftMod;
		LARGE_INTEGER liSize;
		bool bHaveStat = (::GetFileTime(hFile, NULL, NULL, &ftMod) != FALSE) &&
			(::GetFileSizeEx(hFile, &liSize) != FALSE);
		::CloseHandle(hFile);
		if (!bHaveStat || liSize.QuadPart <= 0) continue;

		StoreEntry(job.sFilePath, liSize.QuadPart, ftMod, job.pixels.data(),
			job.nThumbW, job.nThumbH, job.nOrigW, job.nOrigH);
	}
}

void CThumbnailCache::StoreEntry(LPCTSTR sFilePath, __int64 nFileSize, const FILETIME& lastModTime,
	const unsigned char* pBGRA, int nThumbWidth, int nThumbHeight, int nOrigWidth, int nOrigHeight) {
	// Hold the lock across encode + write + eviction so a concurrent TryGet
	// for the same key never sees a half-written temp file.
	std::lock_guard<std::mutex> lock(m_csLock);

	int nThumbW = nThumbWidth;
	int nThumbH = nThumbHeight;

	CString sKey = MakeKey(sFilePath, nFileSize, lastModTime);
	CString sCacheFile;
	if (!GetCacheFilePath(sKey, sCacheFile)) return;

	png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (png_ptr == NULL) return;
	png_infop info_ptr = png_create_info_struct(png_ptr);
	if (info_ptr == NULL) { png_destroy_write_struct(&png_ptr, NULL); return; }

	MemWriteCtx ctx = { NULL, 0, 0 };
	bool bFailed = false;

	if (setjmp(png_jmpbuf(png_ptr))) {
		bFailed = true;
	} else {
		png_set_write_fn(png_ptr, &ctx, PngMemWrite, PngMemFlush);

		// Store original geometry + format signature as text chunks so a cached
		// file is self-describing and can be invalidated by bumping the value.
		png_text text[5];
		memset(text, 0, sizeof(text));
		char szW[16], szH[16], szC[16], szPathHash[32];
		sprintf_s(szW, "%d", nOrigWidth);
		sprintf_s(szH, "%d", nOrigHeight);
		sprintf_s(szC, "%d", 4); // thumbnail pixels are always 32bpp BGRA
		// Path-only hash so Invalidate() can find this entry even after an
		// in-place edit changed the file size/mtime (and thus the cache key).
		CString sPathHash = MakePathHash(sFilePath);
		sprintf_s(szPathHash, "%S", (LPCTSTR)sPathHash);
		text[0].compression = PNG_TEXT_COMPRESSION_NONE;
		text[0].key = (png_charp)kOrigWidthKey;
		text[0].text = szW;
		text[1].compression = PNG_TEXT_COMPRESSION_NONE;
		text[1].key = (png_charp)kOrigHeightKey;
		text[1].text = szH;
		text[2].compression = PNG_TEXT_COMPRESSION_NONE;
		text[2].key = (png_charp)kOrigChannelsKey;
		text[2].text = szC;
		text[3].compression = PNG_TEXT_COMPRESSION_NONE;
		text[3].key = (png_charp)kSignatureKey;
		text[3].text = (png_charp)kSignatureValue;
		text[4].compression = PNG_TEXT_COMPRESSION_NONE;
		text[4].key = (png_charp)kPathHashKey;
		text[4].text = szPathHash;
		png_set_text(png_ptr, info_ptr, text, 5);

		PngWriteFromBGRA(png_ptr, info_ptr, nThumbW, nThumbH, pBGRA);
	}

	png_destroy_write_struct(&png_ptr, &info_ptr);

	if (!bFailed && ctx.buffer != NULL && ctx.size > 0) {
		// Write atomically: temp file then rename, so a crash mid-write cannot
		// leave a half-written cache entry that would fail to decode later.
	CString sTemp = sCacheFile + _T(".tmp");
	HANDLE hFile = ::CreateFile(sTemp, GENERIC_WRITE, 0, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile != INVALID_HANDLE_VALUE) {
		DWORD nWritten = 0;
		BOOL bOk = ::WriteFile(hFile, ctx.buffer, (DWORD)ctx.size, &nWritten, NULL);
		::CloseHandle(hFile);
		if (bOk && nWritten == (DWORD)ctx.size) {
			::SetFileAttributes(sCacheFile, FILE_ATTRIBUTE_NORMAL);
			BOOL bMove = ::MoveFileEx(sTemp, sCacheFile, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
			if (!bMove) {
				::DeleteFile(sTemp);
			}
			} else {
				::DeleteFile(sTemp);
			}
		}
	}
	free(ctx.buffer);

	EnforceSizeLimit();
}

void CThumbnailCache::Invalidate(LPCTSTR sFilePath) {
	if (!m_bEnabled || sFilePath == NULL) return;
	std::lock_guard<std::mutex> lock(m_csLock);

	// An in-place edit (e.g. lossless JPEG transform) changes the file size
	// and/or mtime, so the cache key (path+size+mtime) no longer matches the
	// old entry. Scan all cache files and delete any whose stored path-only
	// hash matches, so stale entries don't linger until LRU eviction.
	CStringA sTargetPathHashA;
	{
		CString sPathHash = MakePathHash(sFilePath);
		sTargetPathHashA = CStringA(sPathHash);
	}

	CString sDir(CacheDir());
	CString sPattern = sDir + _T("*.png");
	WIN32_FIND_DATA fd;
	HANDLE hFind = ::FindFirstFile(sPattern, &fd);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
			CString sCacheFile = sDir + fd.cFileName;
			HANDLE hFile = ::CreateFile(sCacheFile, GENERIC_READ, FILE_SHARE_READ, NULL,
				OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (hFile == INVALID_HANDLE_VALUE) continue;
			DWORD nSize = ::GetFileSize(hFile, NULL);
			if (nSize == INVALID_FILE_SIZE || nSize == 0) {
				::CloseHandle(hFile);
				continue;
			}
			uint8* pBuffer = (uint8*)malloc(nSize);
			if (pBuffer == NULL) {
				::CloseHandle(hFile);
				continue;
			}
			DWORD nRead = 0;
			BOOL bOk = ::ReadFile(hFile, pBuffer, nSize, &nRead, NULL);
			::CloseHandle(hFile);
			if (!bOk || nRead != nSize) {
				free(pBuffer);
				continue;
			}
			// Decode just to read the text chunks; discard the pixels.
			int nW = 0, nH = 0;
			CStringA sStoredPathHash;
			uint8* pPixels = DecodePngBGRAEx(pBuffer, nSize, nW, nH, NULL, NULL, NULL, NULL, &sStoredPathHash);
			free(pBuffer);
			if (pPixels != NULL) delete[] pPixels;
			if (sStoredPathHash == sTargetPathHashA) {
				::SetFileAttributes(sCacheFile, FILE_ATTRIBUTE_NORMAL);
				::DeleteFile(sCacheFile);
			}
		} while (::FindNextFile(hFind, &fd));
		::FindClose(hFind);
	}

	// Also delete the entry matching the current file stat (the common case
	// where the file hasn't been edited since caching).
	__int64 nFileSize = 0;
	HANDLE hFile = ::CreateFile(sFilePath, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, 0, NULL);
	if (hFile != INVALID_HANDLE_VALUE) {
		FILETIME ftMod;
		BOOL bGotTime = ::GetFileTime(hFile, NULL, NULL, &ftMod);
		LARGE_INTEGER li;
		if (::GetFileSizeEx(hFile, &li)) nFileSize = li.QuadPart;
		::CloseHandle(hFile);
		if (bGotTime && nFileSize > 0) {
			CString sKey = MakeKey(sFilePath, nFileSize, ftMod);
			CString sCacheFile;
			if (GetCacheFilePath(sKey, sCacheFile)) {
				::SetFileAttributes(sCacheFile, FILE_ATTRIBUTE_NORMAL);
				::DeleteFile(sCacheFile);
			}
		}
	}
}

void CThumbnailCache::EnforceSizeLimit() {
	if (m_nMaxBytes <= 0) return;
	// Caller (Put) already holds m_csLock; this method is private and only
	// called from there, so it does not re-acquire the lock (CCriticalSection
	// is non-reentrant and would deadlock).

	CString sDir(CacheDir());
	CString sPattern = sDir + _T("*.png");

	WIN32_FIND_DATA fd;
	HANDLE hFind = ::FindFirstFile(sPattern, &fd);
	if (hFind == INVALID_HANDLE_VALUE) return;

	struct CacheEntry {
		CString sPath;
		__int64 nSize;
		FILETIME ftMtime;
	};
	std::vector<CacheEntry> entries;
	__int64 nTotal = 0;
	do {
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
		CString sPath = sDir + fd.cFileName;
		__int64 nSize = (__int64)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
		nTotal += nSize;
		CacheEntry e;
		e.sPath = sPath;
		e.nSize = nSize;
		e.ftMtime = fd.ftLastWriteTime;
		entries.push_back(e);
	} while (::FindNextFile(hFind, &fd));
	::FindClose(hFind);

	if (nTotal <= m_nMaxBytes) return;

	// Sort oldest first (by mtime) and delete until within budget.
	std::sort(entries.begin(), entries.end(), [](const CacheEntry& a, const CacheEntry& b) {
		LONG cmp = ::CompareFileTime(&a.ftMtime, &b.ftMtime);
		if (cmp != 0) return cmp < 0;
		return a.sPath.Compare(b.sPath) < 0;
	});

	for (const auto& e : entries) {
		if (nTotal <= m_nMaxBytes) break;
		::SetFileAttributes(e.sPath, FILE_ATTRIBUTE_NORMAL);
		if (::DeleteFile(e.sPath)) {
			nTotal -= e.nSize;
		}
	}
}
