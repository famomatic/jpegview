#pragma once

//
// CImagePyramid — 줌아웃/썸네일용 피라미드 관리.
//
// IImageSourceData 위에서 동작하는 보조 객체로, 두 가지 피라미드 전략을
// "둘 다 지원"한다:
//
//   1. 임베디드 피라미드 우선 — 소스가 PyramidLevelCount() > 1을 반환하면
//      (pyramidal TIFF 등) 해당 레벨의 IFD에서 직접 디코드.
//
//   2. 즉석 다운샘플링 폴백 — 소스가 레벨 1만 가지면(일반 스트라이프 TIFF),
//      줌아웃 시 원본에서 다운샘플링한 결과를 LRU 캐시에 저장.
//      재방문은 캐시 적중으로 즉시, 첫 방문만 다운샘플링 비용.
//
// 캐시는 메모리 예산 기반으로 동작하며, 지정된 바이트를 초과하면
// 가장 오래된 타일을 방출한다.
//

#include "ImageSourceData.h"
#include <list>

class CImagePyramid
{
public:
	// pSource: 픽셀을 제공할 소스 (비소유, CJPEGImage가 소유).
	// nMaxCacheBytes: 즉석 다운샘플링 캐시 예산. 0이면 캐시 비활성화.
	CImagePyramid(IImageSourceData* pSource, size_t nMaxCacheBytes = 512 * 1024 * 1024);
	~CImagePyramid();

	// 복사/이동 금지
	CImagePyramid(const CImagePyramid&) = delete;
	CImagePyramid& operator=(const CImagePyramid&) = delete;

	// 줌아웃 요청에 적합한 피라미드 레벨을 선택한다.
	// fullTargetSize: 줌 후의 가상 이미지 크기 (화면에 맞춘 크기).
	// 반환값: 0=원본, 1=1/2, 2=1/4, ...
	int SelectLevel(CSize fullTargetSize) const;

	// 뷰포트 디코드를 소스에 위임하되, 적절한 레벨을 선택하고
	// 즉석 다운샘플링 폴백 시 캐시를 활용한다.
	// srcRect: 원본 좌표계의 디코드 영역.
	// level: SelectLevel()이 반환한 레벨.
	// pDst: dstSize.cx * dstSize.cy * 4 바이트 BGRA 버퍼 (호출자 할당).
	// dstSize: 출력 크기.
	// 반환값: false = 디코드 실패.
	bool DecodeForViewport(const CRect& srcRect, int level,
	                       uint8* pDst, CSize dstSize);

	// 캐시를 모두 비운다 (이미지 전환 시 등).
	void ClearCache();

	// 현재 캐시 사용량 (바이트).
	size_t CacheBytes() const { return m_nCacheBytes; }

private:
	IImageSourceData* m_pSource;     // 비소유
	int m_nEmbeddedLevels;           // 소스가 제공하는 레벨 수
	size_t m_nMaxCacheBytes;         // 캐시 예산

	// 즉석 다운샘플링 캐시 엔트리.
	// (level, region) 키로 디코드된 타일을 저장.
	struct CacheEntry
	{
		int level;
		CRect region;
		uint8* pixels;
		CSize size;        // 픽셀 폭/높이
		size_t bytes;
		// LRU 순서는 m_lru의 위치로 표현 (front = 최신).
	};
	std::list<CacheEntry> m_lru;
	size_t m_nCacheBytes;

	// 캐시에서 (level, region)에 해당하는 엔트리를 찾는다.
	// 찾으면 m_lru의 맨 앞으로 이동하고 픽셀 포인터를 반환.
	// 못 찾으면 nullptr.
	uint8* FindInCache(int level, const CRect& region, CSize& outSize);

	// 캐시에 새 엔트리를 추가한다. 예산 초과 시 LRU 방출.
	void AddToCache(int level, const CRect& region, uint8* pPixels, CSize size);

	// 예산을 초과하면 가장 오래된 엔트리를 방출한다.
	void EvictIfNeeded();

	// 캐시 키 비교 (CRect == 연산자가 있지만 명시적으로).
	static bool RegionEqual(const CRect& a, const CRect& b);
};
