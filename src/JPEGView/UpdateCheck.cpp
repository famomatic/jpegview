#include "StdAfx.h"
#include "UpdateCheck.h"
#include "resource.h"
#include <winhttp.h>
#include <process.h>
#include <string>

#pragma comment(lib, "winhttp.lib")

static const wchar_t* UPDATE_CHECK_HOST = L"api.github.com";
static const wchar_t* UPDATE_CHECK_PATH = L"/repos/famomatic/jpegview/releases/latest";
static const size_t MAX_RESPONSE_BYTES = 256 * 1024;

namespace {
	struct CheckParam {
		HWND hWndNotify;
		UINT nMessage;
	};
}

static volatile LONG s_nCheckRunning = 0;
static CComAutoCriticalSection s_csLatestVersion;
static CString s_sLatestVersion;
static CComAutoCriticalSection s_csThread;
static HANDLE s_hCheckThread = NULL;
static HWND s_hNotifyWindow = NULL;

void CUpdateCheck::StartCheck(HWND hWndNotify, UINT nMessage) {
	if (!::IsWindow(hWndNotify)) return;
	s_csThread.Lock();
	if (s_hCheckThread != NULL && ::WaitForSingleObject(s_hCheckThread, 0) == WAIT_OBJECT_0) {
		::CloseHandle(s_hCheckThread);
		s_hCheckThread = NULL;
	}
	if (::InterlockedCompareExchange(&s_nCheckRunning, 1, 0) != 0) {
		s_csThread.Unlock();
		return;
	}
	CheckParam* pParam = new CheckParam { hWndNotify, nMessage };
	HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, CheckThreadProc, pParam, 0, NULL);
	if (hThread == NULL) {
		delete pParam;
		::InterlockedExchange(&s_nCheckRunning, 0);
		s_csThread.Unlock();
		return;
	}
	s_hNotifyWindow = hWndNotify;
	s_hCheckThread = hThread;
	s_csThread.Unlock();
}

void CUpdateCheck::Shutdown(HWND hWndNotify) {
	HANDLE thread = NULL;
	s_csThread.Lock();
	if (s_hNotifyWindow == hWndNotify) s_hNotifyWindow = NULL;
	thread = s_hCheckThread;
	s_csThread.Unlock();
	if (thread != NULL) ::WaitForSingleObject(thread, INFINITE);
	s_csThread.Lock();
	if (s_hCheckThread == thread && thread != NULL) {
		::CloseHandle(s_hCheckThread);
		s_hCheckThread = NULL;
	}
	s_csThread.Unlock();
}

CString CUpdateCheck::GetLatestVersion() {
	s_csLatestVersion.Lock();
	CString sVersion = s_sLatestVersion;
	s_csLatestVersion.Unlock();
	return sVersion;
}

unsigned int __stdcall CUpdateCheck::CheckThreadProc(void* pParam) {
	CheckParam* p = (CheckParam*)pParam;
	CString sTag;
	if (FetchLatestTag(sTag)) {
		s_csLatestVersion.Lock();
		s_sLatestVersion = sTag;
		s_csLatestVersion.Unlock();
		bool bNewer = IsNewer(sTag, CString(JPEGVIEW_VERSION));
		s_csThread.Lock();
		const bool canNotify = s_hNotifyWindow == p->hWndNotify && ::IsWindow(p->hWndNotify);
		if (canNotify) ::PostMessage(p->hWndNotify, p->nMessage, bNewer ? 1 : 0, 0);
		s_csThread.Unlock();
	}
	delete p;
	::InterlockedExchange(&s_nCheckRunning, 0);
	return 0;
}

bool CUpdateCheck::FetchLatestTag(CString& sTag) {
	bool bSuccess = false;
	std::string body;

	HINTERNET hSession = ::WinHttpOpen(L"JPEGView", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (hSession == NULL) {
		hSession = ::WinHttpOpen(L"JPEGView", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	}
	if (hSession == NULL) {
		return false;
	}
	::WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);
	HINTERNET hConnect = ::WinHttpConnect(hSession, UPDATE_CHECK_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
	if (hConnect != NULL) {
		HINTERNET hRequest = ::WinHttpOpenRequest(hConnect, L"GET", UPDATE_CHECK_PATH,
			NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
		if (hRequest != NULL) {
			if (::WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
					WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
				::WinHttpReceiveResponse(hRequest, NULL)) {
				DWORD nStatus = 0;
				DWORD nStatusSize = sizeof(nStatus);
				::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					WINHTTP_HEADER_NAME_BY_INDEX, &nStatus, &nStatusSize, WINHTTP_NO_HEADER_INDEX);
				if (nStatus == 200) {
					bool complete = true;
					DWORD nAvailable = 0;
					while (::WinHttpQueryDataAvailable(hRequest, &nAvailable) && nAvailable > 0) {
						if (body.size() + nAvailable > MAX_RESPONSE_BYTES) {
							complete = false;
							break;
						}
						size_t nOffset = body.size();
						body.resize(nOffset + nAvailable);
						DWORD nRead = 0;
						if (!::WinHttpReadData(hRequest, &body[nOffset], nAvailable, &nRead)) {
							complete = false;
							break;
						}
						body.resize(nOffset + nRead);
						if (nRead == 0) {
							break;
						}
					}
					bSuccess = complete && !body.empty();
				}
			}
			::WinHttpCloseHandle(hRequest);
		}
		::WinHttpCloseHandle(hConnect);
	}
	::WinHttpCloseHandle(hSession);

	if (!bSuccess) {
		return false;
	}

	// Minimal extraction of "tag_name": "<value>" from the JSON response
	size_t nKey = body.find("\"tag_name\"");
	if (nKey == std::string::npos) {
		return false;
	}
	size_t nColon = body.find(':', nKey);
	if (nColon == std::string::npos) {
		return false;
	}
	size_t nOpen = body.find('"', nColon);
	if (nOpen == std::string::npos) {
		return false;
	}
	size_t nClose = body.find('"', nOpen + 1);
	if (nClose == std::string::npos || nClose - nOpen - 1 == 0 || nClose - nOpen - 1 > 64) {
		return false;
	}
	std::string tag = body.substr(nOpen + 1, nClose - nOpen - 1);
	sTag = CString(tag.c_str());
	return true;
}

// Parses up to four dot-separated numeric parts, ignoring any non-digit
// prefix (so "v1.6.1" and "1.6.1" both yield 1,6,1,0).
void CUpdateCheck::ParseVersion(LPCTSTR sVersion, int nParts[4]) {
	for (int i = 0; i < 4; i++) {
		nParts[i] = 0;
	}
	LPCTSTR p = sVersion;
	while (*p != 0 && (*p < _T('0') || *p > _T('9'))) {
		p++;
	}
	for (int i = 0; i < 4 && *p != 0; i++) {
		while (*p >= _T('0') && *p <= _T('9')) {
			const int digit = *p - _T('0');
			if (nParts[i] > (INT_MAX - digit) / 10) nParts[i] = INT_MAX;
			else nParts[i] = nParts[i] * 10 + digit;
			p++;
		}
		if (*p != _T('.')) {
			break;
		}
		p++;
	}
}

bool CUpdateCheck::IsNewer(LPCTSTR sRemote, LPCTSTR sLocal) {
	int nRemote[4], nLocal[4];
	ParseVersion(sRemote, nRemote);
	ParseVersion(sLocal, nLocal);
	for (int i = 0; i < 4; i++) {
		if (nRemote[i] != nLocal[i]) {
			return nRemote[i] > nLocal[i];
		}
	}
	return false;
}
