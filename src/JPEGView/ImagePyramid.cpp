#include "stdafx.h"
#include "ImagePyramid.h"
#include <cmath>
#include <algorithm>

CImagePyramid::CImagePyramid(IImageSourceData* pSource, size_t nMaxCacheBytes)
	: m_pSource(pSource)
	, m_nEmbeddedLevels(pSource ? pSource->PyramidLevelCount() : 1)
	, m_nMaxCacheBytes(nMaxCacheBytes)
	, m_nCacheBytes(0)
{
	if (m_nEmbeddedLevels < 1)
		m_nEmbeddedLevels = 1;
}

CImagePyramid::~CImagePyramid()
{
	ClearCache();
}

int CImagePyramid::SelectLevel(CSize fullTargetSize) const
{
	if (m_pSource == nullptr)
		return 0;
	int srcW = m_pSource->Width();
	int srcH = m_pSource->Height();
	if (fullTargetSize.cx >= srcW && fullTargetSize.cy >= srcH)
		return 0;
	double ratioX = (fullTargetSize.cx > 0) ? (double)srcW / fullTargetSize.cx : 1.0;
	double ratioY = (fullTargetSize.cy > 0) ? (double)srcH / fullTargetSize.cy : 1.0;
	double ratio = (ratioX > ratioY) ? ratioX : ratioY;
	if (ratio < 2.0)
		return 0;
	int level = (int)(log(ratio) / log(2.0));
	if (level < 0)
		level = 0;
	if (m_nEmbeddedLevels > 1 && level >= m_nEmbeddedLevels)
		level = m_nEmbeddedLevels - 1;
	return level;
}

bool CImagePyramid::DecodeForViewport(const CRect& srcRect, int level,
                                      uint8* pDst, CSize dstSize)
{
	if (m_pSource == nullptr || pDst == nullptr)
		return false;
	if (dstSize.cx <= 0 || dstSize.cy <= 0)
		return false;

	// 임베디드 피라미드가 있거나 level 0이면 소스에 직접 위임.
	if (m_nEmbeddedLevels > 1 || level <= 0)
	{
		return m_pSource->DecodeRegion(srcRect, level, pDst, dstSize);
	}

	// 즉석 다운샘플링 폴백: 캐시 조회 후 level 0 디코드 + 다운샘플.
	CSize cachedSize;
	uint8* pCached = FindInCache(level, srcRect, cachedSize);
	if (pCached != nullptr)
	{
		int copyW = (cachedSize.cx < dstSize.cx) ? cachedSize.cx : dstSize.cx;
		int copyH = (cachedSize.cy < dstSize.cy) ? cachedSize.cy : dstSize.cy;
		for (int y = 0; y < copyH; y++)
		{
			memcpy(pDst + (size_t)y * dstSize.cx * 4,
			       pCached + (size_t)y * cachedSize.cx * 4,
			       (size_t)copyW * 4);
		}
		return true;
	}

	int srcW = srcRect.Width();
	int srcH = srcRect.Height();
	if (srcW <= 0 || srcH <= 0)
		return false;

	uint8* pLevel0 = new (std::nothrow) uint8[(size_t)srcW * srcH * 4];
	if (pLevel0 == nullptr)
		return false;

	if (!m_pSource->DecodeRegion(srcRect, 0, pLevel0, CSize(srcW, srcH)))
	{
		delete[] pLevel0;
		return false;
	}

	int scale = 1 << level;
	for (int y = 0; y < dstSize.cy; y++)
	{
		int srcY = (y * scale) + (scale / 2);
		if (srcY >= srcH) srcY = srcH - 1;
		uint8* pDstRow = pDst + (size_t)y * dstSize.cx * 4;
		const uint8* pSrcRow = pLevel0 + (size_t)srcY * srcW * 4;
		for (int x = 0; x < dstSize.cx; x++)
		{
			int srcX = (x * scale) + (scale / 2);
			if (srcX >= srcW) srcX = srcW - 1;
			const uint8* pSrc = pSrcRow + (size_t)srcX * 4;
			pDstRow[x * 4 + 0] = pSrc[0];
			pDstRow[x * 4 + 1] = pSrc[1];
			pDstRow[x * 4 + 2] = pSrc[2];
			pDstRow[x * 4 + 3] = pSrc[3];
		}
	}

	if (m_nMaxCacheBytes > 0)
	{
		uint8* pCachedResult = new (std::nothrow) uint8[(size_t)dstSize.cx * dstSize.cy * 4];
		if (pCachedResult != nullptr)
		{
			memcpy(pCachedResult, pDst, (size_t)dstSize.cx * dstSize.cy * 4);
			AddToCache(level, srcRect, pCachedResult, dstSize);
		}
	}

	delete[] pLevel0;
	return true;
}

bool CImagePyramid::RegionEqual(const CRect& a, const CRect& b)
{
	return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

uint8* CImagePyramid::FindInCache(int level, const CRect& region, CSize& outSize)
{
	for (auto it = m_lru.begin(); it != m_lru.end(); ++it)
	{
		if (it->level == level && RegionEqual(it->region, region))
		{
			if (it != m_lru.begin())
			{
				m_lru.splice(m_lru.begin(), m_lru, it);
			}
			outSize = it->size;
			return it->pixels;
		}
	}
	return nullptr;
}

void CImagePyramid::AddToCache(int level, const CRect& region, uint8* pPixels, CSize size)
{
	CacheEntry entry;
	entry.level = level;
	entry.region = region;
	entry.pixels = pPixels;
	entry.size = size;
	entry.bytes = (size_t)size.cx * size.cy * 4;
	m_nCacheBytes += entry.bytes;
	m_lru.push_front(std::move(entry));
	EvictIfNeeded();
}

void CImagePyramid::EvictIfNeeded()
{
	while (m_nCacheBytes > m_nMaxCacheBytes && !m_lru.empty())
	{
		auto& last = m_lru.back();
		m_nCacheBytes -= last.bytes;
		delete[] last.pixels;
		m_lru.pop_back();
	}
}

void CImagePyramid::ClearCache()
{
	for (auto& entry : m_lru)
	{
		delete[] entry.pixels;
	}
	m_lru.clear();
	m_nCacheBytes = 0;
}
