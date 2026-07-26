#pragma once

// Asynchronous check for a newer release on GitHub.
// StartCheck() spawns a short-lived worker thread that queries the GitHub
// releases API and posts nMessage to hWndNotify when done:
//   wParam = 1 if a release newer than the running version exists, 0 otherwise
// The check is fire-and-forget; failures (offline, API error) post nothing.
class CUpdateCheck {
public:
	// Starts the asynchronous check. No-op if a check is already running.
	static void StartCheck(HWND hWndNotify, UINT nMessage);

	// Latest release tag found by the last successful check, e.g. "v1.7".
	// Valid after the notification message has been received.
	static CString GetLatestVersion();

private:
	CUpdateCheck() = delete;

	static unsigned int __stdcall CheckThreadProc(void* pParam);
	static bool FetchLatestTag(CString& sTag);
	static void ParseVersion(LPCTSTR sVersion, int nParts[4]);
	static bool IsNewer(LPCTSTR sRemote, LPCTSTR sLocal);
};
