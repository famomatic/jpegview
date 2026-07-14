#include "stdafx.h"
#include "TiffLazySource.h"
#include "Helpers.h"
#include "MaxImageDef.h"
#include <algorithm>
#include <cstring>

// TIFFGetFieldDefaulted forwards its va_list to TIFFVGetFieldDefaulted, which
// crashes in Release when libtiff is linked statically with a mismatched CRT.
// TIFFGetField only reads the out-arg when the tag is present, so it never
// touches va_list state for missing tags. Wrap it to provide defaults safely.
namespace {
template <typename T>
inline void TiffGetOrDefault(TIFF* tif, uint32 tag, T& out, const T& defVal) {
    if (TIFFGetField(tif, tag, &out) != 1)
        out = defVal;
}
}

CTiffLazySource::CTiffLazySource()
	: m_tif(nullptr)
	, m_nFrameIndex(0)
	, m_photometric(0)
	, m_planarConfig(PLANARCONFIG_CONTIG)
	, m_sampleFormat(SAMPLEFORMAT_UINT)
	, m_compression(0)
	, m_nCurrentPyramidLevel(0)
	, m_bUseRGBA(false)
{
}

CTiffLazySource::~CTiffLazySource()
{
	Release();
}

void CTiffLazySource::Release()
{
	if (m_bReleased)
		return;
	m_bReleased = true;

	if (m_tif != nullptr)
	{
		TIFFClose(m_tif);
		m_tif = nullptr;
	}

	delete[] m_pICCProfile;
	m_pICCProfile = nullptr;
	m_nICCSize = 0;

	delete[] m_pEXIFData;
	m_pEXIFData = nullptr;
	m_nEXIFSize = 0;

	delete m_pRawMetadata;
	m_pRawMetadata = nullptr;
}

CTiffLazySource* CTiffLazySource::Create(LPCTSTR strFileName, int nFrameIndex, bool& bOutOfMemory)
{
	bOutOfMemory = false;
	CTiffLazySource* pSource = new (std::nothrow) CTiffLazySource();
	if (pSource == nullptr)
	{
		bOutOfMemory = true;
		return nullptr;
	}
	if (!pSource->OpenAndReadMetadata(strFileName, nFrameIndex))
	{
		delete pSource;
		return nullptr;
	}
	return pSource;
}

