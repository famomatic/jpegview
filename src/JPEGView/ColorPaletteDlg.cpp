#include "stdafx.h"
#include "ColorPaletteDlg.h"
#include "JPEGImage.h"

CColorPaletteDlg::CColorPaletteDlg(CJPEGImage* pImage)
	: m_pImage(pImage)
{
}

CColorPaletteDlg::~CColorPaletteDlg()
{
}

LRESULT CColorPaletteDlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
	CenterWindow(GetParent());

	m_btnCopy.Attach(GetDlgItem(IDC_CP_COPY));
	m_btnClose.Attach(GetDlgItem(IDC_CP_CLOSE));

	if (m_pImage != NULL) {
		void* pPixels = m_pImage->OriginalPixels();
		int nWidth = m_pImage->OrigWidth();
		int nHeight = m_pImage->OrigHeight();
		int nChannels = m_pImage->OriginalChannels();

		if (pPixels != NULL && nWidth > 0 && nHeight > 0) {
			// OriginalPixels is 3 or 4 channel. If 3-channel (BGR), we need
			// to synthesize alpha=0xFF for the extractor, but the extractor
			// only reads a for the skip-transparent test, so we can pass a
			// stride hint by converting to 4-channel when needed.
			if (nChannels == 4) {
				m_palette = CColorPalette::Extract(pPixels, nWidth, nHeight, 6);
			} else {
				// 3-channel BGR: build a temporary BGRA buffer.
				int nTotal = nWidth * nHeight;
				UINT8* pBGRA = new UINT8[nTotal * 4];
				const UINT8* pSrc = (const UINT8*)pPixels;
				for (int i = 0; i < nTotal; i++) {
					pBGRA[i * 4 + 0] = pSrc[i * 3 + 0];
					pBGRA[i * 4 + 1] = pSrc[i * 3 + 1];
					pBGRA[i * 4 + 2] = pSrc[i * 3 + 2];
					pBGRA[i * 4 + 3] = 0xFF;
				}
				m_palette = CColorPalette::Extract(pBGRA, nWidth, nHeight, 6);
				delete[] pBGRA;
			}
		}
	}

	if (m_palette.empty()) {
		::SetWindowText(GetDlgItem(IDC_CP_COPY), _T("No colors"));
		::EnableWindow(GetDlgItem(IDC_CP_COPY), FALSE);
	}

	return TRUE;
}

LRESULT CColorPaletteDlg::OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
	EndDialog(IDCANCEL);
	return 0;
}

LRESULT CColorPaletteDlg::OnCloseCmd(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	EndDialog(wID);
	return 0;
}

CRect CColorPaletteDlg::SwatchRect(int nIndex) const {
	// Swatches laid out in a single row, each 48x48 px, starting at (12, 12).
	const int nSwatchSize = 48;
	const int nSpacing = 8;
	const int nStartX = 12;
	const int nStartY = 18;
	CRect rc;
	rc.left = nStartX + nIndex * (nSwatchSize + nSpacing);
	rc.top = nStartY;
	rc.right = rc.left + nSwatchSize;
	rc.bottom = rc.top + nSwatchSize;
	return rc;
}

LRESULT CColorPaletteDlg::OnPaint(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
	CPaintDC dc(m_hWnd);

	for (int i = 0; i < (int)m_palette.size(); i++) {
		const CColorPalette::SColor& c = m_palette[i];
		CRect rc = SwatchRect(i);
		COLORREF cr = RGB(c.r, c.g, c.b);
		CBrush brush;
		brush.CreateSolidBrush(cr);
		dc.FillRect(rc, brush);
		// Draw a thin border around the swatch.
		CPen pen;
		pen.CreatePen(PS_SOLID, 1, RGB(128, 128, 128));
		CPenHandle oldPen = dc.SelectPen(pen);
		CBrushHandle oldBrush = dc.SelectBrush(brush);
		dc.Rectangle(rc);
		dc.SelectPen(oldPen);
		dc.SelectBrush(oldBrush);

		// Draw the hex label below the swatch.
		CString sHex = CColorPalette::ToHex(c);
		dc.SetBkMode(TRANSPARENT);
		dc.DrawText(sHex, sHex.GetLength(), CRect(rc.left - 4, rc.bottom + 2, rc.right + 4, rc.bottom + 16), DT_CENTER | DT_SINGLELINE);
	}

	return 0;
}

LRESULT CColorPaletteDlg::OnLButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/) {
	int nX = (int)(short)LOWORD(lParam);
	int nY = (int)(short)HIWORD(lParam);

	for (int i = 0; i < (int)m_palette.size(); i++) {
		CRect rc = SwatchRect(i);
		if (rc.PtInRect(CPoint(nX, nY))) {
			CString sHex = CColorPalette::ToHex(m_palette[i]);
			CopyToClipboard(sHex);
			// Brief visual feedback in the title bar.
			CString sTitle;
			sTitle.Format(_T("Copied %s"), sHex.GetString());
			::SetWindowText(m_hWnd, sTitle);
			break;
		}
	}
	return 0;
}

void CColorPaletteDlg::CopyToClipboard(LPCTSTR sText) {
	if (!::OpenClipboard(m_hWnd)) return;
	::EmptyClipboard();
#ifdef _UNICODE
	// Copy as Unicode text.
	size_t nLen = _tcslen(sText);
	HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, (nLen + 1) * sizeof(TCHAR));
	if (hMem) {
		TCHAR* pMem = (TCHAR*)::GlobalLock(hMem);
		if (pMem) {
			_tcscpy_s(pMem, nLen + 1, sText);
			::GlobalUnlock(hMem);
			::SetClipboardData(CF_UNICODETEXT, hMem);
		}
	}
#else
	HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, strlen(sText) + 1);
	if (hMem) {
		char* pMem = (char*)::GlobalLock(hMem);
		if (pMem) {
			strcpy_s(pMem, strlen(sText) + 1, sText);
			::GlobalUnlock(hMem);
			::SetClipboardData(CF_TEXT, hMem);
		}
	}
#endif
	::CloseClipboard();
}

LRESULT CColorPaletteDlg::OnCopyAll(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	// Copy all hex codes, newline-separated, to the clipboard.
	CString sAll;
	for (int i = 0; i < (int)m_palette.size(); i++) {
		if (i > 0) sAll += _T("\r\n");
		sAll += CColorPalette::ToHex(m_palette[i]);
	}
	if (!sAll.IsEmpty()) CopyToClipboard(sAll);
	return 0;
}