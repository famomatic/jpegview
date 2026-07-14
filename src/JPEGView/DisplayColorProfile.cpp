#include "stdafx.h"

#include "DisplayColorProfile.h"
#include "SettingsProvider.h"

#ifndef WINXP

#include "lcms2.h"
#include <vector>

namespace {

CRITICAL_SECTION& GetCS() {
	static CRITICAL_SECTION cs = []() { CRITICAL_SECTION c; ::InitializeCriticalSection(&c); return c; }();
	return cs;
}

HWND s_hWnd = NULL;
HMONITOR s_hCachedMonitor = NULL;
cmsHTRANSFORM s_hTransform = NULL;
bool s_bTransformValid = false; // s_hTransform (incl. NULL) matches s_hCachedMonitor
volatile LONG s_nVersion = 0;

// Returns the file name of the ICC profile of the monitor the window is on, empty string if none
CString GetMonitorProfilePath(HMONITOR hMonitor) {
	MONITORINFOEX monitorInfo;
	monitorInfo.cbSize = sizeof(MONITORINFOEX);
	if (!::GetMonitorInfo(hMonitor, &monitorInfo)) {
		return CString();
	}
	HDC hDC = ::CreateDC(NULL, monitorInfo.szDevice, NULL, NULL);
	if (hDC == NULL) {
		return CString();
	}
	TCHAR buffer[MAX_PATH];
	DWORD size = MAX_PATH;
	CString sProfile;
	if (::GetICMProfile(hDC, &size, buffer)) {
		sProfile = buffer;
	}
	::DeleteDC(hDC);
	return sProfile;
}

// Heuristic: profiles describing sRGB need no transform
bool IsSRGBProfileName(const CString& sPath) {
	CString sLower(sPath);
	sLower.MakeLower();
	return sLower.Find(_T("srgb")) >= 0;
}

void RecreateTransform() {
	if (s_hTransform != NULL) {
		// Never delete old transforms: GetTransform() hands out raw handles that may
		// still be in use by another thread in cmsDoTransform(). Transforms only change
		// on monitor/profile switches, so the retained set stays tiny.
		static std::vector<cmsHTRANSFORM> s_retiredTransforms;
		s_retiredTransforms.push_back(s_hTransform);
		s_hTransform = NULL;
	}
	s_bTransformValid = true;
	::InterlockedIncrement(&s_nVersion);

	CString sProfilePath = GetMonitorProfilePath(s_hCachedMonitor);
	if (sProfilePath.IsEmpty() || IsSRGBProfileName(sProfilePath)) {
		return; // no profile or sRGB - identity, no transform needed
	}
	cmsHPROFILE hMonitorProfile = NULL;
#ifdef _UNICODE
	// lcms file API is char* based - use short path to stay ANSI-safe
	char buffer[MAX_PATH];
	if (::WideCharToMultiByte(CP_ACP, 0, sProfilePath, -1, buffer, MAX_PATH, NULL, NULL) > 0) {
		hMonitorProfile = cmsOpenProfileFromFile(buffer, "r");
	}
	if (hMonitorProfile == NULL) {
		// fall back to reading the file into memory (handles non-ANSI paths)
		HANDLE hFile = ::CreateFile(sProfilePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (hFile != INVALID_HANDLE_VALUE) {
			DWORD nSize = ::GetFileSize(hFile, NULL);
			if (nSize != INVALID_FILE_SIZE && nSize > 0 && nSize < 64 * 1024 * 1024) {
				char* pBuffer = new(std::nothrow) char[nSize];
				DWORD nRead = 0;
				if (pBuffer != NULL && ::ReadFile(hFile, pBuffer, nSize, &nRead, NULL) && nRead == nSize) {
					hMonitorProfile = cmsOpenProfileFromMem(pBuffer, nSize);
				}
				delete[] pBuffer;
			}
			::CloseHandle(hFile);
		}
	}
#else
	hMonitorProfile = cmsOpenProfileFromFile(CStringA(sProfilePath), "r");
#endif
	if (hMonitorProfile == NULL) {
		return;
	}
	cmsHPROFILE hSRGBProfile = cmsCreate_sRGBProfile();
	if (hSRGBProfile != NULL) {
		// NOCACHE makes the transform safe for concurrent use from multiple threads
		s_hTransform = cmsCreateTransform(hSRGBProfile, TYPE_BGRA_8, hMonitorProfile, TYPE_BGRA_8,
			INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_COPY_ALPHA | cmsFLAGS_NOCACHE);
		cmsCloseProfile(hSRGBProfile);
	}
	cmsCloseProfile(hMonitorProfile);
}

} // namespace

void CDisplayColorProfile::SetWindow(HWND hWnd) {
	::EnterCriticalSection(&GetCS());
	s_hWnd = hWnd;
	s_bTransformValid = false;
	::LeaveCriticalSection(&GetCS());
}

void* CDisplayColorProfile::GetTransform() {
	if (!CSettingsProvider::This().UseDisplayColorProfile()) {
		return NULL;
	}
	::EnterCriticalSection(&GetCS());
	if (s_hWnd != NULL) {
		HMONITOR hMonitor = ::MonitorFromWindow(s_hWnd, MONITOR_DEFAULTTONEAREST);
		if (hMonitor != s_hCachedMonitor || !s_bTransformValid) {
			s_hCachedMonitor = hMonitor;
			RecreateTransform();
		}
	}
	void* hTransform = s_hTransform;
	::LeaveCriticalSection(&GetCS());
	return hTransform;
}

int CDisplayColorProfile::GetVersion() {
	return (int)s_nVersion;
}

void CDisplayColorProfile::Invalidate() {
	::EnterCriticalSection(&GetCS());
	s_bTransformValid = false;
	::LeaveCriticalSection(&GetCS());
}

bool CDisplayColorProfile::ApplyTransform(void* hTransform, const void* pSource, void* pTarget, int nWidth, int nHeight) {
	if (hTransform == NULL || pSource == NULL || pTarget == NULL || nWidth <= 0 || nHeight <= 0) {
		return false;
	}
	__int64 nNumPixels = (__int64)nWidth * nHeight;
	if (nNumPixels > 0xFFFFFFFF) {
		return false; // pixel count does not fit cmsUInt32Number
	}
	cmsDoTransform((cmsHTRANSFORM)hTransform, pSource, pTarget, (cmsUInt32Number)nNumPixels);
	return true;
}

#else

void CDisplayColorProfile::SetWindow(HWND) { }
void* CDisplayColorProfile::GetTransform() { return NULL; }
int CDisplayColorProfile::GetVersion() { return 0; }
void CDisplayColorProfile::Invalidate() { }
bool CDisplayColorProfile::ApplyTransform(void*, const void*, void*, int, int) { return false; }

#endif
