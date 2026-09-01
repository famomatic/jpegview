#include "stdafx.h"
#include "LazySource.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>

CLazySource::CLazySource()
	: m_nWidth(0), m_nHeight(0), m_nBaseWidth(0), m_nBaseHeight(0)
	, m_nChannels(4), m_nBitsPerSample(8)
	, m_bHasAlpha(false), m_nFrameCount(1), m_nCurrentFrame(0)
	, m_nPyramidLevels(1)
	, m_bTiled(false), m_nRowsPerStrip(0), m_nTileWidth(0), m_nTileHeight(0)
	, m_nStripsPerImage(0), m_nTilesPerImage(0)
	, m_pICCProfile(nullptr), m_nICCSize(0)
	, m_pEXIFData(nullptr), m_nEXIFSize(0)
	, m_pRawMetadata(nullptr)
	, m_bReleased(false)
	, m_nSourceRevision(0), m_nCachedRevision(-1)
	, m_nCachedUnitX(-1), m_nCachedUnitY(-1), m_bCachedUnitTiled(false)
{
}

CLazySource::~CLazySource()
{
	Release();
}

void CLazySource::Release()
{
	CSourceLockGuard lock(*this);
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
	m_singlePixelCache.clear();
}

void CLazySource::InvalidateSinglePixelCache()
{
	++m_nSourceRevision;
	m_nCachedRevision = -1;
	m_singlePixelCache.clear();
}

bool CLazySource::ReadSinglePixel(int x, int y, uint8 outBGRA[4])
{
	// 기본 구현: 해당 픽셀이 속한 스트립 하나를 디코드해서 읽는다.
	// 서브클래스가 더 효율적인 방법을 제공하면 오버라이드한다.
	if (m_bTiled)
	{
		if (m_nTileWidth <= 0 || m_nTileHeight <= 0)
			return false;
		int tileX = x / m_nTileWidth;
		int tileY = y / m_nTileHeight;
		if (m_nCachedRevision != m_nSourceRevision || !m_bCachedUnitTiled ||
			m_nCachedUnitX != tileX || m_nCachedUnitY != tileY)
		{
			try
			{
				const size_t tilePixels = static_cast<size_t>(m_nTileWidth) * m_nTileHeight;
				if (tilePixels / static_cast<size_t>(m_nTileWidth) != static_cast<size_t>(m_nTileHeight) || tilePixels > SIZE_MAX / 4)
					return false;
				m_singlePixelCache.resize(tilePixels * 4);
			}
			catch (const std::bad_alloc&)
			{
				return false;
			}
			if (!DecodeTile(tileX, tileY, m_singlePixelCache.data()))
				return false;
			m_nCachedRevision = m_nSourceRevision;
			m_nCachedUnitX = tileX;
			m_nCachedUnitY = tileY;
			m_bCachedUnitTiled = true;
		}
		int localX = x % m_nTileWidth;
		int localY = y % m_nTileHeight;
		const uint8* p = m_singlePixelCache.data() + ((size_t)localY * m_nTileWidth + localX) * 4;
		outBGRA[0] = p[0]; outBGRA[1] = p[1]; outBGRA[2] = p[2]; outBGRA[3] = p[3];
		return true;
	}
	else
	{
		if (m_nRowsPerStrip <= 0 || m_nWidth <= 0)
			return false;
		int strip = y / m_nRowsPerStrip;
		if (m_nWidth > INT_MAX / 4 || static_cast<size_t>(m_nRowsPerStrip) > SIZE_MAX / (static_cast<size_t>(m_nWidth) * 4))
			return false;
		int dstStride = m_nWidth * 4;
		if (m_nCachedRevision != m_nSourceRevision || m_bCachedUnitTiled ||
			m_nCachedUnitX != 0 || m_nCachedUnitY != strip)
		{
			try
			{
				m_singlePixelCache.resize((size_t)m_nRowsPerStrip * dstStride);
			}
			catch (const std::bad_alloc&)
			{
				return false;
			}
			if (!DecodeStrips(strip, 1, m_singlePixelCache.data(), dstStride))
				return false;
			m_nCachedRevision = m_nSourceRevision;
			m_nCachedUnitX = 0;
			m_nCachedUnitY = strip;
			m_bCachedUnitTiled = false;
		}
		int localY = y % m_nRowsPerStrip;
		const uint8* p = m_singlePixelCache.data() + ((size_t)localY * m_nWidth + x) * 4;
		outBGRA[0] = p[0]; outBGRA[1] = p[1]; outBGRA[2] = p[2]; outBGRA[3] = p[3];
		return true;
	}
}

