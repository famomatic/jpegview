#include "StdAfx.h"
#include "../src/JPEGView/XMPRating.cpp"

#include <cstdio>

CAppModule _Module;

int main() {
    const std::string alternate =
        "<rdf:Description xmlns:meta='http://ns.adobe.com/xap/1.0/' meta:Rating = '4'/>";
    if (ParseRatingFromXMP(alternate) != 4) return 1;

    std::string element =
        "<r:Description xmlns:q=\"http://ns.adobe.com/xap/1.0/\"><q:Rating> 2 </q:Rating></r:Description>";
    if (ParseRatingFromXMP(element) != 2 || !SpliceRatingIntoXMP(element, 5) || ParseRatingFromXMP(element) != 5) return 2;
    if (ParseRatingFromXMP("<xmp:Rating>3junk</xmp:Rating>") != -1) return 3;

    std::string withoutRating =
        "<z:Description xmlns:z='urn:rdf'></z:Description>";
    if (!SpliceRatingIntoXMP(withoutRating, 3) || ParseRatingFromXMP(withoutRating) != 3) return 4;

    std::string packet = CreateXMPPacket(1);
    std::string segment;
    if (!BuildXMPSegment(packet, segment)) return 5;
    std::string jpeg("\xFF\xD8\xFF\xFF", 4);
    jpeg += segment;
    jpeg.append("\xFF\xDA\x00\x08payload", 11);
    int segmentLength = 0, insertOffset = 0;
    bool complete = false;
    const int offset = FindXMPSegment(jpeg, segmentLength, &insertOffset, &complete);
    if (offset != 4 || segmentLength != static_cast<int>(segment.size()) || ParseRatingFromXMP(
        jpeg.substr(offset + 4 + XMP_APP1_HEADER_LEN, segmentLength - 4 - XMP_APP1_HEADER_LEN)) != 1) return 6;

    TCHAR tempDirectory[MAX_PATH] = {};
    if (::GetTempPath(MAX_PATH, tempDirectory) == 0) return 7;
    CString path;
    path.Format(_T("%sJPEGView-XmpRating-%lu.jpg"), tempDirectory, ::GetCurrentProcessId());
    ::DeleteFile(path);
    HANDLE file = ::CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 8;
    const unsigned char minimalJPEG[] = { 0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x08, 0, 0, 0, 0, 0, 0, 'T', 'A', 'I', 'L' };
    DWORD written = 0;
    const bool wrote = ::WriteFile(file, minimalJPEG, sizeof(minimalJPEG), &written, NULL) != FALSE && written == sizeof(minimalJPEG);
    ::CloseHandle(file);
    if (!wrote || !CXMPRating::SetRating(path, 4) || CXMPRating::GetRating(path) != 4) { ::DeleteFile(path); return 9; }
    std::string rewritten;
    FileIdentity ignored;
    if (!ReadJPEGHeader(path, rewritten, ignored) || rewritten.size() < 4 || rewritten.substr(rewritten.size() - 4) != "TAIL") {
        ::DeleteFile(path); return 10;
    }
    ::DeleteFile(path);
    std::puts("XmpRatingTest passed");
    return 0;
}