bool CTiffLazySource::OpenAndReadMetadata(LPCTSTR strFileName, int nFrameIndex)
{
	m_sFileName = strFileName;
	m_nFrameIndex = nFrameIndex;

	m_tif = TIFFOpenW(strFileName, "rm");
	if (m_tif == nullptr)
		return false;

	// 요청된 프레임(페이지)으로 이동.
	if (nFrameIndex > 0)
	{
		if (TIFFSetDirectory(m_tif, (tdir_t)nFrameIndex) == 0)
		{
			TIFFClose(m_tif);
			m_tif = nullptr;
			return false;
		}
	}

	// 메타데이터 읽기.
	uint32 width = 0, height = 0;
	uint16 bitsPerSample = 1, samplesPerPixel = 1;
	TiffGetOrDefault(m_tif, TIFFTAG_IMAGEWIDTH,    width,          (uint32)0);
	TiffGetOrDefault(m_tif, TIFFTAG_IMAGELENGTH,   height,         (uint32)0);
	TiffGetOrDefault(m_tif, TIFFTAG_BITSPERSAMPLE, bitsPerSample,  (uint16)1);
	TiffGetOrDefault(m_tif, TIFFTAG_SAMPLESPERPIXEL, samplesPerPixel, (uint16)1);
	TiffGetOrDefault(m_tif, TIFFTAG_PHOTOMETRIC,   m_photometric,  (uint16)0);
	TiffGetOrDefault(m_tif, TIFFTAG_PLANARCONFIG,  m_planarConfig,  (uint16)PLANARCONFIG_CONTIG);
	TiffGetOrDefault(m_tif, TIFFTAG_SAMPLEFORMAT,  m_sampleFormat,  (uint16)SAMPLEFORMAT_UINT);
	TiffGetOrDefault(m_tif, TIFFTAG_COMPRESSION,   m_compression,   (uint16)COMPRESSION_NONE);

	m_nWidth = (int)width;
	m_nHeight = (int)height;
	m_nBitsPerSample = bitsPerSample;
	m_nChannels = samplesPerPixel;

	// 타일 vs 스트라이프 확인.
	uint32 tileWidth = 0, tileHeight = 0;
	if (TIFFGetField(m_tif, TIFFTAG_TILEWIDTH, &tileWidth) == 1 &&
	    TIFFGetField(m_tif, TIFFTAG_TILELENGTH, &tileHeight) == 1 &&
	    tileWidth > 0 && tileHeight > 0)
	{
		m_bTiled = true;
		m_nTileWidth = (int)tileWidth;
		m_nTileHeight = (int)tileHeight;
		m_nTilesPerImage = TIFFNumberOfTiles(m_tif);
	}
	else
	{
		m_bTiled = false;
		uint32 rowsPerStrip = 0;
		TiffGetOrDefault(m_tif, TIFFTAG_ROWSPERSTRIP, rowsPerStrip, (uint32)0);
		if (rowsPerStrip == 0)
			rowsPerStrip = m_nHeight; // 단일 스트립
		m_nRowsPerStrip = (int)rowsPerStrip;
		m_nStripsPerImage = TIFFNumberOfStrips(m_tif);
	}

	// 알파 채널 감지.
	uint16 extraSamples = 0;
	// EXTRASAMPLES is a count+array tag; read just the count (1 arg).
	if (TIFFGetField(m_tif, TIFFTAG_EXTRASAMPLES, &extraSamples) != 1)
		extraSamples = 0;
	m_bHasAlpha = (extraSamples > 0);

	// RGBA fast path 사용 여부 (8비트 팔레트/YCbCr/MINISWHITE 등).
	m_bUseRGBA = (bitsPerSample == 8) &&
		(m_photometric == PHOTOMETRIC_PALETTE ||
		 m_photometric == PHOTOMETRIC_MINISWHITE ||
		 (m_photometric == PHOTOMETRIC_MINISBLACK && samplesPerPixel == 1) ||
		 m_photometric == PHOTOMETRIC_YCBCR ||
		 // The manual 8-bit path (ConvertStripToBGRA) assumes interleaved
		 // (contig) samples. For separate planar config each strip holds a
		 // single sample plane, so route it through libtiff's RGBA reader,
		 // which composes the planes correctly, to avoid an out-of-bounds read.
		 m_planarConfig == PLANARCONFIG_SEPARATE);

	// ICC 프로파일 읽기.
	uint32 iccSize = 0;
	uint8* iccData = nullptr;
	if (TIFFGetField(m_tif, TIFFTAG_ICCPROFILE, &iccSize, &iccData) == 1 && iccSize > 0 && iccData != nullptr)
	{
		m_pICCProfile = new (std::nothrow) uint8[iccSize];
		if (m_pICCProfile != nullptr)
		{
			memcpy(m_pICCProfile, iccData, iccSize);
			m_nICCSize = iccSize;
		}
	}

	// 프레임 수 계산.
	// Use TIFFNumberOfDirectories() instead of manually iterating every IFD.
	// Manual iteration reads each directory header, which is O(n) disk I/O
	// and stalls opening multi-page scan documents (hundreds of pages) for
	// seconds. TIFFNumberOfDirectories() uses the IFD offset chain in libtiff
	// 4.x without fully reading each directory.
	tdir_t nDirs = TIFFNumberOfDirectories(m_tif);
	m_nFrameCount = (nDirs > 0) ? (int)nDirs : 1;
	// The directory was already set to nFrameIndex at the top of this method
	// (or left at 0 when nFrameIndex == 0). TIFFNumberOfDirectories() in
	// libtiff 4.x does not change the current directory, so the redundant
	// TIFFSetDirectory here was wasting I/O. Only re-set if the count call
	// moved us (it shouldn't, but guard defensively).
	if (TIFFCurrentDirectory(m_tif) != (tdir_t)nFrameIndex)
		TIFFSetDirectory(m_tif, (tdir_t)nFrameIndex);

	// 피라미드 레벨 감지.
	m_nPyramidLevels = DetectPyramidLevels();

	return true;
}