bool CLazySource::SamplePoint(int x, int y, int zoomLevel, uint8 outBGRA[4])
{
	if (outBGRA == nullptr)
		return false;

	// Hold the source lock across the SetPyramidLevel + pixel read so another
	// thread cannot switch the IFD between the level-set and the read. The
	// lock is recursive so DecodeStrips/DecodeTile (which also lock) can be
	// called while this outer lock is held.
	CSourceLockGuard lock(*this);
	if (m_bReleased)
		return false;
	bool bOk = false;
	// zoomLevel > 0이면 임베디드 피라미드 레벨로 전환.
	if (zoomLevel > 0 && zoomLevel < m_nPyramidLevels)
	{
		if (SetPyramidLevel(zoomLevel))
		{
			// 피라미드 레벨에서의 좌표로 변환.
			if (m_nBaseWidth > 0 && m_nBaseHeight > 0) {
				x = (int)((int64_t)x * m_nWidth / m_nBaseWidth);
				y = (int)((int64_t)y * m_nHeight / m_nBaseHeight);
			}
			if (x >= m_nWidth) x = m_nWidth - 1;
			if (y >= m_nHeight) y = m_nHeight - 1;
			if (x >= 0 && x < m_nWidth && y >= 0 && y < m_nHeight)
				bOk = ReadSinglePixel(x, y, outBGRA);
		}
	}
	else
	{
		if (SetPyramidLevel(0) && x >= 0 && x < m_nWidth && y >= 0 && y < m_nHeight)
			bOk = ReadSinglePixel(x, y, outBGRA);
	}
	return bOk;
}

bool CLazySource::DecodeRegion(const CRect& sourceRect, int zoomLevel,
                                uint8* pDst, CSize dstSize)
{
	if (pDst == nullptr)
		return false;
	if (dstSize.cx <= 0 || dstSize.cy <= 0)
		return false;

	// Hold the source lock across the SetPyramidLevel + decode so another
	// thread cannot switch the IFD between the level-set and the read. The
	// lock is recursive so DecodeStrips/DecodeTile (which also lock) can be
	// called while this outer lock is held.
	CSourceLockGuard lock(*this);
	if (m_bReleased)
		return false;
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
		if (zoomLevel > 0 && m_nBaseWidth > 0 && m_nBaseHeight > 0) {
			rect.left = (int)((int64_t)rect.left * m_nWidth / m_nBaseWidth);
			rect.top = (int)((int64_t)rect.top * m_nHeight / m_nBaseHeight);
			rect.right = (int)(((int64_t)rect.right * m_nWidth + m_nBaseWidth - 1) / m_nBaseWidth);
			rect.bottom = (int)(((int64_t)rect.bottom * m_nHeight + m_nBaseHeight - 1) / m_nBaseHeight);
		}
		rect.IntersectRect(rect, CRect(0, 0, m_nWidth, m_nHeight));
		if (rect.Width() <= 0 || rect.Height() <= 0)
			bOk = false;
		else if (m_bTiled)
			bOk = DecodeRegionTiled(rect, zoomLevel, pDst, dstSize);
		else
			bOk = DecodeRegionStripped(rect, zoomLevel, pDst, dstSize);
	}
	return bOk;
}

