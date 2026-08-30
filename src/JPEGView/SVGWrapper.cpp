#include "stdafx.h"
#include "SVGWrapper.h"
#include "MaxImageDef.h"

#include <thorvg.h>
#include <zlib.h>

#include <map>
#include <set>
#include <string>
#include <vector>

// SVG rendering is backed by ThorVG.  Unlike the NanoSVG rasterizer it replaces,
// ThorVG covers <text>/<tspan>, <use>, <clipPath>, <mask>, <pattern> and filters.
//
// Fonts are not discovered by ThorVG on its own: tvg::Text::font() matches the
// SVG's font-family against names that were registered up-front with
// tvg::Text::load().  The registry below maps installed font families to their
// files and registers only the families a given SVG actually asks for, so a
// document with two fonts does not pay for the several hundred installed ones.

namespace {

// ThorVG matches font names with a case-sensitive strcmp, so families are
// registered under the exact spelling the SVG uses.  Lookups into the installed
// font table are case-insensitive to bridge the two.
struct CaseInsensitiveLess {
	bool operator()(const std::wstring& a, const std::wstring& b) const {
		return ::_wcsicmp(a.c_str(), b.c_str()) < 0;
	}
};

typedef std::map<std::wstring, std::wstring, CaseInsensitiveLess> FontFileMap;

CRITICAL_SECTION g_csEngine;
bool g_bEngineReady = false;
FontFileMap* g_pInstalledFonts = NULL;
std::set<std::string>* g_pLoadedFonts = NULL;

// One-time initialization of the lock guarding all state in this file.
BOOL CALLBACK InitCriticalSectionOnce(PINIT_ONCE, PVOID, PVOID*) {
	::InitializeCriticalSection(&g_csEngine);
	return TRUE;
}

INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;

void EnterEngineLock() {
	::InitOnceExecuteOnce(&g_initOnce, InitCriticalSectionOnce, NULL, NULL);
	::EnterCriticalSection(&g_csEngine);
}

void LeaveEngineLock() {
	::LeaveCriticalSection(&g_csEngine);
}

// Strips the " (TrueType)" / " (OpenType)" suffix the font registry appends to
// the family name, e.g. "Arial Bold (TrueType)" -> "Arial Bold".
std::wstring StripFontTypeSuffix(const std::wstring& name) {
	size_t nParen = name.rfind(L" (");
	if (nParen != std::wstring::npos && !name.empty() && name[name.length() - 1] == L')') {
		return name.substr(0, nParen);
	}
	return name;
}

// SVG source is UTF-8; the font tables built from the registry are UTF-16.
std::wstring Utf8ToWide(const std::string& s) {
	if (s.empty()) return std::wstring();
	int nChars = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.length(), NULL, 0);
	if (nChars <= 0) return std::wstring();
	std::wstring out((size_t)nChars, L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.length(), &out[0], nChars);
	return out;
}

std::string WideToUtf8(LPCWSTR s) {
	int nSrc = (int)::wcslen(s);
	if (nSrc == 0) return std::string();
	int nBytes = ::WideCharToMultiByte(CP_UTF8, 0, s, nSrc, NULL, 0, NULL, NULL);
	if (nBytes <= 0) return std::string();
	std::string out((size_t)nBytes, '\0');
	::WideCharToMultiByte(CP_UTF8, 0, s, nSrc, &out[0], nBytes, NULL, NULL);
	return out;
}

bool HasExtension(const std::wstring& path, LPCWSTR ext) {
	size_t nDot = path.rfind(L'.');
	if (nDot == std::wstring::npos) return false;
	return ::_wcsicmp(path.c_str() + nDot, ext) == 0;
}

