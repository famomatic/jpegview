#include "stdafx.h"

#include "XMPRating.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <cctype>
#include <algorithm>

namespace {

const char* XMP_APP1_HEADER = "http://ns.adobe.com/xap/1.0/"; // followed by \0 in the segment
const int XMP_APP1_HEADER_LEN = 29; // including terminating \0

std::mutex& GetCacheMutex() {
	static std::mutex mutex;
	return mutex;
}

struct FileIdentity {
	bool exists = false;
	__int64 size = 0;
	FILETIME modified{};
};

static bool operator==(const FileIdentity& a, const FileIdentity& b) {
	return a.exists == b.exists && (!a.exists || (a.size == b.size && ::CompareFileTime(&a.modified, &b.modified) == 0));
}

FileIdentity GetFileIdentity(LPCTSTR fileName) {
	FileIdentity result;
	HANDLE file = ::CreateFile(fileName, FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE) return result;
	LARGE_INTEGER size{};
	result.exists = ::GetFileSizeEx(file, &size) != FALSE && ::GetFileTime(file, NULL, NULL, &result.modified) != FALSE;
	result.size = size.QuadPart;
	::CloseHandle(file);
	return result;
}

struct CacheEntry {
	int rating;
	FileIdentity image;
	FileIdentity sidecar;
};

std::map<std::wstring, CacheEntry>& GetRatingCache() {
	static std::map<std::wstring, CacheEntry> cache;
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
	// Sidecar XMP packets are small. Refuse unexpectedly large files rather
	// than letting a corrupt or adversarial sidecar allocate gigabytes.
	bool bOk = ::GetFileSizeEx(hFile, &fileSize) && fileSize.QuadPart > 0 && fileSize.QuadPart <= 16 * 1024 * 1024;
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
	static volatile LONG sequence = 0;
	CString sTempName;
	HANDLE hFile = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 100 && hFile == INVALID_HANDLE_VALUE; ++attempt) {
		sTempName.Format(_T("%s.jvtmp.%lu.%ld"), sFileName, ::GetCurrentProcessId(), ::InterlockedIncrement(&sequence));
		hFile = ::CreateFile(sTempName, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
	}
	if (hFile == INVALID_HANDLE_VALUE) {
		return false;
	}
	const FileIdentity originalIdentity = GetFileIdentity(sFileName);
	DWORD originalAttributes = originalIdentity.exists ? ::GetFileAttributes(sFileName) : INVALID_FILE_ATTRIBUTES;
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
	if (bOk) bOk = ::FlushFileBuffers(hFile) != FALSE;
	::CloseHandle(hFile);
	if (bOk) {
		if (originalIdentity.exists)
			bOk = ::ReplaceFile(sFileName, sTempName, NULL, REPLACEFILE_WRITE_THROUGH, NULL, NULL) != 0;
		else
			bOk = ::MoveFileEx(sTempName, sFileName, MOVEFILE_WRITE_THROUGH) != 0;
	}
	if (!bOk) {
		::DeleteFile(sTempName);
	}
	if (bOk && originalAttributes != INVALID_FILE_ATTRIBUTES) ::SetFileAttributes(sFileName, originalAttributes);
	return bOk;
}

bool RewriteFileSegmentSafe(LPCTSTR fileName, const FileIdentity& expectedIdentity,
	size_t replaceOffset, size_t replaceLength, const std::string& replacement) {
	HANDLE source = ::CreateFile(fileName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (source == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER sourceSize = {};
	FILETIME modified = {};
	FileIdentity current;
	current.exists = ::GetFileSizeEx(source, &sourceSize) != FALSE && ::GetFileTime(source, NULL, NULL, &modified) != FALSE;
	current.size = sourceSize.QuadPart;
	current.modified = modified;
	if (!(current == expectedIdentity) || replaceOffset > static_cast<size_t>(sourceSize.QuadPart) ||
		replaceLength > static_cast<size_t>(sourceSize.QuadPart) - replaceOffset) {
		::CloseHandle(source);
		return false;
	}

	static volatile LONG sequence = 0;
	CString tempName;
	HANDLE destination = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 100 && destination == INVALID_HANDLE_VALUE; ++attempt) {
		tempName.Format(_T("%s.jvtmp.%lu.%ld"), fileName, ::GetCurrentProcessId(), ::InterlockedIncrement(&sequence));
		destination = ::CreateFile(tempName, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
	}
	if (destination == INVALID_HANDLE_VALUE) { ::CloseHandle(source); return false; }

	auto writeAll = [](HANDLE file, const char* data, size_t size) {
		size_t writtenTotal = 0;
		while (writtenTotal < size) {
			const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - writtenTotal, 16 * 1024 * 1024));
			DWORD written = 0;
			if (!::WriteFile(file, data + writtenTotal, chunk, &written, NULL) || written != chunk) return false;
			writtenTotal += written;
		}
		return true;
	};
	auto copyBytes = [&writeAll](HANDLE input, HANDLE output, unsigned long long count) {
		std::vector<char> buffer(1024 * 1024);
		while (count > 0) {
			const DWORD chunk = static_cast<DWORD>(std::min<unsigned long long>(count, buffer.size()));
			DWORD read = 0;
			if (!::ReadFile(input, buffer.data(), chunk, &read, NULL) || read != chunk || !writeAll(output, buffer.data(), read))
				return false;
			count -= read;
		}
		return true;
	};

	bool ok = false;
	try {
		ok = copyBytes(source, destination, replaceOffset) && writeAll(destination, replacement.data(), replacement.size());
		if (ok) {
			LARGE_INTEGER skip = {}; skip.QuadPart = static_cast<LONGLONG>(replaceLength);
			ok = ::SetFilePointerEx(source, skip, NULL, FILE_CURRENT) != FALSE &&
				copyBytes(source, destination, static_cast<unsigned long long>(sourceSize.QuadPart) - replaceOffset - replaceLength);
		}
	} catch (const std::bad_alloc&) {
		ok = false;
	}
	if (ok) ok = ::FlushFileBuffers(destination) != FALSE;
	::CloseHandle(destination);
	::CloseHandle(source);
	const DWORD originalAttributes = ::GetFileAttributes(fileName);
	if (ok) ok = ::ReplaceFile(fileName, tempName, NULL, REPLACEFILE_WRITE_THROUGH, NULL, NULL) != FALSE;
	if (!ok) ::DeleteFile(tempName);
	if (ok && originalAttributes != INVALID_FILE_ATTRIBUTES) ::SetFileAttributes(fileName, originalAttributes);
	return ok;
}

// Finds the XMP APP1 segment in a JPEG stream. Returns offset of the 0xFF marker byte
// and sets nSegmentLen to the total segment length (marker + length field + data), -1 if not found.
// If pnInsertOffset is given, it receives the best position to insert a new XMP segment.
int FindXMPSegment(const std::string& jpeg, int& nSegmentLen, int* pnInsertOffset = NULL, bool* pHeaderComplete = NULL) {
	nSegmentLen = 0;
	if (pHeaderComplete != NULL) *pHeaderComplete = false;
	size_t nSize = jpeg.size();
	if (nSize < 4 || (unsigned char)jpeg[0] != 0xFF || (unsigned char)jpeg[1] != 0xD8) {
		return -1;
	}
	size_t nPos = 2;
	size_t nInsert = 2; // directly after SOI by default
	while (nPos < nSize) {
		if ((unsigned char)jpeg[nPos] != 0xFF) break;
		size_t markerCode = nPos + 1;
		while (markerCode < nSize && (unsigned char)jpeg[markerCode] == 0xFF) ++markerCode;
		if (markerCode >= nSize) break;
		const size_t markerOffset = markerCode - 1;
		const unsigned char nMarker = (unsigned char)jpeg[markerCode];
		if (nMarker == 0xD8 || (nMarker >= 0xD0 && nMarker <= 0xD7) || nMarker == 0x01) {
			nPos = markerCode + 1; // standalone marker without length
			continue;
		}
		if (nMarker == 0xDA) {
			if (pHeaderComplete != NULL) *pHeaderComplete = true;
			break; // start of scan - metadata section over
		}
		if (nMarker == 0xD9) {
			if (pHeaderComplete != NULL) *pHeaderComplete = true;
			break;
		}
		if (markerCode + 2 >= nSize) break;
		const int nLen = ((unsigned char)jpeg[markerCode + 1] << 8) + (unsigned char)jpeg[markerCode + 2];
		if (nLen < 2 || markerOffset + 2 + static_cast<size_t>(nLen) > nSize) {
			break; // corrupt
		}
		if (nMarker == 0xE1 && nLen >= 2 + XMP_APP1_HEADER_LEN &&
			memcmp(&jpeg[markerCode + 3], XMP_APP1_HEADER, XMP_APP1_HEADER_LEN) == 0) {
			nSegmentLen = 2 + nLen;
			if (pnInsertOffset != NULL) *pnInsertOffset = static_cast<int>(markerOffset);
			return static_cast<int>(markerOffset);
		}
		if (nMarker == 0xE0 || nMarker == 0xE1) {
			nInsert = markerOffset + 2 + nLen; // insert after JFIF APP0 / EXIF APP1
		}
		nPos = markerOffset + 2 + nLen;
	}
	if (pnInsertOffset != NULL) *pnInsertOffset = (int)nInsert;
	return -1;
}

bool ReadJPEGHeader(LPCTSTR fileName, std::string& header, FileIdentity& identity) {
	header.clear();
	HANDLE file = ::CreateFile(fileName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (file == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size = {};
	identity.exists = ::GetFileSizeEx(file, &size) != FALSE && ::GetFileTime(file, NULL, NULL, &identity.modified) != FALSE;
	identity.size = size.QuadPart;
	const size_t maxHeader = 64 * 1024 * 1024;
	const size_t bytesToRead = identity.exists && size.QuadPart > 0
		? static_cast<size_t>(std::min<__int64>(size.QuadPart, static_cast<__int64>(maxHeader))) : 0;
	bool ok = bytesToRead > 0;
	try { if (ok) header.resize(bytesToRead); } catch (const std::bad_alloc&) { ok = false; }
	size_t offset = 0;
	while (ok && offset < bytesToRead) {
		const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytesToRead - offset, 16 * 1024 * 1024));
		DWORD read = 0;
		ok = ::ReadFile(file, &header[offset], chunk, &read, NULL) != FALSE && read == chunk;
		offset += read;
	}
	::CloseHandle(file);
	if (!ok) header.clear();
	return ok;
}

struct RatingLocation {
	size_t start = std::string::npos;
	size_t length = 0;
};

std::vector<std::string> FindXMPPrefixes(const std::string& xmp) {
	const std::string uri = "http://ns.adobe.com/xap/1.0/";
	std::vector<std::string> prefixes;
	size_t position = 0;
	while ((position = xmp.find(uri, position)) != std::string::npos) {
		if (position > 0 && (xmp[position - 1] == '\'' || xmp[position - 1] == '"') &&
			position + uri.size() < xmp.size() && xmp[position + uri.size()] == xmp[position - 1]) {
			size_t equals = position - 1;
			while (equals > 0 && std::isspace(static_cast<unsigned char>(xmp[equals - 1]))) --equals;
			if (equals > 0 && xmp[equals - 1] == '=') {
				size_t nameEnd = equals - 1;
				while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(xmp[nameEnd - 1]))) --nameEnd;
				size_t nameStart = nameEnd;
				while (nameStart > 0 && !std::isspace(static_cast<unsigned char>(xmp[nameStart - 1])) && xmp[nameStart - 1] != '<') --nameStart;
				const std::string declaration = xmp.substr(nameStart, nameEnd - nameStart);
				if (declaration == "xmlns") prefixes.push_back(std::string());
				else if (declaration.compare(0, 6, "xmlns:") == 0 && declaration.size() > 6)
					prefixes.push_back(declaration.substr(6));
			}
		}
		position += uri.size();
	}
	if (std::find(prefixes.begin(), prefixes.end(), "xmp") == prefixes.end())
		prefixes.push_back("xmp"); // tolerate packets inheriting or omitting the conventional declaration
	return prefixes;
}