int CTiffLazySource::DetectPyramidLevels()
{
	// IFD를 순회하며 크기가 점점 작아지는지 검사.
	// pyramidal TIFF는 보통 원본 IFD 다음에 1/2, 1/4, ... 크기의 IFD가 온다.
	// 단, 멀티프레임 문서와 구분해야 한다: 피라미드는 각 레벨이
	// 이전 레벨의 정확히 1/2(또는 가까운 값)여야 한다.
	if (m_nFrameCount <= 1)
		return 1;

	// Skip pyramid detection for multi-page documents (e.g. scans with
	// hundreds of pages). Pyramidal TIFFs have a small number of levels
	// (typically 3-8), so iterating every IFD on a 500-page document just to
	// discover there is no pyramid wastes seconds of I/O and negates the
	// TIFFNumberOfDirectories() optimization above.
	if (m_nFrameCount > 16)
		return 1;

	int levels = 1;
	tdir_t origDir = TIFFCurrentDirectory(m_tif);

	// 첫 번째 IFD의 크기.
	TIFFSetDirectory(m_tif, 0);
	uint32 w0 = 0, h0 = 0;
	TiffGetOrDefault(m_tif, TIFFTAG_IMAGEWIDTH,  w0, (uint32)0);
	TiffGetOrDefault(m_tif, TIFFTAG_IMAGELENGTH, h0, (uint32)0);

	uint32 prevW = w0, prevH = h0;
	m_pyramidIFDs.clear();
	m_pyramidIFDs.push_back(0);

	tdir_t dir = 1;
	while (TIFFSetDirectory(m_tif, dir) == 1)
	{
		uint32 w = 0, h = 0;
		TiffGetOrDefault(m_tif, TIFFTAG_IMAGEWIDTH,  w, (uint32)0);
		TiffGetOrDefault(m_tif, TIFFTAG_IMAGELENGTH, h, (uint32)0);

		// 이전 레벨의 약 1/2 크기인지 확인 (10% 오차 허용).
		if (w > 0 && h > 0 &&
		    w >= prevW * 0.4 && w <= prevW * 0.6 &&
		    h >= prevH * 0.4 && h <= prevH * 0.6)
		{
			m_pyramidIFDs.push_back((int)dir);
			levels++;
			prevW = w;
			prevH = h;
			dir++;
		}
		else
		{
			break;
		}
	}

	// 원래 디렉토리로 복원.
	TIFFSetDirectory(m_tif, origDir);

	if (levels <= 1)
		m_pyramidIFDs.clear();

	return levels;
}

bool CTiffLazySource::SetPyramidLevel(int level)
{
	// Caller must hold m_tifLock (via LockSource). This method switches the
	// TIFF directory and updates metadata; it must run atomically with the
	// subsequent decode to prevent another thread from switching the IFD
	// between the level-set and the pixel read.
	if (level == m_nCurrentPyramidLevel)
		return true;

	if (level < 0 || level >= m_nPyramidLevels)
		return false;

	if (m_pyramidIFDs.empty())
		return (level == 0);

	tdir_t targetDir = (tdir_t)m_pyramidIFDs[level];
	if (TIFFSetDirectory(m_tif, targetDir) == 0)
		return false;

	// 레벨 전환 후 메타데이터 갱신.
	uint32 width = 0, height = 0;
	TiffGetOrDefault(m_tif, TIFFTAG_IMAGEWIDTH,  width,  (uint32)0);
	TiffGetOrDefault(m_tif, TIFFTAG_IMAGELENGTH, height, (uint32)0);
	m_nWidth = (int)width;
	m_nHeight = (int)height;

	if (m_bTiled)
	{
		uint32 tw = 0, th = 0;
		TIFFGetField(m_tif, TIFFTAG_TILEWIDTH, &tw);
		TIFFGetField(m_tif, TIFFTAG_TILELENGTH, &th);
		m_nTileWidth = (int)tw;
		m_nTileHeight = (int)th;
		m_nTilesPerImage = TIFFNumberOfTiles(m_tif);
	}
	else
	{
		uint32 rps = 0;
		TiffGetOrDefault(m_tif, TIFFTAG_ROWSPERSTRIP, rps, (uint32)0);
		if (rps == 0) rps = m_nHeight;
		m_nRowsPerStrip = (int)rps;
		m_nStripsPerImage = TIFFNumberOfStrips(m_tif);
	}

	m_nCurrentPyramidLevel = level;
	return true;
}