bool CLazySource::DecodeVisibleRegion(const CRect& viewportRect, int zoomLevel,
                                      uint8* pDst, CSize dstSize)
{
	if (pDst == nullptr)
		return false;
	if (dstSize.cx <= 0 || dstSize.cy <= 0)
		return false;

	// Clip the viewport to image bounds and decode only that region. The
	// existing DecodeRegion already handles clipping and pyramid level
	// selection, so we delegate to it with the viewport rectangle instead of
	// the full image. The caller is responsible for zeroing the buffer first
	// so areas outside the viewport remain transparent.
	CRect rect = viewportRect;
	rect.IntersectRect(rect, CRect(0, 0, m_nBaseWidth, m_nBaseHeight));
	if (rect.Width() <= 0 || rect.Height() <= 0)
		return false;

	return DecodeRegion(rect, zoomLevel, pDst, dstSize);
}

bool CLazySource::DecodeResampledRegion(CSize fullTargetSize, CPoint targetOffset,
                                        CSize dstSize, uint8* pDst)
{
	if (pDst == nullptr || fullTargetSize.cx <= 0 || fullTargetSize.cy <= 0 ||
		dstSize.cx <= 0 || dstSize.cy <= 0 || targetOffset.x < 0 || targetOffset.y < 0 ||
		dstSize.cx > fullTargetSize.cx || dstSize.cy > fullTargetSize.cy ||
		targetOffset.x > fullTargetSize.cx - dstSize.cx ||
		targetOffset.y > fullTargetSize.cy - dstSize.cy)
		return false;

	CSourceLockGuard lock(*this);
	if (m_bReleased)
		return false;
	bool bOk = false;
	try
	{
		// Prefer an embedded pyramid level that is still at least as large as
		// the requested target.  Non-pyramidal TIFFs stay at level zero.
		int level = 0;
		// SUBIFD pyramids are not required to use exact powers of two. Probe
		// their real dimensions instead of estimating them with a bit shift.
		for (int candidate = 1; candidate < m_nPyramidLevels; ++candidate) {
			if (!SetPyramidLevel(candidate))
				break;
			if (m_nWidth < fullTargetSize.cx || m_nHeight < fullTargetSize.cy) {
				SetPyramidLevel(level);
				break;
			}
			level = candidate;
		}
		if (!SetPyramidLevel(level) || m_nWidth <= 0 || m_nHeight <= 0)
			throw std::runtime_error("invalid lazy image level");

		std::vector<int> sourceX((size_t)dstSize.cx);
		std::vector<int> sourceY((size_t)dstSize.cy);
		const bool downsamplingX = fullTargetSize.cx <= m_nWidth;
		const bool downsamplingY = fullTargetSize.cy <= m_nHeight;
		auto fillCoordinates = [](std::vector<int>& coords, int sourceSize,
			int targetSize, int targetStart, bool downsample) {
			uint64_t increment;
			if (downsample) {
				increment = ((uint64_t)sourceSize << 16) / targetSize + 1;
			} else {
				increment = (targetSize == 1) ? 0 :
					(65536ULL * (uint64_t)(sourceSize - 1) + 65535) / (targetSize - 1);
			}
			uint64_t position = (uint64_t)targetStart * increment;
			for (size_t i = 0; i < coords.size(); ++i) {
				uint64_t value = position >> 16;
				coords[i] = (int)min((uint64_t)(sourceSize - 1), value);
				position += increment;
			}
		};
		fillCoordinates(sourceX, m_nWidth, fullTargetSize.cx, targetOffset.x, downsamplingX);
		fillCoordinates(sourceY, m_nHeight, fullTargetSize.cy, targetOffset.y, downsamplingY);

		bOk = m_bTiled ? ResampleTiled(sourceX, sourceY, pDst, dstSize)
		                 : ResampleStripped(sourceX, sourceY, pDst, dstSize);
	}
	catch (const std::bad_alloc&)
	{
		bOk = false;
	}
	catch (const std::exception&)
	{
		bOk = false;
	}
	return bOk;
}

