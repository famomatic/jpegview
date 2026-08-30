// Verifies SvgReader rasterizes both plain SVG and gzip-wrapped SVGZ.
//
// The SVGZ path is easy to break silently: a failed inflate just makes the
// image fail to load, which looks identical to an unsupported file. This test
// builds the compressed form in memory with the same zlib the reader uses, so a
// regression shows up as a failing test rather than an empty viewer window.

#include "stdafx.h"
#include "SVGWrapper.h"

#include <turbojpeg.h>
#include <zlib.h>
#include <stdio.h>
#include <string>
#include <vector>

namespace {

// A minimal document exercising the pieces JPEGView cares about: a shape, a CSS
// class, and a text run whose font comes from that class.
const char* SVG_SOURCE =
	"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
	"<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 64 32\">\n"
	"<style type=\"text/css\">.a{fill:#FF0000;} .b{font-family:'Arial';font-size:12px;}</style>\n"
	"<rect class=\"a\" x=\"0\" y=\"0\" width=\"64\" height=\"32\"/>\n"
	"<text class=\"b\" x=\"2\" y=\"20\">Hi</text>\n"
	"</svg>\n";

int g_nFailures = 0;

void Check(bool bCondition, const char* strWhat) {
	if (bCondition) {
		printf("  ok   %s\n", strWhat);
	} else {
		printf("  FAIL %s\n", strWhat);
		g_nFailures++;
	}
}

std::wstring TempFilePath(LPCWSTR strSuffix) {
	WCHAR szDir[MAX_PATH] = { 0 };
	::GetTempPath(_countof(szDir), szDir);
	WCHAR szName[MAX_PATH] = { 0 };
	::swprintf_s(szName, L"%sjpegview_svgtest_%lu%s", szDir, ::GetCurrentProcessId(), strSuffix);
	return szName;
}

bool WriteFileBytes(const std::wstring& sPath, const void* pData, size_t nSize) {
	HANDLE hFile = ::CreateFile(sPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return false;
	DWORD nWritten = 0;
	BOOL bOk = ::WriteFile(hFile, pData, (DWORD)nSize, &nWritten, NULL);
	::CloseHandle(hFile);
	return bOk && nWritten == nSize;
}

// Compresses with gzip framing, exactly what a .svgz file holds.
bool GzipCompress(const char* pSrc, size_t nSrcSize, std::vector<char>& out) {
	z_stream strm;
	::memset(&strm, 0, sizeof(strm));
	// 16 + MAX_WBITS writes a gzip header instead of a zlib one.
	if (::deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8,
		Z_DEFAULT_STRATEGY) != Z_OK) {
		return false;
	}

	out.resize(::deflateBound(&strm, (uLong)nSrcSize) + 32);
	strm.next_in = (Bytef*)pSrc;
	strm.avail_in = (uInt)nSrcSize;
	strm.next_out = (Bytef*)&out[0];
	strm.avail_out = (uInt)out.size();

	int nResult = ::deflate(&strm, Z_FINISH);
	size_t nProduced = out.size() - strm.avail_out;
	::deflateEnd(&strm);
	if (nResult != Z_STREAM_END) return false;

	out.resize(nProduced);
	return true;
}

// A document large enough that the inflated form does not fit the reader's
// first output buffer, which forces the grow-and-retry path in the inflater.
// Highly compressible on purpose, so the compressed size stays far below the
// decompressed one - exactly the shape a real Illustrator .svgz has.
std::string BuildLargeSvg(int nShapes) {
	std::string s;
	s.reserve((size_t)nShapes * 120 + 256);
	s += "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
	s += "<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 64 32\">\n";
	s += "<style type=\"text/css\">.a{fill:#FF0000;} .b{font-family:'Arial';font-size:12px;}</style>\n";
	for (int i = 0; i < nShapes; i++) {
		char szLine[160];
		::sprintf_s(szLine,
			"<rect class=\"a\" x=\"%d\" y=\"%d\" width=\"3\" height=\"3\"/>\n",
			i % 64, (i / 64) % 32);
		s += szLine;
	}
	s += "<text class=\"b\" x=\"2\" y=\"20\">Hi</text>\n</svg>\n";
	return s;
}

// Rasterizes one file and reports whether it produced a plausible image.
bool RenderFile(const std::wstring& sPath, int& width, int& height) {
	int nBpp = 0;
	bool bOutOfMemory = false;
	width = 0;
	height = 0;
	void* pPixels = SvgReader::ReadImage(width, height, nBpp, bOutOfMemory, sPath.c_str());
	if (pPixels == NULL) {
		printf("       ReadImage returned NULL (outOfMemory=%d)\n", (int)bOutOfMemory);
		return false;
	}
	bool bOk = (nBpp == 4) && width > 0 && height > 0;
	delete[] (unsigned char*)pPixels;
	return bOk;
}

} // namespace