bool CTiffLazySource::SetFrame(int nFrame)
{
	// Caller must hold m_tifLock (via LockSource).
	if (nFrame < 0 || nFrame >= m_nFrameCount)
		return false;
	if (nFrame == m_nCurrentFrame)
		return true;
	if (TIFFSetDirectory(m_tif, (tdir_t)nFrame) == 0)
		return false;
	m_nCurrentFrame = nFrame;
	m_nCurrentPyramidLevel = 0;
	return true;
}

const uint8* CTiffLazySource::ICCProfile(unsigned int& nSize) const
{
	nSize = m_nICCSize;
	return m_pICCProfile;
}

void* CTiffLazySource::EXIFData(int& nSize) const
{
	// TIFF는 EXIF APP1 블록을 직접 가지지 않으므로 NULL 반환.
	// TIFF의 메타데이터는 별도 태그로 저장되며, 필요시 여기서 변환할 수 있다.
	nSize = 0;
	return nullptr;
}

CRawMetadata* CTiffLazySource::RawMetadata() const
{
	return m_pRawMetadata;
}

bool CTiffLazySource::DecodeSingleStrip(int stripIndex, uint8* pDst, int dstStride)
{
	// 이 스트립의 행 수 (마지막 스트립은 더 짧을 수 있음).
	int rowsInStrip = m_nRowsPerStrip;
	int stripStartRow = stripIndex * m_nRowsPerStrip;
	if (stripStartRow + rowsInStrip > m_nHeight)
		rowsInStrip = m_nHeight - stripStartRow;
	if (rowsInStrip <= 0)
		return false;

	// RGBA fast path: palette/YCbCr/MINISWHITE/16-bit/32-bit 등은 libtiff의
	// TIFFReadRGBAStrip으로 디코드한다. libtiff가 모든 색상 공간 변환과
	// 비트 깊이 변환을 처리하므로 수동 변환보다 정확하다.
	if (m_bUseRGBA || m_nBitsPerSample != 8)
	{
		// TIFFReadRGBAStrip은 전체 이미지 너비 * rowsInStrip 크기의
		// uint32(ABGR) 버퍼를 채운다. 마지막 스트립은 나머지 행만 유효.
		uint32* pRGBA = (uint32*)_TIFFmalloc((tmsize_t)m_nWidth * rowsInStrip * sizeof(uint32));
		if (pRGBA == nullptr)
			return false;
		// TIFFReadRGBAStrip takes the first ROW of the strip, not the strip
		// index; libtiff rejects any row that is not a multiple of rowsPerStrip.
		if (TIFFReadRGBAStrip(m_tif, (uint32)stripIndex * m_nRowsPerStrip, pRGBA) == 0)
		{
			_TIFFfree(pRGBA);
			return false;
		}
		// libtiff RGBA는 메모리에 ABGR(uint32)로 저장된다.
		// BGRA 레이아웃으로 변환: A=byte3, B=byte2, G=byte1, R=byte0.
		// TIFFReadRGBAStrip returns the strip already in top-down order for a
		// TOPLEFT image (libtiff applies the vertical flip internally based on
		// the file's orientation tag), so read source rows in order. A previous
		// reverse here double-flipped the strip and produced a bottom-up buffer.
		for (int y = 0; y < rowsInStrip; y++)
		{
			const uint32* pSrcRow = pRGBA + (size_t)y * m_nWidth;
			uint8* pDstRow = pDst + (size_t)y * dstStride;
			for (int x = 0; x < m_nWidth; x++)
			{
				uint32 px = pSrcRow[x];
				pDstRow[x * 4 + 0] = (uint8)((px >> 0) & 0xFF);  // B (TIFFGetR)
				pDstRow[x * 4 + 1] = (uint8)((px >> 8) & 0xFF);  // G (TIFFGetG)
				pDstRow[x * 4 + 2] = (uint8)((px >> 16) & 0xFF); // R (TIFFGetB)
				pDstRow[x * 4 + 3] = (uint8)((px >> 24) & 0xFF); // A (TIFFGetA)
			}
		}
		_TIFFfree(pRGBA);
		return true;
	}

	// 8비트 RGB/MINISBLACK fast path: 수동 변환이 TIFFReadRGBAStrip보다 빠르다.
	tmsize_t stripSize = TIFFStripSize(m_tif);
	if (stripSize <= 0)
		return false;

	uint8* pStripBuf = (uint8*)_TIFFmalloc(stripSize);
	if (pStripBuf == nullptr)
		return false;

	tmsize_t bytesRead = TIFFReadEncodedStrip(m_tif, (uint32)stripIndex, pStripBuf, stripSize);
	if (bytesRead < 0)
	{
		_TIFFfree(pStripBuf);
		return false;
	}

	// BGRA로 변환.
	int srcStride = TIFFScanlineSize(m_tif);
	ConvertStripToBGRA(pStripBuf, pDst, m_nWidth, rowsInStrip, (int)srcStride, dstStride);

	_TIFFfree(pStripBuf);
	return true;
}

