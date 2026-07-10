#pragma once

#include "resource.h"
#include "ColorPalette.h"
#include <vector>

class CJPEGImage;

// Dialog showing the dominant colors of the current image as clickable
// swatches. Clicking a swatch copies its hex code to the clipboard.
class CColorPaletteDlg : public CDialogImpl<CColorPaletteDlg>
{
public:

	enum { IDD = IDD_COLOR_PALETTE };

	BEGIN_MSG_MAP(CColorPaletteDlg)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MESSAGE_HANDLER(WM_CLOSE, OnClose)
		MESSAGE_HANDLER(WM_PAINT, OnPaint)
		MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
		COMMAND_ID_HANDLER(IDC_CP_CLOSE, OnCloseCmd)
		COMMAND_ID_HANDLER(IDC_CP_COPY, OnCopyAll)
	END_MSG_MAP()

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnPaint(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnLButtonDown(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& /*bHandled*/);
	LRESULT OnCloseCmd(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnCopyAll(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);

	CColorPaletteDlg(CJPEGImage* pImage);
	~CColorPaletteDlg();

private:
	CJPEGImage* m_pImage;
	std::vector<CColorPalette::SColor> m_palette;
	CButton m_btnCopy;
	CButton m_btnClose;

	CRect SwatchRect(int nIndex) const;
	void CopyToClipboard(LPCTSTR sText);
};