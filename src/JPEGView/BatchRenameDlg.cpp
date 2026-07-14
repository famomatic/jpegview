#include "stdafx.h"
#include "BatchRenameDlg.h"
#include "FileList.h"
#include "Helpers.h"
#include "HelpersGUI.h"
#include "EXIFReader.h"
#include "JPEGImage.h"
#include <list>

CBatchRenameDlg::CBatchRenameDlg(CFileList& fileList)
	: m_fileList(fileList)
	, m_nStartNum(1)
	, m_bRenaming(false)
{
	m_sPattern = _T("{name}_{num}");
}

CBatchRenameDlg::~CBatchRenameDlg()
{
}

LRESULT CBatchRenameDlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
	CenterWindow(GetParent());

	m_lvFiles.Attach(GetDlgItem(IDC_BR_LIST));
	m_edtPattern.Attach(GetDlgItem(IDC_BR_PATTERN));
	m_lblPreview.Attach(GetDlgItem(IDC_BR_PREVIEW));
	m_btnRename.Attach(GetDlgItem(IDC_BR_RENAME));
	m_btnCancel.Attach(GetDlgItem(IDC_BR_CANCEL));
	m_edtStartNum.Attach(GetDlgItem(IDC_BR_STARTNUM));

	m_lvFiles.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);
	m_lvFiles.InsertColumn(0, _T("Original"), LVCFMT_LEFT, 180);
	m_lvFiles.InsertColumn(1, _T("New name"), LVCFMT_LEFT, 180);

	m_edtPattern.SetWindowText(m_sPattern);
	m_edtStartNum.SetWindowText(_T("1"));

	PopulateList();
	UpdatePreview();

	return TRUE;
}

LRESULT CBatchRenameDlg::OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
	EndDialog(IDCANCEL);
	return 0;
}

LRESULT CBatchRenameDlg::OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	EndDialog(wID);
	return 0;
}

void CBatchRenameDlg::PopulateList() {
	m_lvFiles.DeleteAllItems();
	std::list<CFileDesc>& files = m_fileList.GetFileList();
	int nItem = 0;
	for (std::list<CFileDesc>::iterator it = files.begin(); it != files.end(); ++it) {
		LPCTSTR sTitle = it->GetTitle();
		m_lvFiles.InsertItem(nItem, sTitle);
		m_lvFiles.SetCheckState(nItem, TRUE);
		nItem++;
	}
}

CString CBatchRenameDlg::ExpandPattern(LPCTSTR sOriginalFile, const CFileDesc& fileDesc, int nIndex) {
	CString sResult = m_sPattern;

	// Extract name and extension from the original file.
	CString sFile(sOriginalFile);
	CString sName, sExt;
	int nDot = sFile.ReverseFind(_T('.'));
	int nSlash = sFile.ReverseFind(_T('\\'));
	if (nSlash < 0) nSlash = sFile.ReverseFind(_T('/'));
	if (nDot > nSlash) {
		sName = sFile.Mid(nSlash + 1, nDot - nSlash - 1);
		sExt = sFile.Mid(nDot); // includes the dot
	} else {
		sName = (nSlash >= 0) ? sFile.Mid(nSlash + 1) : sFile;
		sExt = _T("");
	}

	// Sequence number, zero-padded to 3 digits (or wider if needed).
	int nTotal = m_fileList.Size();
	int nPadWidth = 3;
	int nTemp = nTotal;
	while (nTemp >= 1000) { nPadWidth++; nTemp /= 10; }
	int nNum = m_nStartNum + nIndex;
	CString sNum;
	sNum.Format(_T("%0*d"), nPadWidth, nNum);

	// Date/time from EXIF if available, else file modification time.
	CString sDate, sTime;
	// Try reading EXIF date by loading the file and extracting the APP1 block.
	// Only JPEG files have EXIF APP1 markers we can scan cheaply.
	CString sLower = sOriginalFile;
	sLower.MakeLower();
	if (sLower.Right(4) == _T(".jpg") || sLower.Right(5) == _T(".jpeg")) {
		HANDLE hFile = ::CreateFile(sOriginalFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile != INVALID_HANDLE_VALUE) {
			DWORD nSize = ::GetFileSize(hFile, NULL);
			// Read at most the first 256 KB — EXIF APP1 is near the file start.
			DWORD nToRead = (nSize < 256 * 1024) ? nSize : (256 * 1024);
			uint8* pBuffer = new uint8[nToRead];
			DWORD nRead = 0;
			if (::ReadFile(hFile, pBuffer, nToRead, &nRead, NULL) && nRead > 0) {
				void* pEXIFBlock = Helpers::FindEXIFBlock(pBuffer, (int)nRead);
				if (pEXIFBlock != NULL) {
					CEXIFReader reader(pEXIFBlock, IF_JPEG);
					if (reader.GetAcquisitionTimePresent()) {
						const SYSTEMTIME& st = reader.GetAcquisitionTime();
						sDate.Format(_T("%04d%02d%02d"), st.wYear, st.wMonth, st.wDay);
						sTime.Format(_T("%02d%02d%02d"), st.wHour, st.wMinute, st.wSecond);
					}
				}
			}
			delete[] pBuffer;
			::CloseHandle(hFile);
		}
	}
	if (sDate.IsEmpty()) {
		// Fall back to file modification time from the file descriptor.
		const FILETIME& ft = fileDesc.GetLastModTime();
		FILETIME ftLocal;
		SYSTEMTIME st;
		::FileTimeToLocalFileTime(&ft, &ftLocal);
		::FileTimeToSystemTime(&ftLocal, &st);
		sDate.Format(_T("%04d%02d%02d"), st.wYear, st.wMonth, st.wDay);
		sTime.Format(_T("%02d%02d%02d"), st.wHour, st.wMinute, st.wSecond);
	}

	// Remember whether the user placed the extension explicitly before the
	// {ext} token is substituted away.
	bool bHasExtToken = m_sPattern.Find(_T("{ext}")) >= 0;

	sResult.Replace(_T("{name}"), sName);
	sResult.Replace(_T("{ext}"), sExt);
	sResult.Replace(_T("{num}"), sNum);
	sResult.Replace(_T("{date}"), sDate);
	sResult.Replace(_T("{time}"), sTime);

	// Never drop the extension: if the pattern has no {ext} token, append the
	// original extension so renamed files remain openable.
	if (!bHasExtToken) {
		sResult += sExt;
	}

	return sResult;
}