bool CLazySource::ResampleStripped(const std::vector<int>& sourceX,
	                                const std::vector<int>& sourceY,
	                                uint8* pDst, CSize dstSize)
{
	if (m_nRowsPerStrip <= 0 || m_nWidth <= 0 || sourceX.empty() || sourceY.empty())
		return false;

	const size_t stride = (size_t)m_nWidth * 4;
	const size_t stripBytes = stride * (size_t)m_nRowsPerStrip;
	if (stride / 4 != (size_t)m_nWidth || stripBytes / stride != (size_t)m_nRowsPerStrip)
		return false;
	std::vector<uint8> strip(stripBytes);

	int outY = 0;
	while (outY < dstSize.cy) {
		int stripIndex = sourceY[(size_t)outY] / m_nRowsPerStrip;
		if (stripIndex < 0 || stripIndex >= m_nStripsPerImage ||
			!DecodeStrips(stripIndex, 1, strip.data(), (int)stride))
			return false;

		int nextY = outY + 1;
		while (nextY < dstSize.cy && sourceY[(size_t)nextY] / m_nRowsPerStrip == stripIndex)
			++nextY;
		for (int dy = outY; dy < nextY; ++dy) {
			int localY = sourceY[(size_t)dy] - stripIndex * m_nRowsPerStrip;
			const uint8* srcRow = strip.data() + (size_t)localY * stride;
			uint8* dstRow = pDst + (size_t)dy * dstSize.cx * 4;
			for (int dx = 0; dx < dstSize.cx; ++dx) {
				const uint8* src = srcRow + (size_t)sourceX[(size_t)dx] * 4;
				memcpy(dstRow + (size_t)dx * 4, src, 4);
			}
		}
		outY = nextY;
	}
	return true;
}

bool CLazySource::ResampleTiled(const std::vector<int>& sourceX,
	                             const std::vector<int>& sourceY,
	                             uint8* pDst, CSize dstSize)
{
	if (m_nTileWidth <= 0 || m_nTileHeight <= 0 || sourceX.empty() || sourceY.empty())
		return false;
	const size_t tilePixels = (size_t)m_nTileWidth * m_nTileHeight;
	if (tilePixels / (size_t)m_nTileWidth != (size_t)m_nTileHeight ||
		tilePixels > SIZE_MAX / 4)
		return false;
	std::vector<uint8> tile(tilePixels * 4);

	int yBegin = 0;
	while (yBegin < dstSize.cy) {
		int tileY = sourceY[(size_t)yBegin] / m_nTileHeight;
		int yEnd = yBegin + 1;
		while (yEnd < dstSize.cy && sourceY[(size_t)yEnd] / m_nTileHeight == tileY)
			++yEnd;

		int xBegin = 0;
		while (xBegin < dstSize.cx) {
			int tileX = sourceX[(size_t)xBegin] / m_nTileWidth;
			int xEnd = xBegin + 1;
			while (xEnd < dstSize.cx && sourceX[(size_t)xEnd] / m_nTileWidth == tileX)
				++xEnd;
			if (!DecodeTile(tileX, tileY, tile.data()))
				return false;

			for (int dy = yBegin; dy < yEnd; ++dy) {
				int localY = sourceY[(size_t)dy] - tileY * m_nTileHeight;
				uint8* dstRow = pDst + (size_t)dy * dstSize.cx * 4;
				for (int dx = xBegin; dx < xEnd; ++dx) {
					int localX = sourceX[(size_t)dx] - tileX * m_nTileWidth;
					const uint8* src = tile.data() + ((size_t)localY * m_nTileWidth + localX) * 4;
					memcpy(dstRow + (size_t)dx * 4, src, 4);
				}
			}
			xBegin = xEnd;
		}
		yBegin = yEnd;
	}
	return true;
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
				return false;
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
