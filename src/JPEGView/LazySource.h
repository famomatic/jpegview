#pragma once

//
// CLazySource — 지연 부분 디코드 베이스.
//
// IImageSourceData의 지연 구현체. 생성 시 메타데이터만 읽고, 픽셀은
// DecodeRegion/SamplePoint가 호출될 때 해당 스트립/타일만 디코드한다.
//
// 현재 CTiffLazySource가 이 베이스를 상속해 libtiff 호출을 구현한다.
// 인터페이스는 포맷 중립적이라 나중에 JPEG 2000, JXL 등이 부분 디코드를
// 지원할 때 같은 베이스를 상속해 확장할 수 있다.
//
// 핵심 최적화: RowsPerStrip=2 같은 잘게 쪼개진 스트립을 가진 이미지에서
// 뷰포트에 해당하는 스트립만 배치로 읽어 오버헤드를 줄인다.
//

#include "ImageSourceData.h"
#include <cstring>

class CLazySource : public IImageSourceData
{
public:
	CLazySource();
	virtual ~CLazySource();

	// 복사/이동 금지
	CLazySource(const CLazySource&) = delete;
	CLazySource& operator=(const CLazySource&) = delete;

	// --- IImageSourceData 공통 구현 ---
	int  Width() const override { return m_nWidth; }
	int  Height() const override { return m_nHeight; }
	int  Channels() const override { return m_nChannels; }
	int  BitsPerSample() const override { return m_nBitsPerSample; }
	bool HasAlpha() const override { return m_bHasAlpha; }
	int  FrameCount() const override { return m_nFrameCount; }
	int  CurrentFrame() const override { return m_nCurrentFrame; }

	bool DecodeRegion(const CRect& sourceRect, int zoomLevel,
	                  uint8* pDst, CSize dstSize) override;
	bool SamplePoint(int x, int y, int zoomLevel,
	                 uint8 outBGRA[4]) override;

	// Decode only the visible viewport region. When ROI decode is enabled and
	// the image exceeds the megapixel threshold, this avoids decoding the full
	// image and instead decodes only the strips/tiles intersecting the viewport
	// rectangle. The rest of the output buffer is left as transparent/black.
	// Returns false if the source cannot do partial decode (caller should fall
	// back to full DecodeRegion).
	bool DecodeVisibleRegion(const CRect& viewportRect, int zoomLevel,
	                         uint8* pDst, CSize dstSize);

	int  PyramidLevelCount() const override { return m_nPyramidLevels; }

	void Release() override;

protected:
	// --- 서브클래스가 채울 필드 ---
	int  m_nWidth;
	int  m_nHeight;
	int  m_nChannels;
	int  m_nBitsPerSample;
	bool m_bHasAlpha;
	int  m_nFrameCount;
	int  m_nCurrentFrame;
	int  m_nPyramidLevels;

	// 스트립/타일 구조 (둘 중 하나가 유효)
	bool m_bTiled;
	int  m_nRowsPerStrip;
	int  m_nTileWidth;
	int  m_nTileHeight;
	int  m_nStripsPerImage;
	int  m_nTilesPerImage;

	// 메타데이터
	uint8* m_pICCProfile;
	unsigned int m_nICCSize;
	uint8* m_pEXIFData;
	int    m_nEXIFSize;
	CRawMetadata* m_pRawMetadata;

	bool m_bReleased;

	// --- 서브클래스가 구현할 훅 ---

	virtual bool DecodeStrips(int startStrip, int stripCount,
	                          uint8* pDst, int dstStride) = 0;
	virtual bool DecodeTile(int tileX, int tileY, uint8* pDst) = 0;
	virtual bool SetPyramidLevel(int level) = 0;
	virtual bool ReadSinglePixel(int x, int y, uint8 outBGRA[4]);

	// Acquire/release the source's internal serialization lock around a
	// SetPyramidLevel + decode sequence so another thread cannot switch the
	// IFD between the level-set and the pixel read. Default is a no-op;
	// CTiffLazySource overrides to lock its recursive_mutex. The lock is
	// recursive so DecodeStrips/DecodeTile (which also lock) can be called
	// while this outer lock is held.
	virtual void LockSource() {}
	virtual void UnlockSource() {}

private:
	bool DecodeRegionStripped(const CRect& sourceRect, int zoomLevel,
	                          uint8* pDst, CSize dstSize);
	bool DecodeRegionTiled(const CRect& sourceRect, int zoomLevel,
	                       uint8* pDst, CSize dstSize);
};
