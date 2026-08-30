#include "StdAfx.h"
#include "DDSWrapper.h"

#include <cstdio>
#include <vector>

CAppModule _Module;

namespace {
#pragma pack(push, 1)
struct PixelFormat {
    DWORD size, flags, fourCC, rgbBits, redMask, greenMask, blueMask, alphaMask;
};
struct Header {
    DWORD size, flags, height, width, pitch, depth, mipCount, reserved[11];
    PixelFormat pixel;
    DWORD caps, caps2, caps3, caps4, reserved2;
};
#pragma pack(pop)

std::vector<unsigned char> MakeDDS(int width, int height, DWORD flags, DWORD fourCC,
    DWORD bits, DWORD red, DWORD green, DWORD blue, DWORD alpha,
    const std::vector<unsigned char>& payload) {
    std::vector<unsigned char> result(4 + sizeof(Header) + payload.size(), 0);
    const DWORD magic = 0x20534444;
    memcpy(result.data(), &magic, 4);
    Header header = {};
    header.size = 124;
    header.flags = 0x6;
    header.width = width;
    header.height = height;
    header.pixel = { 32, flags, fourCC, bits, red, green, blue, alpha };
    memcpy(result.data() + 4, &header, sizeof(header));
    memcpy(result.data() + 4 + sizeof(header), payload.data(), payload.size());
    return result;
}

bool Decode(const std::vector<unsigned char>& input, int expectedWidth, int expectedHeight,
    unsigned char expectedB, unsigned char expectedG, unsigned char expectedR, unsigned char expectedA) {
    int width = 99, height = 99, bpp = 0;
    bool outOfMemory = false;
    unsigned char* pixels = static_cast<unsigned char*>(DdsReader::ReadImage(width, height, bpp,
        outOfMemory, input.data(), static_cast<int>(input.size())));
    if (pixels == nullptr || outOfMemory || width != expectedWidth || height != expectedHeight || bpp != 4) {
        delete[] pixels;
        return false;
    }
    const bool ok = pixels[0] == expectedB && pixels[1] == expectedG && pixels[2] == expectedR && pixels[3] == expectedA;
    delete[] pixels;
    return ok;
}
}

int main() {
    int width = 0, height = 0, bpp = 0;
    bool oom = false;
    const unsigned char tiny[] = { 'D', 'D', 'S', ' ' };
    if (DdsReader::ReadImage(width, height, bpp, oom, tiny, sizeof(tiny)) != nullptr) return 1;

    // One BGRA pixel with explicit channel masks.
    std::vector<unsigned char> raw = MakeDDS(1, 1, 0x41, 0, 32,
        0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000, { 3, 2, 1, 4 });
    if (!Decode(raw, 1, 1, 3, 2, 1, 4)) return 2;

    // BC1: RGB565 red endpoint and all indices zero.
    std::vector<unsigned char> bc1 = MakeDDS(4, 4, 0x4, MAKEFOURCC('D','X','T','1'), 0, 0, 0, 0, 0,
        { 0x00, 0xF8, 0x00, 0x00, 0, 0, 0, 0 });
    if (!Decode(bc1, 4, 4, 0, 0, 255, 255)) return 3;

    // BC2 explicit alpha nibble 0 must remain transparent.
    std::vector<unsigned char> bc2Payload(16, 0);
    bc2Payload[8] = 0x00; bc2Payload[9] = 0xF8;
    std::vector<unsigned char> bc2 = MakeDDS(4, 4, 0x4, MAKEFOURCC('D','X','T','3'), 0, 0, 0, 0, 0, bc2Payload);
    if (!Decode(bc2, 4, 4, 0, 0, 255, 0)) return 4;

    bc1.pop_back();
    if (DdsReader::ReadImage(width, height, bpp, oom, bc1.data(), static_cast<int>(bc1.size())) != nullptr) return 5;
    std::puts("DdsReaderTest passed");
    return 0;
}