void CTiffLazySource::ConvertStripToBGRA(const uint8* pSrc, uint8* pDst,
                                          int width, int rowsInStrip,
                                          int srcStride, int dstStride)
{
	// This method is only reached for 8-bit RGB/MINISBLACK/MINISWHITE
	// (the RGBA fast path in DecodeSingleStrip/DecodeTile handles palette,
	// YCbCr, CMYK, and 16/32-bit via TIFFReadRGBAStrip/TIFFReadRGBATile).

	for (int y = 0; y < rowsInStrip; y++)
	{
		const uint8* pSrcRow = pSrc + (size_t)y * srcStride;
		uint8* pDstRow = pDst + (size_t)y * dstStride;

		if (m_photometric == PHOTOMETRIC_RGB)
		{
			for (int x = 0; x < width; x++)
			{
				const uint8* s = pSrcRow + (size_t)x * m_nChannels;
				pDstRow[x * 4 + 0] = s[2]; // B
				pDstRow[x * 4 + 1] = s[1]; // G
				pDstRow[x * 4 + 2] = s[0]; // R
				pDstRow[x * 4 + 3] = (m_bHasAlpha && m_nChannels > 3) ? s[3] : 0xFF;
			}
		}
		else if (m_photometric == PHOTOMETRIC_MINISBLACK)
		{
			for (int x = 0; x < width; x++)
			{
				uint8 g = pSrcRow[x];
				pDstRow[x * 4 + 0] = g;
				pDstRow[x * 4 + 1] = g;
				pDstRow[x * 4 + 2] = g;
				pDstRow[x * 4 + 3] = 0xFF;
			}
		}
		else if (m_photometric == PHOTOMETRIC_MINISWHITE)
		{
			for (int x = 0; x < width; x++)
			{
				uint8 g = 255 - pSrcRow[x];
				pDstRow[x * 4 + 0] = g;
				pDstRow[x * 4 + 1] = g;
				pDstRow[x * 4 + 2] = g;
				pDstRow[x * 4 + 3] = 0xFF;
			}
		}
		else
		{
			// Should not reach here: palette/YCbCr/CMYK use the RGBA path.
			// Fill black as a safe fallback rather than a wrong-color copy.
			memset(pDstRow, 0, width * 4);
		}
	}
}