bool FindRatingLocation(const std::string& xmp, RatingLocation& location) {
	for (const std::string& prefix : FindXMPPrefixes(xmp)) {
		const std::string name = prefix.empty() ? "Rating" : prefix + ":Rating";
		size_t position = 0;
		while ((position = xmp.find(name, position)) != std::string::npos) {
			const bool leftBoundary = position == 0 || std::isspace(static_cast<unsigned char>(xmp[position - 1])) || xmp[position - 1] == '<';
			size_t after = position + name.size();
			if (leftBoundary) {
				size_t equals = after;
				while (equals < xmp.size() && std::isspace(static_cast<unsigned char>(xmp[equals]))) ++equals;
				if (equals < xmp.size() && xmp[equals] == '=') {
					size_t quote = equals + 1;
					while (quote < xmp.size() && std::isspace(static_cast<unsigned char>(xmp[quote]))) ++quote;
					if (quote < xmp.size() && (xmp[quote] == '\'' || xmp[quote] == '"')) {
						const size_t end = xmp.find(xmp[quote], quote + 1);
						if (end != std::string::npos) { location = { quote + 1, end - quote - 1 }; return true; }
					}
				}
			}
			position = after;
		}
		const std::string open = "<" + name + ">";
		const std::string close = "</" + name + ">";
		const size_t openPosition = xmp.find(open);
		if (openPosition != std::string::npos) {
			const size_t valueStart = openPosition + open.size();
			const size_t valueEnd = xmp.find(close, valueStart);
			if (valueEnd != std::string::npos) { location = { valueStart, valueEnd - valueStart }; return true; }
		}
	}
	return false;
}

