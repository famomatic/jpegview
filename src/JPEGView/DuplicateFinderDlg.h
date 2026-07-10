#pragma once

#include "resource.h"
#include <vector>
#include <utility>

class CFileList;

// Finds duplicate and near-duplicate images in the current folder using a
// perceptual hash (pHash) computed from a downscaled grayscale version of
// each image. Images with Hamming distance below a threshold are grouped.
class CDuplicateFinderDlg : public CDialogImpl<CDuplicateFinderDlg>
{
public:

	enum { IDD = IDD_DUPLICATE_FINDER };

	BEGIN_MSG_MAP(CDuplicateFinderDlg)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MESSAGE_HANDLER(WM_CLOSE, OnClose)
		COMMAND_ID_HANDLER(IDC_DF_CANCEL, OnCancel)
		COMMAND_ID_HANDLER(IDC_DF_SCAN, OnScan)
		COMMAND_ID_HANDLER(IDC_DF_CLOSE, OnCloseCmd)
		NOTIFY_HANDLER(IDC_DF_LIST, LVN_ITEMCHANGED, OnListViewItemChanged)
	END_MSG_MAP()

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnScan(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnCloseCmd(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnListViewItemChanged(WPARAM wParam, LPNMHDR lpnmhdr, BOOL& bHandled);

	CDuplicateFinderDlg(CFileList& fileList);
	~CDuplicateFinderDlg();

private:
	struct SEntry {
		CString sPath;      // full path
		CString sTitle;     // file name only
		unsigned long long uHash;        // perceptual hash
		int nGroup;          // duplicate group index, -1 if unique
		bool bIsDup;         // true if part of a duplicate group
	};

	CFileList& m_fileList;
	std::vector<SEntry> m_entries;

	CListViewCtrl m_lvFiles;
	CButton m_btnScan;
	CButton m_btnCancel;
	CButton m_btnClose;
	CProgressBarCtrl m_progress;
	CStatic m_lblStatus;

	bool m_bScanning;

	void PopulateList();
	unsigned long long ComputePHash(LPCTSTR sFileName);
	int HammingDistance(unsigned long long a, unsigned long long b);
};