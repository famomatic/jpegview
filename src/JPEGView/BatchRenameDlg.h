#pragma once

#include "resource.h"
#include "FileList.h"

class CFileList;

// Dialog to batch-rename image files in a folder using a pattern.
// Supported tokens in the pattern:
//   {name}    - original file name (without extension)
//   {ext}     - original extension
//   {num}     - sequence number, zero-padded to width
//   {date}    - EXIF date (yyyyMMdd) or file mod date if no EXIF
//   {time}    - EXIF time (HHmmss) or file mod time if no EXIF
class CBatchRenameDlg : public CDialogImpl<CBatchRenameDlg>
{
public:

	enum { IDD = IDD_BATCH_RENAME };

	BEGIN_MSG_MAP(CBatchRenameDlg)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MESSAGE_HANDLER(WM_CLOSE, OnClose)
		COMMAND_ID_HANDLER(IDC_BR_CANCEL, OnCancel)
		COMMAND_ID_HANDLER(IDC_BR_RENAME, OnRename)
		COMMAND_HANDLER(IDC_BR_PATTERN, EN_CHANGE, OnPatternChanged)
	END_MSG_MAP()

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnRename(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnPatternChanged(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);

	CBatchRenameDlg(CFileList& fileList);
	~CBatchRenameDlg();

private:
	CFileList& m_fileList;

	CListViewCtrl m_lvFiles;
	CEdit m_edtPattern;
	CStatic m_lblPreview;
	CButton m_btnRename;
	CButton m_btnCancel;
	CEdit m_edtStartNum;

	CString m_sPattern;
	int m_nStartNum;
	bool m_bRenaming;

	void PopulateList();
	CString ExpandPattern(LPCTSTR sOriginalFile, const CFileDesc& fileDesc, int nIndex);
	void UpdatePreview();
};