int ParseRatingFromXMP(const std::string& xmp) {
	RatingLocation location;
	if (!FindRatingLocation(xmp, location)) return -1;
	size_t begin = location.start;
	size_t end = location.start + location.length;
	while (begin < end && std::isspace(static_cast<unsigned char>(xmp[begin]))) ++begin;
	while (end > begin && std::isspace(static_cast<unsigned char>(xmp[end - 1]))) --end;
	if (end - begin != 1 || xmp[begin] < '0' || xmp[begin] > '5') return -1;
	return xmp[begin] - '0';
}

// Sets or inserts the rating in an existing XMP packet. Returns false if the packet
// has no place to put the rating (no rdf:Description element).
bool SpliceRatingIntoXMP(std::string& xmp, int nRating) {
	char buffer[16];
	sprintf_s(buffer, "%d", nRating);

	RatingLocation location;
	if (FindRatingLocation(xmp, location)) {
		xmp.replace(location.start, location.length, buffer);
		return true;
	}
	// no rating yet - add as attribute to the first rdf:Description tag
	size_t nPos = xmp.find(":Description");
	if (nPos != std::string::npos) nPos = xmp.rfind('<', nPos);
	if (nPos == std::string::npos) nPos = xmp.find("<Description");
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
	std::string jpegHeader;
	FileIdentity identity;
	if (!ReadJPEGHeader(sFileName, jpegHeader, identity)) {
		return -1;
	}
	int nSegmentLen;
	int nOffset = FindXMPSegment(jpegHeader, nSegmentLen);
	if (nOffset < 0) {
		return -1;
	}
	std::string xmp = jpegHeader.substr(nOffset + 4 + XMP_APP1_HEADER_LEN, nSegmentLen - 4 - XMP_APP1_HEADER_LEN);
	return ParseRatingFromXMP(xmp);
}

