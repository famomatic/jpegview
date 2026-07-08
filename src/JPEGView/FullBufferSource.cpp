#include "stdafx.h"
#include "FullBufferSource.h"
#include "Helpers.h"
#include <cstring>

// ---------------------------------------------------------------------------
// 생성 / 소멸
// ---------------------------------------------------------------------------

CFullBufferSource::CFullBufferSource(int nWidth, int nHeight, int nChannels, int nBitsPerSample,
                                     uint8* pPixels, bool bHasAlpha,
                                     uint8* pEXIFData, int nEXIFSize,
                                     uint8* pICCProfile, unsigned int nICCSize,
                                     CRawMetadata* pRawMetadata,
                                     int nFrameCount)
	: m_nWidth(nWidth)
	, m_nHeight(nHeight)
	, m_nChannels(nChannels)
	, m_nBitsPerSample(nBitsPerSample)
	, m_bHasAlpha(bHasAlpha)
	, m_nFrameCount(nFrameCount)
	, m_nCurrentFrame(0)
	, m_pPixels(pPixels)
	, m_pEXIFData(pEXIFData)
	, m_nEXIFSize(nEXIFSize)
	, m_pICCProfile(pICCProfile)
	, m_nICCSize(nICCSize)
	, m_pRawMetadata(pRawMetadata)
	, m_bReleased(false)
{
	// 행당 바이트: 채널 수 * 폭, 4바이트 경계로 패딩.
	// 기존 래퍼들은 모두 이 레이아웃을 따른다 (Helpers::DoPadding 참조).
	m_nStride = Helpers::DoPadding(m_nWidth * m_nChannels, 4);
}

CFullBufferSource::~CFullBufferSource()
{
	Release();
}

void CFullBufferSource::Release()
{
	if (m_bReleased)
		return;
	m_bReleased = true;

	delete[] m_pPixels;
	m_pPixels = nullptr;

	delete[] m_pEXIFData;
	m_pEXIFData = nullptr;
	m_nEXIFSize = 0;

	delete[] m_pICCProfile;
	m_pICCProfile = nullptr;
	m_nICCSize = 0;

	delete m_pRawMetadata;
	m_pRawMetadata = nullptr;
}

// ---------------------------------------------------------------------------
// 멀티프레임
// ---------------------------------------------------------------------------

bool CFullBufferSource::SetFrame(int nFrame)
{
	// CFullBufferSource는 단일 프레임 버퍼만 들고 있다.
	// 멀티프레임 애니메이션(GIF/WebP-anim)은 현재 별도 경로를 타므로
	// 여기서는 프레임 0만 유효하다. 향후 애니메이션 프레임을 버퍼 배열로
	// 확장할 때 이 메서드를 채운다.
	if (nFrame < 0 || nFrame >= m_nFrameCount)
		return false;
	m_nCurrentFrame = nFrame;
	return (nFrame == 0);
}

// ---------------------------------------------------------------------------
// 메타데이터 접근
// ---------------------------------------------------------------------------

const uint8* CFullBufferSource::ICCProfile(unsigned int& nSize) const
{
	nSize = m_nICCSize;
	return m_pICCProfile;
}

void* CFullBufferSource::EXIFData(int& nSize) const
{
	// 인터페이스 계약: caller가 free() 해야 하므로 복사본을 반환한다.
	nSize = m_nEXIFSize;
	if (m_pEXIFData == nullptr || m_nEXIFSize == 0)
		return nullptr;
	uint8* pCopy = new (std::nothrow) uint8[m_nEXIFSize];
	if (pCopy != nullptr)
		memcpy(pCopy, m_pEXIFData, m_nEXIFSize);
	return pCopy;
}

CRawMetadata* CFullBufferSource::RawMetadata() const
{
	return m_pRawMetadata;
}

// ---------------------------------------------------------------------------
// 내부 헬퍼: 소스 버퍼에서 (x, y) 픽셀의 BGRA를 읽는다.
// ---------------------------------------------------------------------------

void CFullBufferSource::ReadPixelAt(int x, int y, uint8 outBGRA[4]) const
{
	const uint8* pRow = m_pPixels + (size_t)y * m_nStride;
	if (m_nChannels == 4)
	{
		const uint8* p = pRow + (size_t)x * 4;
		outBGRA[0] = p[0]; outBGRA[1] = p[1]; outBGRA[2] = p[2]; outBGRA[3] = p[3];
	}
	else if (m_nChannels == 3)
	{
		const uint8* p = pRow + (size_t)x * 3;
		outBGRA[0] = p[0]; outBGRA[1] = p[1]; outBGRA[2] = p[2]; outBGRA[3] = 0xFF;
	}
	else // 1채널 그레이스케일
	{
		uint8 g = pRow[x];
		outBGRA[0] = g; outBGRA[1] = g; outBGRA[2] = g; outBGRA[3] = 0xFF;
	}
}

