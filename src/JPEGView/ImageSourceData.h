#pragma once

//
// IImageSourceData — 픽셀 데이터 제공 추상 인터페이스.
//
// CJPEGImage는 더 이상 전체 픽셀 버퍼를 직접 소유하지 않고 이 인터페이스에
// 픽셀 접근을 위임한다. 두 가지 구현체가 같은 인터페이스를 구현한다:
//
//   CFullBufferSource  — 기존 래퍼가 전체 디코드한 버퍼를 감싼다 (JPEG/PNG/WebP/...).
//   CLazySource        — 뷰포트/포인트 요청 시에만 디코드한다 (TIFF, 향후 확장).
//
// 이중 분리 덕분에 초고화질 이미지를 전체 디코드 없이 볼 수 있고, 모든 포맷이
// 동일한 경로를 타므로 "범용 프레임워크"가 된다.
//
// 자세한 설계는 docs/ultra-high-res-design.md 참조.
//

#include "ImageProcessingTypes.h"

// MFC/ATL 타입 (CRect/CSize/CPoint) — stdafx.h 경유로 atlmisc.h에서 제공.
// 이 헤더는 stdafx.h 이후에 include되는 것을 전제로 한다.

class CRawMetadata;

class IImageSourceData
{
public:
	virtual ~IImageSourceData() {}

	// --- 메타데이터 (생성 즉시 사용 가능, 디코드 불필요) ---

	// 원본 이미지의 폭/높이 (픽셀)
	virtual int  Width() const = 0;
	virtual int  Height() const = 0;

	// 채널 수: 1(그레이스케일) / 3(RGB) / 4(BGRA)
	virtual int  Channels() const = 0;

	// 샘플당 비트: 8 / 16 / 32
	virtual int  BitsPerSample() const = 0;

	// 비자명한 알파 채널 존재 여부
	virtual bool HasAlpha() const = 0;

	// 멀티프레임: 전체 프레임 수, 현재 프레임, 프레임 전환
	virtual int  FrameCount() const = 0;
	virtual int  CurrentFrame() const = 0;
	virtual bool SetFrame(int nFrame) = 0;

	// --- 뷰포트 디코드 ---
	//
	// sourceRect: 원본 이미지 좌표계에서 디코드할 영역.
	// zoomLevel: 피라미드 레벨. 0=원본, 1=1/2, 2=1/4, ...
	//            임베디드 피라미드가 있으면 해당 레벨의 IFD에서 읽고,
	//            없으면 level 0에서 다운샘플링한다 (CImagePyramid가 처리).
	// pDst: 호출자가 dstSize.cx * dstSize.cy * 4 바이트를 사전 할당한
	//       32bpp BGRA 버퍼. 채널 순서는 B,G,R,A.
	// dstSize: 출력 버퍼의 폭/높이. 보통 sourceRect 크기를 zoomLevel 만큼
	//          축소한 값이지만, 리샘플러가 원하는 크기를 그대로 받는다.
	// 반환값: false = 영역이 이미지 밖이거나 디코드 실패.
	virtual bool DecodeRegion(const CRect& sourceRect, int zoomLevel,
	                          uint8* pDst, CSize dstSize) = 0;

	// --- 포인트 샘플 (LDC/히스토그램용) ---
	//
	// (x, y) 한 픽셀의 BGRA 값을 반환한다. 전체 디코드 없이 임의 좌표를
	// 읽을 수 있어야 한다. LDC는 이 메서드로 120,000포인트를 샘플링한다.
	// zoomLevel은 보통 0 (원본 기준). 줌아웃 상태에서 일관된 통계가
	// 필요하면 다른 레벨을 지정할 수 있다.
	// 반환값: false = 좌표가 이미지 밖이거나 읽기 실패.
	virtual bool SamplePoint(int x, int y, int zoomLevel,
	                         uint8 outBGRA[4]) = 0;

	// --- 피라미드 ---
	//
	// 임베디드 피라미드 레벨 수. 1이면 원본만 있어 폴백 대상.
	// pyramidal TIFF는 여러 IFD로 밉 레벨을 가지므로 > 1을 반환한다.
	virtual int  PyramidLevelCount() const = 0;

	// --- 메타데이터 부가 ---
	//
	// ICC 프로파일. nSize에 바이트 수를 채운다. 없으면 NULL 반환.
	// 반환 포인터는 소스 객체가 살아있는 동안 유효하다 (caller free 아님).
	virtual const uint8* ICCProfile(unsigned int& nSize) const = 0;

	// EXIF APP1 블록. nSize에 바이트 수를 채운다. 없으면 NULL 반환.
	// caller가 free() 해야 한다 (래퍼 구현에 따라 복사본 반환).
	virtual void* EXIFData(int& nSize) const = 0;

	// RAW 카메라 메타데이터. 없으면 NULL 반환.
	// 반환 포인터는 소스 객체가 소유한다 (caller free 아님).
	virtual CRawMetadata* RawMetadata() const = 0;

	// --- 리소스 ---
	//
	// 열려있는 파일 핸들, 디코더 컨텍스트 등을 해제한다.
	// 소멸자에서 자동으로 호출되지만, 명시적 해제가 필요할 때 사용.
	virtual void Release() = 0;
};