bool CTiffLazySource::DecodeStrips(int startStrip, int stripCount,
                                    uint8* pDst, int dstStride)
{
	std::lock_guard<std::recursive_mutex> lock(m_tifLock);
	if (m_tif == nullptr || m_bReleased)
		return false;

	for (int i = 0; i < stripCount; i++)
	{
		int stripIndex = startStrip + i;
		if (stripIndex >= m_nStripsPerImage)
			break;

		// 각 스트립을 pDst의 해당 오프셋에 디코드.
		// 64-bit offset: i*rowsPerStrip*dstStride overflows a 32-bit int for
		// images larger than ~2 GB (well within the x64 dimension limit),
		// producing a wrapped/negative offset and an out-of-bounds write.
		size_t stripOffset = (size_t)i * m_nRowsPerStrip * dstStride;
		if (!DecodeSingleStrip(stripIndex, pDst + stripOffset, dstStride))
		{
			// 실패한 스트립은 검은색으로 채운다.
			int rows = m_nRowsPerStrip;
			int stripStartRow = stripIndex * m_nRowsPerStrip;
			if (stripStartRow + rows > m_nHeight)
				rows = m_nHeight - stripStartRow;
			for (int y = 0; y < rows; y++)
				memset(pDst + stripOffset + (size_t)y * dstStride, 0, m_nWidth * 4);
		}
	}
	return true;
}

bool CTiffLazySource::DecodeTile(int tileX, int tileY, uint8* pDst)
{
	std::lock_guard<std::recursive_mutex> lock(m_tifLock);
	if (m_tif == nullptr || m_bReleased)
		return false;

	// 타일 인덱스 계산 (행 우선).
	int tilesPerRow = (m_nWidth + m_nTileWidth - 1) / m_nTileWidth;
	int tileIndex = tileY * tilesPerRow + tileX;

	// RGBA fast path: palette/YCbCr/MINISWHITE/16-bit/32-bit 등은 libtiff의
	// TIFFReadRGBATile로 디코드한다.
	if (m_bUseRGBA || m_nBitsPerSample != 8)
	{
		uint32* pRGBA = (uint32*)_TIFFmalloc((tmsize_t)m_nTileWidth * m_nTileHeight * sizeof(uint32));
		if (pRGBA == nullptr)
			return false;
		if (TIFFReadRGBATile(m_tif, (uint32)tileX * m_nTileWidth, (uint32)tileY * m_nTileHeight, pRGBA) == 0)
		{
			_TIFFfree(pRGBA);
			return false;
		}
		// libtiff RGBA(ABGR uint32) -> BGRA 변환.
		// TIFFReadRGBATile returns the tile already in top-down order for a
		// TOPLEFT image (libtiff applies the vertical flip internally based on
		// the file's orientation tag), so read source rows in order.
		for (int y = 0; y < m_nTileHeight; y++)
		{
			const uint32* pSrcRow = pRGBA + (size_t)y * m_nTileWidth;
			uint8* pDstRow = pDst + (size_t)y * m_nTileWidth * 4;
			for (int x = 0; x < m_nTileWidth; x++)
			{
				uint32 px = pSrcRow[x];
				pDstRow[x * 4 + 0] = (uint8)((px >> 0) & 0xFF);  // B
				pDstRow[x * 4 + 1] = (uint8)((px >> 8) & 0xFF);  // G
				pDstRow[x * 4 + 2] = (uint8)((px >> 16) & 0xFF); // R
				pDstRow[x * 4 + 3] = (uint8)((px >> 24) & 0xFF); // A
			}
		}
		_TIFFfree(pRGBA);
		return true;
	}

	// 8비트 RGB/MINISBLACK fast path.
	tmsize_t tileSize = TIFFTileSize(m_tif);
	if (tileSize <= 0)
		return false;

	uint8* pTileBuf = (uint8*)_TIFFmalloc(tileSize);
	if (pTileBuf == nullptr)
		return false;

	tmsize_t bytesRead = TIFFReadEncodedTile(m_tif, (uint32)tileIndex, pTileBuf, tileSize);
	if (bytesRead < 0)
	{
		_TIFFfree(pTileBuf);
		return false;
	}

	// BGRA로 변환.
	int srcStride = TIFFTileRowSize(m_tif);
	ConvertStripToBGRA(pTileBuf, pDst, m_nTileWidth, m_nTileHeight, (int)srcStride, m_nTileWidth * 4);

	_TIFFfree(pTileBuf);
	return true;
}
// end
