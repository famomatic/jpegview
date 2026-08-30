#include "stdafx.h"
#include "BatchRenameDlg.h"
#include "FileList.h"
#include "Helpers.h"
#include "HelpersGUI.h"
#include "EXIFReader.h"
#include "JPEGImage.h"
#include <list>
#include <vector>
#include <set>

namespace {
struct CStringNoCaseLess {
	bool operator()(const CString& left, const CString& right) const { return left.CompareNoCase(right) < 0; }
};

bool IsValidFileName(const CString& name, CString& reason) {
	if (name.IsEmpty() || name == _T(".") || name == _T("..")) {
		reason = _T("The generated file name is empty or reserved.");
		return false;
	}
	if (name.GetLength() > 255 || name[name.GetLength() - 1] == _T('.') || name[name.GetLength() - 1] == _T(' ')) {
		reason = _T("File names cannot exceed 255 characters or end in a dot/space.");
		return false;
	}
	for (int i = 0; i < name.GetLength(); i++) {
		const TCHAR ch = name[i];
		if (ch < 32 || _tcschr(_T("<>:\"/\\|?*"), ch) != NULL) {
			reason = _T("The generated name contains a path separator or an invalid character.");
			return false;
		}
	}
	CString base = name;
	const int dot = base.Find(_T('.'));
	if (dot >= 0) base = base.Left(dot);
	base.MakeUpper();
	if (base == _T("CON") || base == _T("PRN") || base == _T("AUX") || base == _T("NUL") ||
		(base.GetLength() == 4 && (base.Left(3) == _T("COM") || base.Left(3) == _T("LPT")) &&
			base[3] >= _T('1') && base[3] <= _T('9'))) {
		reason = _T("The generated name uses a reserved Windows device name.");
		return false;
	}
	return true;
}

CString JoinPath(const CString& directory, const CString& name) {
	CString path = directory;
	if (!path.IsEmpty() && path[path.GetLength() - 1] != _T('\\') && path[path.GetLength() - 1] != _T('/'))
		path += _T("\\");
	path += name;
	return path;
}
}

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
	m_btnRename.EnableWindow(FALSE);
	m_btnCancel.EnableWindow(FALSE);

	CString sDir = m_fileList.CurrentDirectory();
	struct RenameJob {
		CString oldPath;
		CString newPath;
		CString tempPath;
		bool changes;
	};
	std::vector<RenameJob> jobs;
	std::set<CString, CStringNoCaseLess> targets;
	CString error;

	std::list<CFileDesc>& files = m_fileList.GetFileList();
	int nItem = 0;
	for (std::list<CFileDesc>::iterator it = files.begin(); it != files.end(); ++it, nItem++) {
		if (!m_lvFiles.GetCheckState(nItem)) continue;
		CString sNewName = ExpandPattern(it->GetName(), *it, nItem);
		if (!IsValidFileName(sNewName, error)) break;
		RenameJob job;
		job.oldPath = it->GetName();
		job.newPath = JoinPath(sDir, sNewName);
		job.changes = job.oldPath.Compare(job.newPath) != 0;
		if (!targets.insert(job.newPath).second) {
			error.Format(_T("More than one selected file would be named:\n%s"), sNewName.GetString());
			break;
		}
		jobs.push_back(job);
	}
	if (error.IsEmpty() && jobs.empty()) error = _T("No files are selected.");

	std::set<CString, CStringNoCaseLess> vacatedSources;
	for (const RenameJob& job : jobs) if (job.changes) vacatedSources.insert(job.oldPath);
	if (error.IsEmpty()) {
		for (const RenameJob& job : jobs) {
			if (!job.changes) continue;
			if (::GetFileAttributes(job.oldPath) == INVALID_FILE_ATTRIBUTES) {
				error.Format(_T("The source file no longer exists:\n%s"), job.oldPath.GetString());
				break;
			}
			if (::GetFileAttributes(job.newPath) != INVALID_FILE_ATTRIBUTES && vacatedSources.find(job.newPath) == vacatedSources.end()) {
				error.Format(_T("A target file already exists and is not being renamed:\n%s"), job.newPath.GetString());
				break;
			}
		}
	}
	if (!error.IsEmpty()) {
		m_bRenaming = false;
		m_btnRename.EnableWindow(TRUE);
		m_btnCancel.EnableWindow(TRUE);
		::MessageBox(m_hWnd, error, _T("Batch Rename"), MB_OK | MB_ICONWARNING);
		return 0;
	}

	static volatile LONG tempSequence = 0;
	for (RenameJob& job : jobs) {
		if (!job.changes) continue;
		do {
			CString tempName;
			tempName.Format(_T(".jpegview-rename-%lu-%ld.tmp"), ::GetCurrentProcessId(), ::InterlockedIncrement(&tempSequence));
			job.tempPath = JoinPath(sDir, tempName);
		} while (::GetFileAttributes(job.tempPath) != INVALID_FILE_ATTRIBUTES);
	}

	std::vector<size_t> staged;
	for (size_t i = 0; i < jobs.size(); i++) {
		if (!jobs[i].changes) continue;
		if (!::MoveFileEx(jobs[i].oldPath, jobs[i].tempPath, MOVEFILE_WRITE_THROUGH)) {
			error.Format(_T("Failed to stage a rename (error %lu):\n%s"), ::GetLastError(), jobs[i].oldPath.GetString());
			break;
		}
		staged.push_back(i);
	}
	if (!error.IsEmpty()) {
		for (std::vector<size_t>::reverse_iterator it = staged.rbegin(); it != staged.rend(); ++it)
			::MoveFileEx(jobs[*it].tempPath, jobs[*it].oldPath, MOVEFILE_WRITE_THROUGH);
	} else {
		std::vector<size_t> committed;
		for (size_t index : staged) {
			if (!::MoveFileEx(jobs[index].tempPath, jobs[index].newPath, MOVEFILE_WRITE_THROUGH)) {
				error.Format(_T("Failed to commit a rename (error %lu):\n%s"), ::GetLastError(), jobs[index].newPath.GetString());
				break;
			}
			committed.push_back(index);
		}
		if (!error.IsEmpty()) {
			bool rollbackOK = true;
			for (size_t index : committed)
				if (!::MoveFileEx(jobs[index].newPath, jobs[index].tempPath, MOVEFILE_WRITE_THROUGH)) rollbackOK = false;
			for (std::vector<size_t>::reverse_iterator it = staged.rbegin(); it != staged.rend(); ++it)
				if (::GetFileAttributes(jobs[*it].tempPath) != INVALID_FILE_ATTRIBUTES &&
					!::MoveFileEx(jobs[*it].tempPath, jobs[*it].oldPath, MOVEFILE_WRITE_THROUGH)) rollbackOK = false;
			if (!rollbackOK) error += _T("\nAutomatic rollback was incomplete; some .jpegview-rename temporary files may remain.");
		}
	}

	m_bRenaming = false;
	m_btnRename.EnableWindow(TRUE);
	m_btnCancel.EnableWindow(TRUE);
	if (!error.IsEmpty()) {
		::MessageBox(m_hWnd, error, _T("Batch Rename"), MB_OK | MB_ICONERROR);
		return 0;
	}

	CString sMsg;
	int nRenamed = 0;
	for (const RenameJob& job : jobs) if (job.changes) nRenamed++;
	sMsg.Format(_T("Renamed %d file(s)."), nRenamed);
	::MessageBox(m_hWnd, sMsg, _T("Batch Rename"), MB_OK | MB_ICONINFORMATION);

	EndDialog(wID);
	return 0;
}
