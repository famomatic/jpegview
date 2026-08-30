#include "StdAfx.h"
#include "LazySource.h"
#include <cstdio>
#include <vector>

CAppModule _Module;

class CFakeLazySource final : public CLazySource {
public:
    CFakeLazySource(int width, int height, bool tiled)
        : m_decodeCalls(0) {
        m_nWidth = m_nBaseWidth = width;
        m_nHeight = m_nBaseHeight = height;
        m_nChannels = 4;
        m_nBitsPerSample = 8;
        m_bHasAlpha = true;
        m_nPyramidLevels = 1;
        m_bTiled = tiled;
        if (tiled) {
            m_nTileWidth = 8;
            m_nTileHeight = 4;
            m_nTilesPerImage = ((width + 7) / 8) * ((height + 3) / 4);
        } else {
            m_nRowsPerStrip = 2;
            m_nStripsPerImage = (height + 1) / 2;
        }
    }

    bool SetFrame(int frame) override { return frame == 0; }
    const uint8* ICCProfile(unsigned int& size) const override { size = 0; return nullptr; }
    void* EXIFData(int& size) const override { size = 0; return nullptr; }
    CRawMetadata* RawMetadata() const override { return nullptr; }
    int DecodeCalls() const { return m_decodeCalls; }

protected:
    bool DecodeStrips(int startStrip, int stripCount, uint8* dst, int stride) override {
        ++m_decodeCalls;
        for (int s = 0; s < stripCount; ++s) {
            for (int localY = 0; localY < m_nRowsPerStrip; ++localY) {
                int y = (startStrip + s) * m_nRowsPerStrip + localY;
                if (y >= m_nHeight) break;
                uint8* row = dst + ((size_t)s * m_nRowsPerStrip + localY) * stride;
                for (int x = 0; x < m_nWidth; ++x) PutPixel(row + (size_t)x * 4, x, y);
            }
        }
        return true;
    }

    bool DecodeTile(int tileX, int tileY, uint8* dst) override {
        ++m_decodeCalls;
        for (int y = 0; y < m_nTileHeight; ++y) {
            for (int x = 0; x < m_nTileWidth; ++x) {
                PutPixel(dst + ((size_t)y * m_nTileWidth + x) * 4,
                    tileX * m_nTileWidth + x, tileY * m_nTileHeight + y);
            }
        }
        return true;
    }

    bool SetPyramidLevel(int level) override { return level == 0; }

private:
    static void PutPixel(uint8* p, int x, int y) {
        p[0] = (uint8)x;
        p[1] = (uint8)y;
        p[2] = (uint8)(x + y);
        p[3] = 255;
    }
    int m_decodeCalls;
};

static int SourceCoordinate(int sourceSize, int targetSize, int target) {
    uint64_t increment = ((uint64_t)sourceSize << 16) / targetSize + 1;
    return (int)(((uint64_t)target * increment) >> 16);
}

static bool RunCase(const char* name, int sourceW, int sourceH, bool tiled,
    CSize target, CPoint offset, CSize viewport) {
    CFakeLazySource source(sourceW, sourceH, tiled);
    std::vector<uint8> pixels((size_t)viewport.cx * viewport.cy * 4, 0);
    bool ok = source.DecodeResampledRegion(target, offset, viewport, pixels.data());
    for (int y = 0; ok && y < viewport.cy; ++y) {
        int sy = SourceCoordinate(sourceH, target.cy, offset.y + y);
        for (int x = 0; x < viewport.cx; ++x) {
            int sx = SourceCoordinate(sourceW, target.cx, offset.x + x);
            const uint8* p = pixels.data() + ((size_t)y * viewport.cx + x) * 4;
            if (p[0] != (uint8)sx || p[1] != (uint8)sy ||
                p[2] != (uint8)(sx + sy) || p[3] != 255) {
                ok = false;
                break;
            }
        }
    }
    printf("%-48s %s  decode calls=%d\n", name, ok ? "PASS" : "FAIL", source.DecodeCalls());
    return ok;
}

int main() {
    bool ok = true;
    ok &= RunCase("stripped full viewport", 100, 50, false,
        CSize(10, 5), CPoint(0, 0), CSize(10, 5));
    ok &= RunCase("stripped clipped viewport", 100, 50, false,
        CSize(10, 5), CPoint(2, 1), CSize(3, 2));
    ok &= RunCase("tiled full viewport", 32, 16, true,
        CSize(8, 4), CPoint(0, 0), CSize(8, 4));
    return ok ? 0 : 1;
}
