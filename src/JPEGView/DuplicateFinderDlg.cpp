#include "stdafx.h"
#include "DuplicateFinderDlg.h"
#include "FileList.h"
#include "Helpers.h"
#include "HelpersGUI.h"
#include <gdiplus.h>
#include <list>
#include <algorithm>

CDuplicateFinderDlg::CDuplicateFinderDlg(CFileList& fileList)
	: m_fileList(fileList)
	, m_bScanning(false)
{
}

CDuplicateFinderDlg::~CDuplicateFinderDlg()
{
}

LRESULT CDuplicateFinderDlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
	CenterWindow(GetParent());

	m_lvFiles.Attach(GetDlgItem(IDC_DF_LIST));
	m_btnScan.Attach(GetDlgItem(IDC_DF_SCAN));
	m_btnCancel.Attach(GetDlgItem(IDC_DF_CANCEL));
	m_btnClose.Attach(GetDlgItem(IDC_DF_CLOSE));
	m_progress.Attach(GetDlgItem(IDC_DF_PROGRESS));
	m_lblStatus.Attach(GetDlgItem(IDC_DF_STATUS));

	m_lvFiles.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);
	m_lvFiles.InsertColumn(0, _T("File"), LVCFMT_LEFT, 200);
	m_lvFiles.InsertColumn(1, _T("Status"), LVCFMT_LEFT, 100);
	m_lvFiles.InsertColumn(2, _T("Group"), LVCFMT_LEFT, 60);

	m_lblStatus.SetWindowText(_T("Click Scan to find duplicates."));
	return TRUE;
}

LRESULT CDuplicateFinderDlg::OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
	EndDialog(IDCANCEL);
	return 0;
}

LRESULT CDuplicateFinderDlg::OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	EndDialog(wID);
	return 0;
}

LRESULT CDuplicateFinderDlg::OnCloseCmd(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	EndDialog(wID);
	return 0;
}

// Compute a 64-bit perceptual hash (pHash) from an image file.
// The image is loaded at thumbnail size via GDI+, converted to 8x8 grayscale,
// and each bit is set based on whether the pixel is above the mean.
// GDI+ handles JPEG, PNG, BMP, GIF, TIFF — the most common formats. Files in
// exotic formats simply get hash 0 and are treated as unique.
unsigned long long CDuplicateFinderDlg::ComputePHash(LPCTSTR sFileName) {
	unsigned long long uHash = 0;

	Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromFile(sFileName);
	if (pBitmap == NULL) return 0;
	if (pBitmap->GetLastStatus() != Gdiplus::Ok) { delete pBitmap; return 0; }

	int nW = pBitmap->GetWidth();
	int nH = pBitmap->GetHeight();
	if (nW <= 0 || nH <= 0) { delete pBitmap; return 0; }

	// Downscale to 8x8 using a GDI+ Graphics context.
	Gdiplus::Bitmap thumb(8, 8, PixelFormat32bppARGB);
	Gdiplus::Graphics g(&thumb);
	g.SetInterpolationMode(Gdiplus::InterpolationModeLowQuality);
	g.DrawImage(pBitmap, 0, 0, 8, 8);
	delete pBitmap;

	// Extract grayscale values.
	uint8 gray[64];
	Gdiplus::BitmapData bmpData;
	Gdiplus::Rect rect(0, 0, 8, 8);
	if (thumb.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bmpData) == Gdiplus::Ok) {
		int nSum = 0;
		for (int y = 0; y < 8; y++) {
			uint32* pRow = (uint32*)((uint8*)bmpData.Scan0 + y * bmpData.Stride);
			for (int x = 0; x < 8; x++) {
				uint32 px = pRow[x];
				uint8 r = (px >> 16) & 0xFF;
				uint8 g = (px >> 8) & 0xFF;
				uint8 b = px & 0xFF;
				gray[y * 8 + x] = (uint8)((r * 299 + g * 587 + b * 114) / 1000);
				nSum += gray[y * 8 + x];
			}
		}
		thumb.UnlockBits(&bmpData);

		uint8 nAvg = (uint8)(nSum / 64);
		for (int i = 0; i < 64; i++) {
			if (gray[i] >= nAvg) uHash |= ((unsigned long long)1 << i);
		}
	}

	return uHash;
}

