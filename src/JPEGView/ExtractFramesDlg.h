#pragma once

#include "resource.h"

class CJPEGImage;
class CFileList;

// Dialog to extract all frames of an animated image (animated PNG, WebP,
// JPEG XL, AVIF, GIF) to individual PNG files in a user-chosen folder.
class CExtractFramesDlg : public CDialogImpl<CExtractFramesDlg>
{
public:

	enum { IDD = IDD_EXTRACT_FRAMES };

	BEGIN_MSG_MAP(CExtractFramesDlg)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MESSAGE_HANDLER(WM_CLOSE, OnClose)
		COMMAND_ID_HANDLER(IDC_EF_CANCEL, OnCancel)
		COMMAND_ID_HANDLER(IDC_EF_BROWSE, OnBrowse)
		COMMAND_ID_HANDLER(IDC_EF_EXTRACT, OnExtract)
	END_MSG_MAP()

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnBrowse(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnExtract(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);

	CExtractFramesDlg(LPCTSTR sFileName, int nFrameCount);
	~CExtractFramesDlg();

private:
	CString m_sFileName;
	int m_nFrameCount;

	CEdit m_edtDest;
	CButton m_btnBrowse;
	CButton m_btnExtract;
	CButton m_btnCancel;
	CProgressBarCtrl m_progress;
	CStatic m_lblStatus;
	CComboBox m_cbFormat;

	CString m_sDestFolder;

	bool ExtractFrame(int nFrameIndex, LPCTSTR sDestFolder, LPCTSTR sBaseName, LPCTSTR sExt);
};