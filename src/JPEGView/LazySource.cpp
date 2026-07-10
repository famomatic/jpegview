#include "stdafx.h"
#include "LazySource.h"
#include <algorithm>

CLazySource::CLazySource()
	: m_nWidth(0), m_nHeight(0), m_nChannels(4), m_nBitsPerSample(8)
	, m_bHasAlpha(false), m_nFrameCount(1), m_nCurrentFrame(0)
	, m_nPyramidLevels(1)
	, m_bTiled(false), m_nRowsPerStrip(0), m_nTileWidth(0), m_nTileHeight(0)
	, m_nStripsPerImage(0), m_nTilesPerImage(0)
	, m_pICCProfile(nullptr), m_nICCSize(0)
	, m_pEXIFData(nullptr), m_nEXIFSize(0)
	, m_pRawMetadata(nullptr)
	, m_bReleased(false)
{
}

CLazySource::~CLazySource()
{
	Release();
}

void CLazySource::Release()
{
	if (m_bReleased)
		return;
	m_bReleased = true;

	delete[] m_pICCProfile;
	m_pICCProfile = nullptr;
	m_nICCSize = 0;

	delete[] m_pEXIFData;
	m_pEXIFData = nullptr;
	m_nEXIFSize = 0;

	delete m_pRawMetadata;
	m_pRawMetadata = nullptr;
}

bool CLazySource::ReadSinglePixel(int x, int y, uint8 outBGRA[4])
{
	// 기본 구현: 해당 픽셀이 속한 스트립 하나를 디코드해서 읽는다.
	// 서브클래스가 더 효율적인 방법을 제공하면 오버라이드한다.
	if (m_bTiled)
	{
		int tileX = x / m_nTileWidth;
		int tileY = y / m_nTileHeight;
		uint8* pTile = new (std::nothrow) uint8[(size_t)m_nTileWidth * m_nTileHeight * 4];
		if (pTile == nullptr)
			return false;
		if (!DecodeTile(tileX, tileY, pTile))
		{
			delete[] pTile;
			return false;
		}
		int localX = x % m_nTileWidth;
		int localY = y % m_nTileHeight;
		const uint8* p = pTile + ((size_t)localY * m_nTileWidth + localX) * 4;
		outBGRA[0] = p[0]; outBGRA[1] = p[1]; outBGRA[2] = p[2]; outBGRA[3] = p[3];
		delete[] pTile;
		return true;
	}
	else
	{
		int strip = y / m_nRowsPerStrip;
		int dstStride = m_nWidth * 4;
		uint8* pStrip = new (std::nothrow) uint8[(size_t)m_nRowsPerStrip * dstStride];
		if (pStrip == nullptr)
			return false;
		if (!DecodeStrips(strip, 1, pStrip, dstStride))
		{
			delete[] pStrip;
			return false;
		}
		int localY = y % m_nRowsPerStrip;
		const uint8* p = pStrip + ((size_t)localY * m_nWidth + x) * 4;
		outBGRA[0] = p[0]; outBGRA[1] = p[1]; outBGRA[2] = p[2]; outBGRA[3] = p[3];
		delete[] pStrip;
		return true;
	}
}

bool CLazySource::SamplePoint(int x, int y, int zoomLevel, uint8 outBGRA[4])
{
	if (m_bReleased)
		return false;

	// Hold the source lock across the SetPyramidLevel + pixel read so another
	// thread cannot switch the IFD between the level-set and the read. The
	// lock is recursive so DecodeStrips/DecodeTile (which also lock) can be
	// called while this outer lock is held.
	LockSource();
	bool bOk = false;
	// zoomLevel > 0이면 임베디드 피라미드 레벨로 전환.
	if (zoomLevel > 0 && zoomLevel < m_nPyramidLevels)
	{
		if (SetPyramidLevel(zoomLevel))
		{
			// 피라미드 레벨에서의 좌표로 변환.
			int scaledW = m_nWidth >> zoomLevel;
			int scaledH = m_nHeight >> zoomLevel;
			if (scaledW < 1) scaledW = 1;
			if (scaledH < 1) scaledH = 1;
			if (x >= scaledW) x = scaledW - 1;
			if (y >= scaledH) y = scaledH - 1;
			if (x >= 0 && x < m_nWidth && y >= 0 && y < m_nHeight)
				bOk = ReadSinglePixel(x, y, outBGRA);
		}
	}
	else
	{
		if (SetPyramidLevel(0) && x >= 0 && x < m_nWidth && y >= 0 && y < m_nHeight)
			bOk = ReadSinglePixel(x, y, outBGRA);
	}
	UnlockSource();
	return bOk;
}