// Enumerates one of the font registry keys into the family -> file-path table.
void ReadFontRegistryKey(HKEY hRoot, LPCWSTR strSubKey, const std::wstring& sFontDir, FontFileMap& out) {
	HKEY hKey = NULL;
	if (::RegOpenKeyEx(hRoot, strSubKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return;

	for (DWORD dwIndex = 0; ; dwIndex++) {
		WCHAR szName[512];
		DWORD cchName = _countof(szName);
		BYTE bData[1024];
		DWORD cbData = sizeof(bData);
		DWORD dwType = 0;

		LONG lResult = ::RegEnumValue(hKey, dwIndex, szName, &cchName, NULL, &dwType, bData, &cbData);
		if (lResult == ERROR_NO_MORE_ITEMS) break;
		if (lResult != ERROR_SUCCESS) continue;
		if (dwType != REG_SZ || cbData < sizeof(WCHAR)) continue;

		std::wstring sFile((LPCWSTR)bData, cbData / sizeof(WCHAR));
		size_t nNul = sFile.find(L'\0');
		if (nNul != std::wstring::npos) sFile.resize(nNul);
		if (sFile.empty()) continue;

		// ThorVG's SFNT loader accepts only bare TTF and OTF; TrueType/OpenType
		// collections (.ttc/.otc) fail the SFNT magic check, so skip them here
		// rather than register a family that can never be resolved.
		if (!HasExtension(sFile, L".ttf") && !HasExtension(sFile, L".otf")) continue;

		// Registry values may hold a bare file name (relative to the system font
		// directory) or a full path, as used for per-user installed fonts.
		bool bAbsolute = sFile.find(L':') != std::wstring::npos || (sFile.size() > 1 && sFile[0] == L'\\');
		std::wstring sPath = bAbsolute ? sFile : (sFontDir + L"\\" + sFile);

		std::wstring sFamily = StripFontTypeSuffix(std::wstring(szName, cchName));
		if (sFamily.empty()) continue;

		// A family may be listed several times (one entry per face); the first
		// one wins, which is the regular face for the usual registry ordering.
		out.insert(std::make_pair(sFamily, sPath));
	}
	::RegCloseKey(hKey);
}

// Builds the installed-font table once.  Caller must hold the engine lock.
const FontFileMap& GetInstalledFonts() {
	if (g_pInstalledFonts != NULL) return *g_pInstalledFonts;

	g_pInstalledFonts = new FontFileMap();

	WCHAR szWindows[MAX_PATH] = { 0 };
	::GetWindowsDirectory(szWindows, _countof(szWindows));
	std::wstring sSystemFontDir = std::wstring(szWindows) + L"\\Fonts";

	ReadFontRegistryKey(HKEY_LOCAL_MACHINE,
		L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", sSystemFontDir, *g_pInstalledFonts);

	// Per-user fonts installed without administrator rights.
	WCHAR szLocalAppData[MAX_PATH] = { 0 };
	std::wstring sUserFontDir;
	if (::ExpandEnvironmentStrings(L"%LOCALAPPDATA%\\Microsoft\\Windows\\Fonts",
		szLocalAppData, _countof(szLocalAppData)) > 0) {
		sUserFontDir = szLocalAppData;
	}
	ReadFontRegistryKey(HKEY_CURRENT_USER,
		L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts",
		sUserFontDir.empty() ? sSystemFontDir : sUserFontDir, *g_pInstalledFonts);

	return *g_pInstalledFonts;
}

// Collects the font-family values referenced anywhere in the SVG source, both
// the presentation attribute (font-family="X") and the CSS property
// (font-family:X).  Values are returned with surrounding quotes removed and are
// kept byte-identical otherwise, because that is what ThorVG will compare.
void CollectFontFamilies(const char* pBuffer, size_t nSize, std::vector<std::string>& out) {
	static const char szKey[] = "font-family";
	const size_t nKeyLen = sizeof(szKey) - 1;
	std::set<std::string> seen;

	for (size_t i = 0; i + nKeyLen < nSize; i++) {
		if (::memcmp(pBuffer + i, szKey, nKeyLen) != 0) continue;

		size_t j = i + nKeyLen;
		while (j < nSize && (pBuffer[j] == ' ' || pBuffer[j] == '\t')) j++;
		if (j >= nSize || (pBuffer[j] != ':' && pBuffer[j] != '=')) continue;
		j++;
		while (j < nSize && (pBuffer[j] == ' ' || pBuffer[j] == '\t')) j++;

		// The value ends at the CSS/attribute terminator.  A quoted value keeps
		// its content verbatim, including spaces and commas.
		char cQuote = 0;
		if (j < nSize && (pBuffer[j] == '\'' || pBuffer[j] == '"')) {
			cQuote = pBuffer[j];
			j++;
		}
		size_t nStart = j;
		while (j < nSize) {
			char c = pBuffer[j];
			if (cQuote != 0) {
				if (c == cQuote) break;
			} else if (c == ';' || c == '}' || c == '"' || c == '\'' || c == '<' || c == '\n' || c == '\r') {
				break;
			}
			j++;
		}

		std::string sValue(pBuffer + nStart, j - nStart);
		while (!sValue.empty() && (sValue[sValue.length() - 1] == ' ' || sValue[sValue.length() - 1] == '\t')) {
			sValue.erase(sValue.length() - 1);
		}
		if (sValue.empty() || sValue.length() > 256) continue;
		if (seen.insert(sValue).second) out.push_back(sValue);

		i = j;
	}
}

// Registers one family with ThorVG under the exact name the SVG uses.
// Caller must hold the engine lock.
bool RegisterFont(const std::string& sName, const std::wstring& sPath) {
	if (g_pLoadedFonts->find(sName) != g_pLoadedFonts->end()) return true;

	HANDLE hFile = ::CreateFile(sPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return false;

	LARGE_INTEGER fileSize;
	if (!::GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0 || fileSize.QuadPart > 64 * 1024 * 1024) {
		::CloseHandle(hFile);
		return false;
	}

	DWORD nSize = (DWORD)fileSize.QuadPart;
	std::vector<char> data(nSize);
	DWORD nRead = 0;
	BOOL bOk = ::ReadFile(hFile, &data[0], nSize, &nRead, NULL);
	::CloseHandle(hFile);
	if (!bOk || nRead != nSize) return false;

	// copy = true hands ownership of the parsed data to ThorVG, so the local
	// buffer can go away with this scope.
	const char* pMimeType = HasExtension(sPath, L".otf") ? "otf" : "ttf";
	if (tvg::Text::load(sName.c_str(), &data[0], nSize, pMimeType, true) != tvg::Result::Success) {
		return false;
	}

	g_pLoadedFonts->insert(sName);
	return true;
}

// Registers the fonts this SVG references, plus one fallback so that text using
// an unavailable family still renders instead of disappearing.
void RegisterFontsForSvg(const char* pBuffer, size_t nSize) {
	std::vector<std::string> families;
	CollectFontFamilies(pBuffer, nSize, families);
	if (families.empty()) return;

	const FontFileMap& installed = GetInstalledFonts();

	for (size_t i = 0; i < families.size(); i++) {
		const std::string& sName = families[i];
		std::wstring sWideName = Utf8ToWide(sName);
		FontFileMap::const_iterator it = installed.find(sWideName);
		if (it != installed.end()) {
			RegisterFont(sName, it->second);
		}
	}

	// ThorVG falls back to whichever font was registered first when a family
	// cannot be resolved, so make sure at least one exists.
	if (g_pLoadedFonts->empty()) {
		static const LPCWSTR arrFallbacks[] = { L"Segoe UI", L"Arial", L"Tahoma" };
		for (int i = 0; i < _countof(arrFallbacks); i++) {
			FontFileMap::const_iterator it = installed.find(arrFallbacks[i]);
			if (it != installed.end() && RegisterFont(WideToUtf8(arrFallbacks[i]), it->second)) break;
		}
	}
}

// Brings up the ThorVG runtime once.  Worker threads are left at zero: JPEGView
// already rasterizes on its own load threads, and the engine is shared by all of
// them.  Caller must hold the engine lock.
bool EnsureEngine() {
	if (g_bEngineReady) return true;
	if (tvg::Initializer::init(0) != tvg::Result::Success) return false;
	g_pLoadedFonts = new std::set<std::string>();
	g_bEngineReady = true;
	return true;
}

// SVGZ is a gzip-wrapped SVG.  ThorVG's loader only understands the plain XML,
// so a compressed file is inflated here first.  Returns a new[] buffer that the
// caller owns, or NULL if the data is not gzip or does not inflate.
char* InflateGzip(const char* pSrc, size_t nSrcSize, size_t& nOutSize, bool& outOfMemory) {
	nOutSize = 0;
	outOfMemory = false;
	if (nSrcSize < 2 || (unsigned char)pSrc[0] != 0x1F || (unsigned char)pSrc[1] != 0x8B) return NULL;

	z_stream strm;
	::memset(&strm, 0, sizeof(strm));
	// 16 + MAX_WBITS selects gzip framing rather than raw zlib.
	if (::inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) return NULL;

	// Start at a modest multiple of the compressed size and grow as needed; the
	// inflated document is still bounded by the same cap as an uncompressed one.
	size_t nCapacity = nSrcSize * 4 + 8192;
	if (nCapacity > MAX_SVG_SOURCE_SIZE) nCapacity = MAX_SVG_SOURCE_SIZE;
	char* pOut = new(std::nothrow) char[nCapacity + 1];
	if (pOut == NULL) {
		::inflateEnd(&strm);
		outOfMemory = true;
		return NULL;
	}

	strm.next_in = (Bytef*)pSrc;
	strm.avail_in = (uInt)nSrcSize;
	strm.next_out = (Bytef*)pOut;
	strm.avail_out = (uInt)nCapacity;

	int nResult = Z_OK;
	for (;;) {
		nResult = ::inflate(&strm, Z_NO_FLUSH);
		if (nResult == Z_STREAM_END) break;
		if (nResult != Z_OK && nResult != Z_BUF_ERROR) break;
		if (strm.avail_out > 0 && nResult == Z_OK) continue;

		if (nCapacity >= MAX_SVG_SOURCE_SIZE) {
			nResult = Z_MEM_ERROR;
			break;
		}
		size_t nNewCapacity = nCapacity * 2;
		if (nNewCapacity > MAX_SVG_SOURCE_SIZE) nNewCapacity = MAX_SVG_SOURCE_SIZE;
		char* pGrown = new(std::nothrow) char[nNewCapacity + 1];
		if (pGrown == NULL) {
			outOfMemory = true;
			nResult = Z_MEM_ERROR;
			break;
		}
		size_t nProduced = nCapacity - strm.avail_out;
		::memcpy(pGrown, pOut, nProduced);
		delete[] pOut;
		pOut = pGrown;
		strm.next_out = (Bytef*)(pOut + nProduced);
		strm.avail_out = (uInt)(nNewCapacity - nProduced);
		nCapacity = nNewCapacity;
	}

	size_t nProduced = nCapacity - strm.avail_out;
	::inflateEnd(&strm);

	if (nResult != Z_STREAM_END || nProduced == 0) {
		delete[] pOut;
		return NULL;
	}

	pOut[nProduced] = 0;
	nOutSize = nProduced;
	return pOut;
}

// Everything shared between load threads - the engine refcount, the installed
// font table and ThorVG's font cache - is set up here under one lock.  The
// actual parse and rasterization afterwards touch only per-image objects and so
// run concurrently.
bool PrepareEngineAndFonts(const char* pBuffer, size_t nSize) {
	EnterEngineLock();
	bool bReady = EnsureEngine();
	if (bReady) RegisterFontsForSvg(pBuffer, nSize);
	LeaveEngineLock();
	return bReady;
}

} // namespace

// Reads an SVG file and rasterizes it to BGRA pixels.
// SVG is a vector format; JPEGView rasterizes it once at load time at the
// intrinsic size (or a fallback). Zoom re-rasterization is handled by the
// caller requesting a new target size via the load thread cache invalidation.
void* SvgReader::ReadImage(int& width, int& height, int& bpp, bool& outOfMemory,
	LPCTSTR strFileName, int targetWidth, int targetHeight)
{
	outOfMemory = false;
	bpp = 4;
	width = 0;
	height = 0;

	// Load the SVG file into memory
	HANDLE hFile = ::CreateFile(strFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return NULL;

	LARGE_INTEGER fileSize;
	if (!::GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart > MAX_SVG_FILE_SIZE) {
		::CloseHandle(hFile);
		return NULL;
	}

	size_t nSize = (size_t)fileSize.QuadPart;
	char* pBuffer = new(std::nothrow) char[nSize + 1];
	if (pBuffer == NULL) {
		::CloseHandle(hFile);
		outOfMemory = true;
		return NULL;
	}

	DWORD nBytesRead = 0;
	BOOL bOk = ::ReadFile(hFile, pBuffer, (DWORD)nSize, &nBytesRead, NULL);
	::CloseHandle(hFile);
	if (!bOk || nBytesRead != nSize) {
		delete[] pBuffer;
		return NULL;
	}
	pBuffer[nSize] = 0; // null-terminate; ThorVG expects a terminated buffer for text formats

	// .svgz (and .svg files that happen to be gzip-compressed) must be inflated
	// before ThorVG sees them.
	size_t nInflatedSize = 0;
	bool bInflateOom = false;
	char* pInflated = InflateGzip(pBuffer, nSize, nInflatedSize, bInflateOom);
	if (pInflated != NULL) {
		delete[] pBuffer;
		pBuffer = pInflated;
		nSize = nInflatedSize;
	} else if (bInflateOom) {
		delete[] pBuffer;
		outOfMemory = true;
		return NULL;
	}

	if (!PrepareEngineAndFonts(pBuffer, nSize)) {
		delete[] pBuffer;
		return NULL;
	}

	tvg::Picture* pPicture = tvg::Picture::gen();
	if (pPicture == NULL) {
		delete[] pBuffer;
		return NULL;
	}

	// copy = true: ThorVG keeps its own copy, so the file buffer is released below.
	if (pPicture->load(pBuffer, (uint32_t)nSize, "svg", NULL, true) != tvg::Result::Success) {
		tvg::Paint::rel(pPicture);
		delete[] pBuffer;
		return NULL;
	}
	delete[] pBuffer;

	float fIntrinsicW = 0.0f, fIntrinsicH = 0.0f;
	pPicture->size(&fIntrinsicW, &fIntrinsicH);

	// Determine rasterization size
	int w = (int)(fIntrinsicW + 0.5f);
	int h = (int)(fIntrinsicH + 0.5f);
	if (w <= 0 || h <= 0) {
		// No intrinsic size; use fallback
		w = 1024;
		h = 1024;
	}

	if (targetWidth > 0 && targetHeight > 0) {
		w = targetWidth;
		h = targetHeight;
	} else if (targetWidth > 0) {
		float scale = (float)targetWidth / (float)w;
		h = (int)(h * scale + 0.5f);
		w = targetWidth;
	} else if (targetHeight > 0) {
		float scale = (float)targetHeight / (float)h;
		w = (int)(w * scale + 0.5f);
		h = targetHeight;
	}

	// Clamp to max dimensions
	if (w > (int)MAX_IMAGE_DIMENSION) w = MAX_IMAGE_DIMENSION;
	if (h > (int)MAX_IMAGE_DIMENSION) h = MAX_IMAGE_DIMENSION;
	if (w < 1) w = 1;
	if (h < 1) h = 1;

	if ((double)w * h > MAX_IMAGE_PIXELS) {
		tvg::Paint::rel(pPicture);
		outOfMemory = true;
		return NULL;
	}

	unsigned char* pPixelData = new(std::nothrow) unsigned char[(size_t)w * h * 4];
	if (pPixelData == NULL) {
		tvg::Paint::rel(pPicture);
		outOfMemory = true;
		return NULL;
	}

	tvg::SwCanvas* pCanvas = tvg::SwCanvas::gen();
	if (pCanvas == NULL) {
		tvg::Paint::rel(pPicture);
		delete[] pPixelData;
		return NULL;
	}

	// ARGB8888S is 0xAARRGGBB per 32-bit word, i.e. B,G,R,A in memory on
	// little-endian: the BGRA layout JPEGView expects.  The 'S' variant keeps
	// the alpha un-premultiplied, matching what the rest of the pipeline assumes
	// for images that carry an alpha channel.
	bOk = pCanvas->target((uint32_t*)pPixelData, (uint32_t)w, (uint32_t)w, (uint32_t)h,
		tvg::ColorSpace::ARGB8888S) == tvg::Result::Success;

	if (bOk) {
		pPicture->size((float)w, (float)h);
		bOk = pCanvas->add(pPicture) == tvg::Result::Success;
		if (!bOk) tvg::Paint::rel(pPicture);
	} else {
		tvg::Paint::rel(pPicture);
	}

	// draw(true) clears the freshly allocated target before compositing.
	if (bOk) bOk = pCanvas->draw(true) == tvg::Result::Success;
	if (bOk) bOk = pCanvas->sync() == tvg::Result::Success;

	// The canvas owns the picture that was added to it.
	delete pCanvas;

	if (!bOk) {
		delete[] pPixelData;
		return NULL;
	}

	width = w;
	height = h;
	return (void*)pPixelData;
}