int main(int argc, char** argv) {
	printf("SvgReaderTest\n");

	// Force TurboJPEG into this executable. libjpeg-turbo's bundled spng used
	// to archive a private zlib into turbojpeg.lib, mixing its stream functions
	// with ZLIB::ZLIB and making otherwise valid SVGZ files fail to inflate.
	tjhandle hTurboJpeg = ::tj3Init(TJINIT_DECOMPRESS);
	Check(hTurboJpeg != NULL, "TurboJPEG initializes alongside SVG zlib");
	if (hTurboJpeg != NULL) ::tj3Destroy(hTurboJpeg);

	// Given a path, just rasterize that file. Handy for checking a real-world
	// document against the reader without going through the viewer UI.
	if (argc > 1) {
		WCHAR szPath[MAX_PATH * 4] = { 0 };
		::MultiByteToWideChar(CP_ACP, 0, argv[1], -1, szPath, _countof(szPath));
		int w = 0, h = 0;
		bool bOk = RenderFile(szPath, w, h);
		printf("  %ws -> %s (%dx%d)\n", szPath, bOk ? "ok" : "FAILED", w, h);
		return bOk && g_nFailures == 0 ? 0 : 1;
	}

	const size_t nSourceSize = ::strlen(SVG_SOURCE);

	std::wstring sPlain = TempFilePath(L".svg");
	std::wstring sCompressed = TempFilePath(L".svgz");

	if (!WriteFileBytes(sPlain, SVG_SOURCE, nSourceSize)) {
		printf("  FAIL could not write %ws\n", sPlain.c_str());
		return 1;
	}

	std::vector<char> compressed;
	if (!GzipCompress(SVG_SOURCE, nSourceSize, compressed)) {
		printf("  FAIL gzip compression failed\n");
		::DeleteFile(sPlain.c_str());
		return 1;
	}
	if (!WriteFileBytes(sCompressed, &compressed[0], compressed.size())) {
		printf("  FAIL could not write %ws\n", sCompressed.c_str());
		::DeleteFile(sPlain.c_str());
		return 1;
	}

	printf("  plain %zu bytes, gzip %zu bytes\n", nSourceSize, compressed.size());

	int nPlainW = 0, nPlainH = 0;
	Check(RenderFile(sPlain, nPlainW, nPlainH), "plain .svg rasterizes");

	int nGzipW = 0, nGzipH = 0;
	Check(RenderFile(sCompressed, nGzipW, nGzipH), "gzip .svgz rasterizes");

	Check(nPlainW == nGzipW && nPlainH == nGzipH,
		"both forms produce the same dimensions");

	::DeleteFile(sPlain.c_str());
	::DeleteFile(sCompressed.c_str());

	// Same again with a document whose inflated size exceeds the inflater's
	// initial buffer several times over.
	std::string sLarge = BuildLargeSvg(6000);
	std::vector<char> largeCompressed;
	if (!GzipCompress(sLarge.c_str(), sLarge.length(), largeCompressed)) {
		printf("  FAIL gzip compression failed for the large document\n");
		return 1;
	}
	printf("  large plain %zu bytes, gzip %zu bytes (ratio %.1fx)\n",
		sLarge.length(), largeCompressed.size(),
		(double)sLarge.length() / (double)largeCompressed.size());

	std::wstring sLargePlain = TempFilePath(L"_large.svg");
	std::wstring sLargeGzip = TempFilePath(L"_large.svgz");
	if (WriteFileBytes(sLargePlain, sLarge.c_str(), sLarge.length()) &&
		WriteFileBytes(sLargeGzip, &largeCompressed[0], largeCompressed.size())) {
		int nLargePlainW = 0, nLargePlainH = 0;
		Check(RenderFile(sLargePlain, nLargePlainW, nLargePlainH), "large plain .svg rasterizes");
		int nLargeGzipW = 0, nLargeGzipH = 0;
		Check(RenderFile(sLargeGzip, nLargeGzipW, nLargeGzipH),
			"large gzip .svgz rasterizes (exercises the inflate grow path)");
		Check(nLargePlainW == nLargeGzipW && nLargePlainH == nLargeGzipH,
			"large forms produce the same dimensions");
	} else {
		printf("  FAIL could not write the large test files\n");
		g_nFailures++;
	}
	::DeleteFile(sLargePlain.c_str());
	::DeleteFile(sLargeGzip.c_str());

	printf("%s\n", g_nFailures == 0 ? "PASSED" : "FAILED");
	return g_nFailures == 0 ? 0 : 1;
}
