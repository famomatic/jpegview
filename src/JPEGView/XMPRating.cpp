#include "stdafx.h"

#include "XMPRating.h"

#include <map>
#include <string>

namespace {

const char* XMP_APP1_HEADER = "http://ns.adobe.com/xap/1.0/"; // followed by \0 in the segment
const int XMP_APP1_HEADER_LEN = 29; // including terminating \0

CRITICAL_SECTION& GetCacheCS() {
	static CRITICAL_SECTION cs = []() { CRITICAL_SECTION c; ::InitializeCriticalSection(&c); return c; }();
	return cs;
}

std::map<std::wstring, int>& GetRatingCache() {
	static std::map<std::wstring, int> cache;
	return cache;
}

bool IsJPEGFile(LPCTSTR sFileName) {
	LPCTSTR sExt = _tcsrchr(sFileName, _T('.'));
	return sExt != NULL && (_tcsicmp(sExt, _T(".jpg")) == 0 || _tcsicmp(sExt, _T(".jpeg")) == 0);
}

CString SidecarFileName(LPCTSTR sFileName) {
	CString sName(sFileName);
	int nPos = sName.ReverseFind(_T('.'));
	int nPosSlash = sName.ReverseFind(_T('\\'));
	if (nPos > nPosSlash) {
		sName = sName.Left(nPos);
	}
	return sName + _T(".xmp");
}

bool ReadWholeFile(LPCTSTR sFileName, std::string& contents) {
	HANDLE hFile = ::CreateFile(sFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return false;
	}
	LARGE_INTEGER fileSize;
	bool bOk = ::GetFileSizeEx(hFile, &fileSize) && fileSize.QuadPart > 0 && fileSize.QuadPart < 1024 * 1024 * 1024;
	if (bOk) {
		try {
			contents.resize((size_t)fileSize.QuadPart);
		} catch (...) {
			bOk = false;
		}
	}
	if (bOk) {
		char* pBuffer = &contents[0];
		__int64 nRemaining = fileSize.QuadPart;
		while (bOk && nRemaining > 0) {
			DWORD nToRead = (DWORD)min(nRemaining, (__int64)(64 * 1024 * 1024));
			DWORD nRead = 0;
			bOk = ::ReadFile(hFile, pBuffer, nToRead, &nRead, NULL) && nRead > 0;
			pBuffer += nRead;
			nRemaining -= nRead;
		}
	}
	::CloseHandle(hFile);
	if (!bOk) contents.clear();
	return bOk;
}

// Writes to a temporary file in the same directory, then atomically replaces the target
bool WriteWholeFileSafe(LPCTSTR sFileName, const std::string& contents) {
	CString sTempName = CString(sFileName) + _T(".jvtmp");
	HANDLE hFile = ::CreateFile(sTempName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return false;
	}
	bool bOk = true;
	const char* pBuffer = contents.data();
	__int64 nRemaining = (__int64)contents.size();
	while (bOk && nRemaining > 0) {
		DWORD nToWrite = (DWORD)min(nRemaining, (__int64)(64 * 1024 * 1024));
		DWORD nWritten = 0;
		bOk = ::WriteFile(hFile, pBuffer, nToWrite, &nWritten, NULL) && nWritten == nToWrite;
		pBuffer += nWritten;
		nRemaining -= nWritten;
	}
	::CloseHandle(hFile);
	if (bOk) {
		bOk = ::MoveFileEx(sTempName, sFileName, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
	}
	if (!bOk) {
		::DeleteFile(sTempName);
	}
	return bOk;
}

// Finds the XMP APP1 segment in a JPEG stream. Returns offset of the 0xFF marker byte
// and sets nSegmentLen to the total segment length (marker + length field + data), -1 if not found.
// If pnInsertOffset is given, it receives the best position to insert a new XMP segment.
int FindXMPSegment(const std::string& jpeg, int& nSegmentLen, int* pnInsertOffset = NULL) {
	nSegmentLen = 0;
	size_t nSize = jpeg.size();
	if (nSize < 4 || (unsigned char)jpeg[0] != 0xFF || (unsigned char)jpeg[1] != 0xD8) {
		return -1;
	}
	size_t nPos = 2;
	size_t nInsert = 2; // directly after SOI by default
	while (nPos + 4 <= nSize && (unsigned char)jpeg[nPos] == 0xFF) {
		unsigned char nMarker = (unsigned char)jpeg[nPos + 1];
		if (nMarker == 0xD8 || (nMarker >= 0xD0 && nMarker <= 0xD7) || nMarker == 0x01) {
			nPos += 2; // standalone marker without length
			continue;
		}
		if (nMarker == 0xDA) {
			break; // start of scan - metadata section over
		}
		int nLen = ((unsigned char)jpeg[nPos + 2] << 8) + (unsigned char)jpeg[nPos + 3];
		if (nLen < 2 || nPos + 2 + nLen > nSize) {
			break; // corrupt
		}
		if (nMarker == 0xE1 && nLen >= 2 + XMP_APP1_HEADER_LEN &&
			memcmp(&jpeg[nPos + 4], XMP_APP1_HEADER, XMP_APP1_HEADER_LEN) == 0) {
			nSegmentLen = 2 + nLen;
			if (pnInsertOffset != NULL) *pnInsertOffset = (int)nPos;
			return (int)nPos;
		}
		if (nMarker == 0xE0 || nMarker == 0xE1) {
			nInsert = nPos + 2 + nLen; // insert after JFIF APP0 / EXIF APP1
		}
		nPos += 2 + nLen;
	}
	if (pnInsertOffset != NULL) *pnInsertOffset = (int)nInsert;
	return -1;
}

// Extracts the rating from an XMP packet string, -1 if not present.
// Handles both attribute (xmp:Rating="3") and element (<xmp:Rating>3</xmp:Rating>) form.
int ParseRatingFromXMP(const std::string& xmp) {
	size_t nPos = xmp.find("xmp:Rating=\"");
	size_t nValue;
	if (nPos != std::string::npos) {
		nValue = nPos + strlen("xmp:Rating=\"");
	} else {
		nPos = xmp.find("<xmp:Rating>");
		if (nPos == std::string::npos) {
			return -1;
		}
		nValue = nPos + strlen("<xmp:Rating>");
	}
	if (nValue >= xmp.size()) {
		return -1;
	}
	int nRating = atoi(xmp.c_str() + nValue);
	return (nRating >= 0 && nRating <= 5) ? nRating : -1;
}

// Sets or inserts the rating in an existing XMP packet. Returns false if the packet
// has no place to put the rating (no rdf:Description element).
bool SpliceRatingIntoXMP(std::string& xmp, int nRating) {
	char buffer[16];
	sprintf_s(buffer, "%d", nRating);

	// attribute form
	size_t nPos = xmp.find("xmp:Rating=\"");
	if (nPos != std::string::npos) {
		size_t nStart = nPos + strlen("xmp:Rating=\"");
		size_t nEnd = xmp.find('"', nStart);
		if (nEnd == std::string::npos) return false;
		xmp.replace(nStart, nEnd - nStart, buffer);
		return true;
	}
	// element form
	nPos = xmp.find("<xmp:Rating>");
	if (nPos != std::string::npos) {
		size_t nStart = nPos + strlen("<xmp:Rating>");
		size_t nEnd = xmp.find("</xmp:Rating>", nStart);
		if (nEnd == std::string::npos) return false;
		xmp.replace(nStart, nEnd - nStart, buffer);
		return true;
	}
	// no rating yet - add as attribute to the first rdf:Description tag
	nPos = xmp.find("<rdf:Description");
	if (nPos == std::string::npos) {
		return false;
	}
	size_t nTagEnd = xmp.find('>', nPos);
	if (nTagEnd == std::string::npos) {
		return false;
	}
	if (nTagEnd > nPos && xmp[nTagEnd - 1] == '/') {
		nTagEnd--; // self-closing tag: insert before "/>"
	}
	std::string sInsert;
	if (xmp.find("xmlns:xmp=", nPos) == std::string::npos || xmp.find("xmlns:xmp=", nPos) > nTagEnd) {
		sInsert += " xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"";
	}
	sInsert += std::string(" xmp:Rating=\"") + buffer + "\"";
	xmp.insert(nTagEnd, sInsert);
	return true;
}

std::string CreateXMPPacket(int nRating) {
	char buffer[640];
	sprintf_s(buffer,
		"<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
		"<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"JPEGView\">\n"
		" <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
		"  <rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" xmp:Rating=\"%d\"/>\n"
		" </rdf:RDF>\n"
		"</x:xmpmeta>\n"
		"<?xpacket end=\"w\"?>", nRating);
	return std::string(buffer);
}

// Builds a complete JPEG APP1 segment (FF E1 len header payload) from an XMP packet
bool BuildXMPSegment(const std::string& xmp, std::string& segment) {
	size_t nPayload = XMP_APP1_HEADER_LEN + xmp.size();
	if (2 + nPayload > 65535) {
		return false; // does not fit in a single APP1 segment
	}
	int nLen = (int)(2 + nPayload);
	segment.clear();
	segment.reserve(4 + nPayload);
	segment.push_back((char)0xFF);
	segment.push_back((char)0xE1);
	segment.push_back((char)((nLen >> 8) & 0xFF));
	segment.push_back((char)(nLen & 0xFF));
	segment.append(XMP_APP1_HEADER, XMP_APP1_HEADER_LEN);
	segment.append(xmp);
	return true;
}

int GetRatingFromJPEG(LPCTSTR sFileName) {
	std::string jpeg;
	if (!ReadWholeFile(sFileName, jpeg)) {
		return -1;
	}
	int nSegmentLen;
	int nOffset = FindXMPSegment(jpeg, nSegmentLen);
	if (nOffset < 0) {
		return -1;
	}
	std::string xmp = jpeg.substr(nOffset + 4 + XMP_APP1_HEADER_LEN, nSegmentLen - 4 - XMP_APP1_HEADER_LEN);
	return ParseRatingFromXMP(xmp);
}

bool SetRatingInJPEG(LPCTSTR sFileName, int nRating) {
	std::string jpeg;
	if (!ReadWholeFile(sFileName, jpeg)) {
		return false;
	}
	int nSegmentLen;
	int nInsertOffset = 2;
	int nOffset = FindXMPSegment(jpeg, nSegmentLen, &nInsertOffset);
	std::string xmp;
	if (nOffset >= 0) {
		xmp = jpeg.substr(nOffset + 4 + XMP_APP1_HEADER_LEN, nSegmentLen - 4 - XMP_APP1_HEADER_LEN);
		if (!SpliceRatingIntoXMP(xmp, nRating)) {
			xmp = CreateXMPPacket(nRating);
		}
	} else {
		xmp = CreateXMPPacket(nRating);
	}
	std::string segment;
	if (!BuildXMPSegment(xmp, segment)) {
		return false;
	}
	if (nOffset >= 0) {
		jpeg.replace((size_t)nOffset, (size_t)nSegmentLen, segment);
	} else {
		jpeg.insert((size_t)nInsertOffset, segment);
	}
	return WriteWholeFileSafe(sFileName, jpeg);
}

int GetRatingFromSidecar(LPCTSTR sFileName) {
	std::string xmp;
	if (!ReadWholeFile(SidecarFileName(sFileName), xmp)) {
		return -1;
	}
	return ParseRatingFromXMP(xmp);
}

bool SetRatingInSidecar(LPCTSTR sFileName, int nRating) {
	CString sSidecar = SidecarFileName(sFileName);
	std::string xmp;
	if (ReadWholeFile(sSidecar, xmp)) {
		if (!SpliceRatingIntoXMP(xmp, nRating)) {
			xmp = CreateXMPPacket(nRating);
		}
	} else {
		xmp = CreateXMPPacket(nRating);
	}
	return WriteWholeFileSafe(sSidecar, xmp);
}

} // namespace

int CXMPRating::GetRating(LPCTSTR sFileName) {
	if (sFileName == NULL || sFileName[0] == 0) {
		return -1;
	}
	if (IsJPEGFile(sFileName)) {
		int nRating = GetRatingFromJPEG(sFileName);
		if (nRating >= 0) {
			return nRating;
		}
	}
	return GetRatingFromSidecar(sFileName);
}

int CXMPRating::GetCachedRating(LPCTSTR sFileName) {
	if (sFileName == NULL || sFileName[0] == 0) {
		return -1;
	}
	std::wstring key(sFileName);
	::EnterCriticalSection(&GetCacheCS());
	std::map<std::wstring, int>::iterator iter = GetRatingCache().find(key);
	bool bFound = iter != GetRatingCache().end();
	int nRating = bFound ? iter->second : -1;
	::LeaveCriticalSection(&GetCacheCS());
	if (bFound) {
		return nRating;
	}
	nRating = GetRating(sFileName);
	::EnterCriticalSection(&GetCacheCS());
	GetRatingCache()[key] = nRating;
	::LeaveCriticalSection(&GetCacheCS());
	return nRating;
}

bool CXMPRating::SetRating(LPCTSTR sFileName, int nRating) {
	if (sFileName == NULL || sFileName[0] == 0 || nRating < 0 || nRating > 5) {
		return false;
	}
	bool bOk = IsJPEGFile(sFileName) ? SetRatingInJPEG(sFileName, nRating) : SetRatingInSidecar(sFileName, nRating);
	if (bOk) {
		::EnterCriticalSection(&GetCacheCS());
		GetRatingCache()[std::wstring(sFileName)] = nRating;
		::LeaveCriticalSection(&GetCacheCS());
	}
	return bOk;
}