bool SetRatingInJPEG(LPCTSTR sFileName, int nRating) {
	std::string jpegHeader;
	FileIdentity identity;
	if (!ReadJPEGHeader(sFileName, jpegHeader, identity)) {
		return false;
	}
	int nSegmentLen;
	int nInsertOffset = 2;
	bool headerComplete = false;
	int nOffset = FindXMPSegment(jpegHeader, nSegmentLen, &nInsertOffset, &headerComplete);
	if (nOffset < 0 && !headerComplete) return false;
	std::string xmp;
	if (nOffset >= 0) {
		xmp = jpegHeader.substr(nOffset + 4 + XMP_APP1_HEADER_LEN, nSegmentLen - 4 - XMP_APP1_HEADER_LEN);
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
	const size_t replacementOffset = nOffset >= 0 ? static_cast<size_t>(nOffset) : static_cast<size_t>(nInsertOffset);
	const size_t replacementLength = nOffset >= 0 ? static_cast<size_t>(nSegmentLen) : 0;
	return RewriteFileSegmentSafe(sFileName, identity, replacementOffset, replacementLength, segment);
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
	CString normalized(sFileName);
	normalized.MakeLower();
	const std::wstring key(normalized.GetString());
	std::lock_guard<std::mutex> lock(GetCacheMutex());
	const FileIdentity imageIdentity = GetFileIdentity(sFileName);
	const CString sidecar = SidecarFileName(sFileName);
	const FileIdentity sidecarIdentity = GetFileIdentity(sidecar);
	auto iter = GetRatingCache().find(key);
	if (iter != GetRatingCache().end() && iter->second.image == imageIdentity && iter->second.sidecar == sidecarIdentity)
		return iter->second.rating;
	const int nRating = GetRating(sFileName);
	static const size_t MAX_CACHE_ENTRIES = 4096;
	if (GetRatingCache().size() >= MAX_CACHE_ENTRIES && iter == GetRatingCache().end()) GetRatingCache().erase(GetRatingCache().begin());
	GetRatingCache()[key] = CacheEntry{ nRating, GetFileIdentity(sFileName), GetFileIdentity(sidecar) };
	return nRating;
}

bool CXMPRating::SetRating(LPCTSTR sFileName, int nRating) {
	if (sFileName == NULL || sFileName[0] == 0 || nRating < 0 || nRating > 5) {
		return false;
	}
	std::lock_guard<std::mutex> lock(GetCacheMutex());
	bool bOk = IsJPEGFile(sFileName) ? SetRatingInJPEG(sFileName, nRating) : SetRatingInSidecar(sFileName, nRating);
	if (bOk) {
		CString normalized(sFileName);
		normalized.MakeLower();
		const CString sidecar = SidecarFileName(sFileName);
		GetRatingCache()[std::wstring(normalized.GetString())] = CacheEntry{
			nRating, GetFileIdentity(sFileName), GetFileIdentity(sidecar) };
	}
	return bOk;
}