int CDuplicateFinderDlg::HammingDistance(unsigned long long a, unsigned long long b) {
	unsigned long long x = a ^ b;
	int nDist = 0;
	while (x) { nDist += (int)(x & 1); x >>= 1; }
	return nDist;
}

LRESULT CDuplicateFinderDlg::OnScan(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if (m_bScanning) return 0;
	m_bScanning = true;
	m_btnScan.EnableWindow(FALSE);

	m_entries.clear();
	m_lvFiles.DeleteAllItems();

	std::list<CFileDesc>& files = m_fileList.GetFileList();
	int nTotal = (int)files.size();
	m_progress.SetRange32(0, nTotal);
	m_progress.SetPos(0);

	int nIdx = 0;
	for (std::list<CFileDesc>::iterator it = files.begin(); it != files.end(); ++it, nIdx++) {
		CString sPath = it->GetName();
		CString sTitle = it->GetTitle();

		unsigned long long uHash = ComputePHash(sPath);

		SEntry entry;
		entry.sPath = sPath;
		entry.sTitle = sTitle;
		entry.uHash = uHash;
		entry.nGroup = -1;
		entry.bIsDup = false;
		m_entries.push_back(entry);

		m_progress.SetPos(nIdx + 1);
		CString sStatus;
		sStatus.Format(_T("Scanning %d / %d: %s"), nIdx + 1, nTotal, sTitle.GetString());
		m_lblStatus.SetWindowText(sStatus);

		// Pump messages so the dialog stays responsive.
		MSG msg;
		while (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
		}
	}

	// Group duplicates: images with Hamming distance <= 5 are considered duplicates.
	const int nThreshold = 5;
	int nGroup = 0;
	for (size_t i = 0; i < m_entries.size(); i++) {
		if (m_entries[i].nGroup >= 0) continue; // already assigned
		bool bFoundDup = false;
		for (size_t j = i + 1; j < m_entries.size(); j++) {
			if (m_entries[j].nGroup >= 0) continue;
			if (m_entries[i].uHash != 0 && m_entries[j].uHash != 0 &&
				HammingDistance(m_entries[i].uHash, m_entries[j].uHash) <= nThreshold) {
				m_entries[j].nGroup = nGroup;
				m_entries[j].bIsDup = true;
				bFoundDup = true;
			}
		}
		if (bFoundDup) {
			m_entries[i].nGroup = nGroup;
			m_entries[i].bIsDup = true;
			nGroup++;
		}
	}

	PopulateList();

	int nDupCount = 0;
	for (size_t i = 0; i < m_entries.size(); i++) if (m_entries[i].bIsDup) nDupCount++;

	CString sFinal;
	sFinal.Format(_T("Done. %d image(s) in %d duplicate group(s)."), nDupCount, nGroup);
	m_lblStatus.SetWindowText(sFinal);

	m_bScanning = false;
	m_btnScan.EnableWindow(TRUE);
	return 0;
}

void CDuplicateFinderDlg::PopulateList() {
	m_lvFiles.DeleteAllItems();
	int nItem = 0;
	for (size_t i = 0; i < m_entries.size(); i++) {
		const SEntry& e = m_entries[i];
		m_lvFiles.InsertItem(nItem, e.sTitle);
		if (e.bIsDup) {
			m_lvFiles.SetItemText(nItem, 1, _T("Duplicate"));
			CString sGroup;
			sGroup.Format(_T("#%d"), e.nGroup + 1);
			m_lvFiles.SetItemText(nItem, 2, sGroup);
			m_lvFiles.SetCheckState(nItem, TRUE);
		} else {
			m_lvFiles.SetItemText(nItem, 1, _T("Unique"));
		}
		nItem++;
	}
}

LRESULT CDuplicateFinderDlg::OnListViewItemChanged(WPARAM wParam, LPNMHDR lpnmhdr, BOOL& bHandled) {
	bHandled = FALSE;
	return 0;
}