#pragma once

//
// CTiffLazySource — libtiff 기반 지연 부분 디코드.
//
// CLazySource를 상속해 libtiff 호출을 구현한다.
// 생성 시 TIFFOpen으로 파일을 열고 메타데이터만 읽는다.
// DecodeRegion/SamplePoint가 호출될 때 해당 스트립/타일만
// TIFFReadEncodedStrip/TIFFReadEncodedTile로 디코드한다.
//
// pyramidal TIFF의 경우 IFD를 순회해 임베디드 밉 레벨을 감지하고,
// SetPyramidLevel로 해당 IFD로 전환한다.
//

#include "LazySource.h"
#include <tiffio.h>
#include <vector>
#include <mutex>

class CTiffLazySource : public CLazySource
{
public:
	static CTiffLazySource* Create(LPCTSTR strFileName, int nFrameIndex, bool& bOutOfMemory);
	~CTiffLazySource() override;

	bool DecodeStrips(int startStrip, int stripCount,
	                  uint8* pDst, int dstStride) override;
	bool DecodeTile(int tileX, int tileY, uint8* pDst) override;
	bool SetPyramidLevel(int level) override;
	bool SetFrame(int nFrame) override;

	const uint8* ICCProfile(unsigned int& nSize) const override;
	void*        EXIFData(int& nSize) const override;
	CRawMetadata* RawMetadata() const override;
	void Release() override;

	// Serialize the SetPyramidLevel + decode sequence against concurrent
	// access. m_tifLock is recursive so DecodeStrips/DecodeTile can re-enter.
	void LockSource() override { m_tifLock.lock(); }
	void UnlockSource() override { m_tifLock.unlock(); }

private:
	CTiffLazySource();
	bool OpenAndReadMetadata(LPCTSTR strFileName, int nFrameIndex);
	int DetectPyramidLevels();
	bool DecodeSingleStrip(int stripIndex, uint8* pDst, int dstStride);
	void ConvertStripToBGRA(const uint8* pSrc, uint8* pDst,
	                         int width, int rowsInStrip,
	                         int srcStride, int dstStride);

	TIFF* m_tif;
	CString m_sFileName;
	int m_nFrameIndex;
	uint16 m_photometric;
	uint16 m_planarConfig;
	uint16 m_sampleFormat;
	uint16 m_compression;
	std::vector<int> m_pyramidIFDs;
	int m_nCurrentPyramidLevel;
	bool m_bUseRGBA;

	// Serializes all access to m_tif. libtiff is not thread-safe and a single
	// TIFF* cannot be touched concurrently; the resampler (ProcessingThreadPool)
	// and LDC/histogram (SamplePoint) can both reach this source at once.
	// Recursive so that SamplePoint/DecodeRegion can hold the lock across the
	// SetPyramidLevel + decode sequence, preventing another thread from
	// switching the IFD between the level-set and the pixel read.
	std::recursive_mutex m_tifLock;
};
// end
