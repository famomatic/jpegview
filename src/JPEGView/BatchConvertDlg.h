#pragma once

#include "resource.h"

class CFileList;

// Dialog to batch-convert images in a folder to a different format.
// Modeled after Tichau/FileConverter: select files, pick target format/quality,
// optionally resize, and convert all in one pass with a progress bar.
class CBatchConvertDlg : public CDialogImpl<CBatchConvertDlg>
{
public:

	enum { IDD = IDD_BATCHCONVERT };

	BEGIN_MSG_MAP(CBatchConvertDlg)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MESSAGE_HANDLER(WM_CLOSE, OnClose)
		COMMAND_ID_HANDLER(IDC_BC_CANCEL, OnCancel)
		COMMAND_ID_HANDLER(IDC_BC_CONVERT, OnConvert)
		COMMAND_ID_HANDLER(IDC_BC_SELECTALL, OnSelectAll)
		COMMAND_ID_HANDLER(IDC_BC_SELECTNONE, OnSelectNone)
		COMMAND_ID_HANDLER(IDC_BC_BROWSE, OnBrowse)
		COMMAND_HANDLER(IDC_BC_FORMAT, CBN_SELCHANGE, OnFormatChanged)
		NOTIFY_HANDLER(IDC_BC_LIST, LVN_ITEMCHANGED, OnListViewItemChanged)
	END_MSG_MAP()

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnConvert(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnSelectAll(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnSelectNone(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnBrowse(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnFormatChanged(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnListViewItemChanged(WPARAM wParam, LPNMHDR lpnmhdr, BOOL& bHandled);

	CBatchConvertDlg(CFileList& fileList);
	~CBatchConvertDlg();

private:
	CFileList& m_fileList;

	CListViewCtrl m_lvFiles;
	CComboBox m_cbFormat;
	CEdit m_edtQuality;
	CEdit m_edtDest;
	CButton m_btnBrowse;
	CButton m_btnConvert;
	CButton m_btnCancel;
	CButton m_btnSelectAll;
	CButton m_btnSelectNone;
	CButton m_btnKeepOrig;
	CButton m_btnResize;
	CEdit m_edtMaxWidth;
	CEdit m_edtMaxHeight;
	CProgressBarCtrl m_progress;
	CStatic m_lblStatus;

	int m_nNumFiles;
	bool m_bConverting;

	int CreateItemList();
	CString GetOutputPath(LPCTSTR sInputPath, LPCTSTR sInputFile, LPCTSTR sExt, bool bKeepOriginal);
	bool ConvertSingle(LPCTSTR sInputFile, LPCTSTR sOutputFile, int nFormat, int nQuality, int nMaxWidth, int nMaxHeight);
	void UpdateQualityEnabled();
};