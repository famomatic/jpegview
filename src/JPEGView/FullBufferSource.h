#pragma once

//
// CFullBufferSource — IImageSourceData의 즉시 전체 디코드 구현체.
//
// 기존 포맷 래퍼(JPEG/PNG/WebP/JXL/AVIF/HEIF/QOI/DDS/JP2/EXR/HDR/JXR/
// BMP/TGA/PSD/RAW/SVG)가 ReadImage로 디코드한 전체 픽셀 버퍼를 감싼다.
// 이 포맷들은 어차피 전체를 메모리에 올려야 디코드할 수 있으므로 부분
// 로딩의 이점이 없고, CFullBufferSource는 단순히 버퍼를 노출한다.
//
// DecodeRegion은 level 0에서는 메모리 복사 + 채널 변환이고,
// level > 0에서는 메모리 내 다운샘플링(피라미드 폴백)을 수행한다.
//

#include "ImageSourceData.h"

class CFullBufferSource : public IImageSourceData
{
public:
	// 생성자: 기존 래퍼가 디코드한 픽셀 버퍼의 소유권을 받는다.
	//   nWidth, nHeight  — 픽셀 단위 폭/높이
	//   nChannels        — 1(그레이) / 3(RGB) / 4(BGRA)
	//   nBitsPerSample   — 8 / 16 / 32 (현재는 8만 사용)
	//   pPixels          — 디코드된 픽셀 버퍼 (new[]로 할당된 것, 소유권 이전)
	//                      레이아웃: 행 단위, 행은 4바이트 경계로 패딩.
	//                      채널 순서: 3채널=BGR, 4채널=BGRA, 1채널=그레이.
	//   bHasAlpha        — 4채널이고 비자명한 알파가 있으면 true
	//   pEXIFData        — EXIF APP1 블록 (caller가 free해야 하는 복사본 반환용)
	//                      NULL 가능. 소유권 이전 (new[]).
	//   nEXIFSize        — EXIF 블록 바이트 수
	//   pICCProfile      — ICC 프로파일 (new[]로 할당, 소유권 이전). NULL 가능.
	//   nICCSize         — ICC 프로파일 바이트 수
	//   pRawMetadata     — RAW 카메라 메타데이터 (new로 할당, 소유권 이전). NULL 가능.
	//   nFrameCount      — 멀티프레임 이미지의 프레임 수 (보통 1)
	CFullBufferSource(int nWidth, int nHeight, int nChannels, int nBitsPerSample,
	                  uint8* pPixels, bool bHasAlpha,
	                  uint8* pEXIFData = nullptr, int nEXIFSize = 0,
	                  uint8* pICCProfile = nullptr, unsigned int nICCSize = 0,
	                  CRawMetadata* pRawMetadata = nullptr,
	                  int nFrameCount = 1);
	~CFullBufferSource() override;

	// 복사/이동 금지
	CFullBufferSource(const CFullBufferSource&) = delete;
	CFullBufferSource& operator=(const CFullBufferSource&) = delete;

	// --- IImageSourceData ---
	int  Width() const override { return m_nWidth; }
	int  Height() const override { return m_nHeight; }
	int  Channels() const override { return m_nChannels; }
	int  BitsPerSample() const override { return m_nBitsPerSample; }
	bool HasAlpha() const override { return m_bHasAlpha; }
	int  FrameCount() const override { return m_nFrameCount; }
	int  CurrentFrame() const override { return m_nCurrentFrame; }
	bool SetFrame(int nFrame) override;

	bool DecodeRegion(const CRect& sourceRect, int zoomLevel,
	                  uint8* pDst, CSize dstSize) override;
	bool SamplePoint(int x, int y, int zoomLevel,
	                 uint8 outBGRA[4]) override;

	int  PyramidLevelCount() const override { return 1; }

	const uint8* ICCProfile(unsigned int& nSize) const override;
	void*        EXIFData(int& nSize) const override;
	CRawMetadata* RawMetadata() const override;

	void Release() override;

	// --- 하위 호환 ---
	// 기존 코드가 OriginalPixels()로 전체 버퍼에 직접 접근하는 동안
	// 임시로 노출. CJPEGImage 마이그레이션 완료 후 제거 예정.
	const void* GetFullBuffer() const { return m_pPixels; }
	int GetStride() const { return m_nStride; }

	// Relinquishes ownership of the pixel buffer: returns m_pPixels and sets it
	// to null so the destructor/Release() will not free it. Used when a
	// destructive transform (Rotate/Mirror/Crop) needs to take over the buffer.
	uint8* DetachPixels() { uint8* p = m_pPixels; m_pPixels = nullptr; return p; }

private:
	int  m_nWidth;
	int  m_nHeight;
	int  m_nChannels;
	int  m_nBitsPerSample;
	bool m_bHasAlpha;
	int  m_nFrameCount;
	int  m_nCurrentFrame;

	uint8* m_pPixels;          // 전체 픽셀 버퍼 (소유)
	int    m_nStride;          // 행당 바이트 (4바이트 패딩 포함)

	uint8* m_pEXIFData;         // EXIF APP1 블록 (소유)
	int    m_nEXIFSize;
	uint8* m_pICCProfile;      // ICC 프로파일 (소유)
	unsigned int m_nICCSize;
	CRawMetadata* m_pRawMetadata;  // RAW 메타데이터 (소유)

	bool m_bReleased;

	// level 0에서 sourceRect 영역을 pDst로 복사 (채널 변환 포함).
	void CopyRegionLevel0(const CRect& sourceRect, uint8* pDst, CSize dstSize) const;

	// level > 0에서 메모리 내 다운샘플링 (포인트 샘플링 기반).
	void DownsampleRegion(const CRect& sourceRect, int zoomLevel,
	                      uint8* pDst, CSize dstSize) const;

	// 소스 버퍼에서 (x, y) 픽셀의 BGRA를 읽는다.
	void ReadPixelAt(int x, int y, uint8 outBGRA[4]) const;
};