// ---------------------------------------------------------------------------
// DecodeRegion
// ---------------------------------------------------------------------------

bool CFullBufferSource::DecodeRegion(const CRect& sourceRect, int zoomLevel,
                                     uint8* pDst, CSize dstSize)
{
	if (m_bReleased || pDst == nullptr)
		return false;
	if (dstSize.cx <= 0 || dstSize.cy <= 0)
		return false;

	// sourceRect를 이미지 경계로 클립
	CRect rect = sourceRect;
	rect.IntersectRect(rect, CRect(0, 0, m_nWidth, m_nHeight));
	if (rect.Width() <= 0 || rect.Height() <= 0)
		return false;

	if (zoomLevel <= 0)
	{
		CopyRegionLevel0(rect, pDst, dstSize);
	}
	else
	{
		DownsampleRegion(rect, zoomLevel, pDst, dstSize);
	}
	return true;
}

void CFullBufferSource::CopyRegionLevel0(const CRect& sourceRect, uint8* pDst, CSize dstSize) const
{
	// dstSize는 sourceRect 크기와 같다고 가정 (리샘플러가 1:1 매핑을 요청한 경우).
	// 다를 경우 첫 행/열만 채운다 (안전 장치).
	const int dstW = dstSize.cx;
	const int dstH = dstSize.cy;
	const int dstStride = dstW * 4;

	for (int y = 0; y < dstH; y++)
	{
		int srcY = sourceRect.top + y;
		if (srcY < 0 || srcY >= m_nHeight)
		{
			memset(pDst + (size_t)y * dstStride, 0, dstStride);
			continue;
		}
		uint8* pDstRow = pDst + (size_t)y * dstStride;
		for (int x = 0; x < dstW; x++)
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
			ReadPixelAt(srcX, srcY, pDstRow + x * 4);
		}
	}
}

void CFullBufferSource::DownsampleRegion(const CRect& sourceRect, int zoomLevel,
                                         uint8* pDst, CSize dstSize) const
{
	// 피라미드 폴백: level N은 원본을 2^N 배 축소.
	// 포인트 샘플링으로 간단히 구현. CImagePyramid가 더 정교한 다운샘플링을
	// 원하면 그쪽에서 처리하고, 여기서는 최후의 폴백만 제공한다.
	const int dstW = dstSize.cx;
	const int dstH = dstSize.cy;
	const int dstStride = dstW * 4;
	const int scale = 1 << zoomLevel;

	for (int y = 0; y < dstH; y++)
	{
		int srcY = sourceRect.top + (y * scale) + (scale / 2);
		if (srcY >= m_nHeight) srcY = m_nHeight - 1;
		uint8* pDstRow = pDst + (size_t)y * dstStride;
		for (int x = 0; x < dstW; x++)
		{
			int srcX = sourceRect.left + (x * scale) + (scale / 2);
			if (srcX >= m_nWidth) srcX = m_nWidth - 1;
			ReadPixelAt(srcX, srcY, pDstRow + x * 4);
		}
	}
}

// ---------------------------------------------------------------------------
// SamplePoint
// ---------------------------------------------------------------------------

bool CFullBufferSource::SamplePoint(int x, int y, int zoomLevel, uint8 outBGRA[4])
{
	if (m_bReleased)
		return false;
	if (x < 0 || x >= m_nWidth || y < 0 || y >= m_nHeight)
		return false;

	// zoomLevel > 0이면 좌표를 축소 레벨에 맞게 스케일.
	// 하지만 CFullBufferSource는 임베디드 피라미드가 없으므로
	// level 0 좌표로 변환해서 읽는다.
	if (zoomLevel > 0)
	{
		int scale = 1 << zoomLevel;
		x = (x * scale) + (scale / 2);
		y = (y * scale) + (scale / 2);
		if (x >= m_nWidth) x = m_nWidth - 1;
		if (y >= m_nHeight) y = m_nHeight - 1;
	}

	ReadPixelAt(x, y, outBGRA);
	return true;
}