bool CLazySource::DecodeRegion(const CRect& sourceRect, int zoomLevel,
                                uint8* pDst, CSize dstSize)
{
	if (m_bReleased || pDst == nullptr)
		return false;
	if (dstSize.cx <= 0 || dstSize.cy <= 0)
		return false;

	// Hold the source lock across the SetPyramidLevel + decode so another
	// thread cannot switch the IFD between the level-set and the read. The
	// lock is recursive so DecodeStrips/DecodeTile (which also lock) can be
	// called while this outer lock is held.
	LockSource();
	bool bOk = false;
	// 피라미드 레벨 전환.
	if (zoomLevel > 0 && zoomLevel < m_nPyramidLevels)
	{
		if (SetPyramidLevel(zoomLevel))
			bOk = true;
	}
	else
	{
		if (SetPyramidLevel(0))
			bOk = true;
	}
	if (bOk)
	{
		// sourceRect를 현재 레벨의 이미지 경계로 클립.
		CRect rect = sourceRect;
		rect.IntersectRect(rect, CRect(0, 0, m_nWidth, m_nHeight));
		if (rect.Width() <= 0 || rect.Height() <= 0)
			bOk = false;
		else if (m_bTiled)
			bOk = DecodeRegionTiled(rect, zoomLevel, pDst, dstSize);
		else
			bOk = DecodeRegionStripped(rect, zoomLevel, pDst, dstSize);
	}
	UnlockSource();
	return bOk;
}

bool CLazySource::DecodeVisibleRegion(const CRect& viewportRect, int zoomLevel,
                                      uint8* pDst, CSize dstSize)
{
	if (m_bReleased || pDst == nullptr)
		return false;
	if (dstSize.cx <= 0 || dstSize.cy <= 0)
		return false;

	// Clip the viewport to image bounds and decode only that region. The
	// existing DecodeRegion already handles clipping and pyramid level
	// selection, so we delegate to it with the viewport rectangle instead of
	// the full image. The caller is responsible for zeroing the buffer first
	// so areas outside the viewport remain transparent.
	CRect rect = viewportRect;
	rect.IntersectRect(rect, CRect(0, 0, m_nWidth, m_nHeight));
	if (rect.Width() <= 0 || rect.Height() <= 0)
		return false;

	return DecodeRegion(rect, zoomLevel, pDst, dstSize);
}


