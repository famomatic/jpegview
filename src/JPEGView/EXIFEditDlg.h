#pragma once

#include "resource.h"

class CJPEGImage;

// Dialog to edit EXIF metadata: date/time, GPS removal, orientation.
// Works on JPEG files that have an EXIF APP1 block.
class CEXIFEditDlg : public CDialogImpl<CEXIFEditDlg>
{
public:

	enum { IDD = IDD_EXIFEDIT };

	BEGIN_MSG_MAP(CEXIFEditDlg)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MESSAGE_HANDLER(WM_CLOSE, OnClose)
		COMMAND_ID_HANDLER(IDC_EE_CANCEL, OnCancel)
		COMMAND_ID_HANDLER(IDC_EE_APPLY, OnApply)
	END_MSG_MAP()

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnApply(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);

	CEXIFEditDlg(LPCTSTR sFileName);
	~CEXIFEditDlg();

private:
	CString m_sFileName;

	CButton m_btnGPS;
	CComboBox m_cbOrientation;
	CButton m_btnApply;
	CButton m_btnCancel;

	bool m_bHasEXIF;
	bool m_bHasGPS;
	int m_nOrientation;
};