void CBatchRenameDlg::UpdatePreview() {
	// Read pattern and start number from the controls.
	m_edtPattern.GetWindowText(m_sPattern);
	CString sStartNum;
	m_edtStartNum.GetWindowText(sStartNum);
	m_nStartNum = _ttoi(sStartNum);
	if (m_nStartNum < 0) m_nStartNum = 0;

	// Show a preview for the first checked file.
	std::list<CFileDesc>& files = m_fileList.GetFileList();
	int nItem = 0;
	for (std::list<CFileDesc>::iterator it = files.begin(); it != files.end(); ++it, nItem++) {
		if (m_lvFiles.GetCheckState(nItem)) {
			CString sNew = ExpandPattern(it->GetName(), *it, nItem);
			CString sPreview;
			sPreview.Format(_T("Preview: %s -> %s"), it->GetTitle(), sNew.GetString());
			m_lblPreview.SetWindowText(sPreview);
			return;
		}
	}
	m_lblPreview.SetWindowText(_T("No files selected"));
}

LRESULT CBatchRenameDlg::OnPatternChanged(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	UpdatePreview();
	return 0;
}

LRESULT CBatchRenameDlg::OnRename(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if (m_bRenaming) return 0;
	m_bRenaming = true;

	CString sDir = m_fileList.CurrentDirectory();

	std::list<CFileDesc>& files = m_fileList.GetFileList();
	int nItem = 0;
	int nRenamed = 0;
	int nSkipped = 0;

	for (std::list<CFileDesc>::iterator it = files.begin(); it != files.end(); ++it, nItem++) {
		if (!m_lvFiles.GetCheckState(nItem)) continue;

		CString sOldPath = it->GetName();
		CString sNewName = ExpandPattern(sOldPath, *it, nItem);

		// Build the full new path.
		CString sNewPath = sDir;
		if (!sDir.IsEmpty() && sDir[sDir.GetLength() - 1] != _T('\\') && sDir[sDir.GetLength() - 1] != _T('/'))
			sNewPath += _T("\\");
		sNewPath += sNewName;

		// Skip if the target is the same as the source.
		if (sOldPath.CompareNoCase(sNewPath) == 0) { nSkipped++; continue; }

		// Skip if the target already exists (avoid clobbering).
		if (::GetFileAttributes(sNewPath) != INVALID_FILE_ATTRIBUTES) { nSkipped++; continue; }

		if (::MoveFile(sOldPath, sNewPath)) {
			nRenamed++;
		} else {
			nSkipped++;
		}
	}

	m_bRenaming = false;

	CString sMsg;
	sMsg.Format(_T("Renamed %d file(s).\nSkipped %d file(s)."), nRenamed, nSkipped);
	::MessageBox(m_hWnd, sMsg, _T("Batch Rename"), MB_OK | MB_ICONINFORMATION);

	EndDialog(wID);
	return 0;
}