bool CLazySource::DecodeRegionStripped(const CRect& sourceRect, int zoomLevel,
                                       uint8* pDst, CSize dstSize)
{
	// 뷰포트가 커버하는 스트립 범위 계산.
	int startStrip = sourceRect.top / m_nRowsPerStrip;
	int endStrip = (sourceRect.bottom - 1) / m_nRowsPerStrip;
	int stripCount = endStrip - startStrip + 1;

	// 스트립들을 디코드할 임시 버퍼.
	// 전체 폭 * 스트립 수 * 행당 행 수 만큼 필요.
	int totalRows = stripCount * m_nRowsPerStrip;
	int srcStride = m_nWidth * 4;
	uint8* pStrips = new (std::nothrow) uint8[(size_t)totalRows * srcStride];
	if (pStrips == nullptr)
		return false;

	if (!DecodeStrips(startStrip, stripCount, pStrips, srcStride))
	{
		delete[] pStrips;
		return false;
	}

	// 디코드된 스트립에서 sourceRect 영역을 pDst로 복사.
	// 스트립의 시작 행은 startStrip * m_nRowsPerStrip.
	int stripStartY = startStrip * m_nRowsPerStrip;
	int dstStride = dstSize.cx * 4;

	for (int y = 0; y < dstSize.cy; y++)
	{
		int srcY = sourceRect.top + y - stripStartY;
		if (srcY < 0 || srcY >= totalRows)
		{
			memset(pDst + (size_t)y * dstStride, 0, dstStride);
			continue;
		}
		const uint8* pSrcRow = pStrips + (size_t)srcY * srcStride;
		uint8* pDstRow = pDst + (size_t)y * dstStride;
		for (int x = 0; x < dstSize.cx; x++)
		{
			int srcX = sourceRect.left + x;
			if (srcX < 0 || srcX >= m_nWidth)
			{
				pDstRow[x * 4 + 0] = 0;
				pDstRow[x * 4 + 1] = 0;
				pDstRow[x * 4 + 2] = 0;
				pDstRow[x * 4 + 3] = 0xFF;
				continue;
			}
			const uint8* pSrc = pSrcRow + (size_t)srcX * 4;
			pDstRow[x * 4 + 0] = pSrc[0];
			pDstRow[x * 4 + 1] = pSrc[1];
			pDstRow[x * 4 + 2] = pSrc[2];
			pDstRow[x * 4 + 3] = pSrc[3];
		}
	}

	delete[] pStrips;
	return true;
}

bool CLazySource::DecodeRegionTiled(const CRect& sourceRect, int zoomLevel,
                                    uint8* pDst, CSize dstSize)
{
	// 뷰포트가 커버하는 타일 범위 계산.
	int startTileX = sourceRect.left / m_nTileWidth;
	int endTileX = (sourceRect.right - 1) / m_nTileWidth;
	int startTileY = sourceRect.top / m_nTileHeight;
	int endTileY = (sourceRect.bottom - 1) / m_nTileHeight;

	int dstStride = dstSize.cx * 4;

	// 각 타일을 디코드하여 pDst의 해당 위치에 복사.
	for (int ty = startTileY; ty <= endTileY; ty++)
	{
		for (int tx = startTileX; tx <= endTileX; tx++)
		{
			uint8* pTile = new (std::nothrow) uint8[(size_t)m_nTileWidth * m_nTileHeight * 4];
			if (pTile == nullptr)
				return false;
			if (!DecodeTile(tx, ty, pTile))
			{
				delete[] pTile;
				continue; // 일부 타일 실패해도 계속
			}

			// 타일의 전역 좌표.
			int tileOriginX = tx * m_nTileWidth;
			int tileOriginY = ty * m_nTileHeight;

			// 타일과 sourceRect의 교집합을 pDst에 복사.
			for (int ly = 0; ly < m_nTileHeight; ly++)
			{
				int globalY = tileOriginY + ly;
				int dstY = globalY - sourceRect.top;
				if (dstY < 0 || dstY >= dstSize.cy)
					continue;
				for (int lx = 0; lx < m_nTileWidth; lx++)
				{
					int globalX = tileOriginX + lx;
					int dstX = globalX - sourceRect.left;
					if (dstX < 0 || dstX >= dstSize.cx)
						continue;
					const uint8* pSrc = pTile + ((size_t)ly * m_nTileWidth + lx) * 4;
					uint8* pDstPixel = pDst + ((size_t)dstY * dstSize.cx + dstX) * 4;
					pDstPixel[0] = pSrc[0];
					pDstPixel[1] = pSrc[1];
					pDstPixel[2] = pSrc[2];
					pDstPixel[3] = pSrc[3];
				}
			}
			delete[] pTile;
		}
	}
	return true;
}