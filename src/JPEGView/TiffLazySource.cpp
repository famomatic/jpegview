#include "stdafx.h"
#include "TiffLazySource.h"
#include "Helpers.h"
#include "MaxImageDef.h"
#include <algorithm>
#include <cstring>

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
	TIFFGetFieldDefaulted(m_tif, TIFFTAG_IMAGEWIDTH, &width);
	TIFFGetFieldDefaulted(m_tif, TIFFTAG_IMAGELENGTH, &height);
	TIFFGetFieldDefaulted(m_tif, TIFFTAG_BITSPERSAMPLE, &bitsPerSample);
	TIFFGetFieldDefaulted(m_tif, TIFFTAG_SAMPLESPERPIXEL, &samplesPerPixel);
	TIFFGetFieldDefaulted(m_tif, TIFFTAG_PHOTOMETRIC, &m_photometric);
	TIFFGetFieldDefaulted(m_tif, TIFFTAG_PLANARCONFIG, &m_planarConfig);
	TIFFGetFieldDefaulted(m_tif, TIFFTAG_SAMPLEFORMAT, &m_sampleFormat);
	TIFFGetFieldDefaulted(m_tif, TIFFTAG_COMPRESSION, &m_compression);

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
		TIFFGetFieldDefaulted(m_tif, TIFFTAG_ROWSPERSTRIP, &rowsPerStrip);
		if (rowsPerStrip == 0)
			rowsPerStrip = m_nHeight; // 단일 스트립
		m_nRowsPerStrip = (int)rowsPerStrip;
		m_nStripsPerImage = TIFFNumberOfStrips(m_tif);
	}

	// 알파 채널 감지.
	uint16 extraSamples = 0;
	TIFFGetFieldDefaulted(m_tif, TIFFTAG_EXTRASAMPLES, &extraSamples, nullptr);
	m_bHasAlpha = (extraSamples > 0);

	// RGBA fast path 사용 여부 (8비트 팔레트/YCbCr/MINISWHITE 등).
	m_bUseRGBA = (bitsPerSample == 8) &&
		(m_photometric == PHOTOMETRIC_PALETTE ||
		 m_photometric == PHOTOMETRIC_MINISWHITE ||
		 (m_photometric == PHOTOMETRIC_MINISBLACK && samplesPerPixel == 1) ||
		 m_photometric == PHOTOMETRIC_YCBCR);

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
	m_nFrameCount = 1;
	TIFFSetDirectory(m_tif, 0);
	while (TIFFReadDirectory(m_tif))
		m_nFrameCount++;
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

	int levels = 1;
	tdir_t origDir = TIFFCurrentDirectory(m_tif);

	// 첫 번째 IFD의 크기.
	TIFFSetDirectory(m_tif, 0);
	uint32 w0 = 0, h0 = 0;
	TIFFGetFieldDefaulted(m_tif, TIFFTAG_IMAGEWIDTH, &w0);
	TIFFGetFieldDefaulted(m_tif, TIFFTAG_IMAGELENGTH, &h0);

	uint32 prevW = w0, prevH = h0;
	m_pyramidIFDs.clear();
	m_pyramidIFDs.push_back(0);

	tdir_t dir = 1;
	while (TIFFSetDirectory(m_tif, dir) == 1)
	{
		uint32 w = 0, h = 0;
		TIFFGetFieldDefaulted(m_tif, TIFFTAG_IMAGEWIDTH, &w);
		TIFFGetFieldDefaulted(m_tif, TIFFTAG_IMAGELENGTH, &h);

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
	TIFFGetFieldDefaulted(m_tif, TIFFTAG_IMAGEWIDTH, &width);
	TIFFGetFieldDefaulted(m_tif, TIFFTAG_IMAGELENGTH, &height);
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
		TIFFGetFieldDefaulted(m_tif, TIFFTAG_ROWSPERSTRIP, &rps);
		if (rps == 0) rps = m_nHeight;
		m_nRowsPerStrip = (int)rps;
		m_nStripsPerImage = TIFFNumberOfStrips(m_tif);
	}

	m_nCurrentPyramidLevel = level;
	return true;
}

bool CTiffLazySource::SetFrame(int nFrame)
{
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
	// libtiff로 스트립 하나를 디코드.
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

	// 이 스트립의 행 수 (마지막 스트립은 더 짧을 수 있음).
	int rowsInStrip = m_nRowsPerStrip;
	int stripStartRow = stripIndex * m_nRowsPerStrip;
	if (stripStartRow + rowsInStrip > m_nHeight)
		rowsInStrip = m_nHeight - stripStartRow;
	if (rowsInStrip <= 0)
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
	// RGBA fast path: libtiff의 TIFFReadRGBAImage를 쓸 수 없으므로
	// 수동으로 변환. 8비트 RGB/그레이스케일만 처리.
	if (m_nBitsPerSample != 8)
	{
		// 16/32비트는 일단 검은색으로 채운다 (향후 지원).
		for (int y = 0; y < rowsInStrip; y++)
			memset(pDst + (size_t)y * dstStride, 0, width * 4);
		return;
	}

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
			// 팔레트, YCbCr, CMYK 등은 RGBA 변환이 복잡하므로
			// 여기서는 단순 복사. 향후 TIFFReadRGBAImage 기반으로 보강.
			for (int x = 0; x < width; x++)
			{
				const uint8* s = pSrcRow + (size_t)x * m_nChannels;
				pDstRow[x * 4 + 0] = s[0];
				pDstRow[x * 4 + 1] = (m_nChannels > 1) ? s[1] : s[0];
				pDstRow[x * 4 + 2] = (m_nChannels > 2) ? s[2] : s[0];
				pDstRow[x * 4 + 3] = 0xFF;
			}
		}
	}
}

bool CTiffLazySource::DecodeStrips(int startStrip, int stripCount,
                                    uint8* pDst, int dstStride)
{
	if (m_tif == nullptr || m_bReleased)
		return false;

	for (int i = 0; i < stripCount; i++)
	{
		int stripIndex = startStrip + i;
		if (stripIndex >= m_nStripsPerImage)
			break;

		// 각 스트립을 pDst의 해당 오프셋에 디코드.
		int stripOffset = i * m_nRowsPerStrip * dstStride;
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
	if (m_tif == nullptr || m_bReleased)
		return false;

	// 타일 인덱스 계산 (행 우선).
	int tilesPerRow = (m_nWidth + m_nTileWidth - 1) / m_nTileWidth;
	int tileIndex = tileY * tilesPerRow + tileX;

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
