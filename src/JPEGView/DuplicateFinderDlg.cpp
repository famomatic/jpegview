#include "stdafx.h"
#include "DuplicateFinderDlg.h"
#include "FileList.h"
#include "Helpers.h"
#include "HelpersGUI.h"
#include <gdiplus.h>
#include <list>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>

CDuplicateFinderDlg::CDuplicateFinderDlg(CFileList& fileList)
	: m_fileList(fileList)
	, m_bScanning(false)
	, m_bCancelRequested(false)
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
	if (m_bScanning) {
		m_bCancelRequested = true;
		m_btnCancel.EnableWindow(FALSE);
		m_lblStatus.SetWindowText(_T("Cancelling..."));
		return 0;
	}
	EndDialog(IDCANCEL);
	return 0;
}

LRESULT CDuplicateFinderDlg::OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if (m_bScanning) {
		m_bCancelRequested = true;
		m_btnCancel.EnableWindow(FALSE);
		m_lblStatus.SetWindowText(_T("Cancelling..."));
		return 0;
	}
	EndDialog(wID);
	return 0;
}

LRESULT CDuplicateFinderDlg::OnCloseCmd(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if (m_bScanning) {
		m_bCancelRequested = true;
		m_btnCancel.EnableWindow(FALSE);
		m_lblStatus.SetWindowText(_T("Cancelling..."));
		return 0;
	}
	EndDialog(wID);
	return 0;
}

// Compute a DCT-based 63-bit perceptual hash plus coarse color/aspect metadata.
// GDI+ handles JPEG, PNG, BMP, GIF, TIFF — the most common formats. Files in
// exotic formats simply get hash 0 and are treated as unique.
bool CDuplicateFinderDlg::ComputePHash(LPCTSTR sFileName, unsigned long long& uHash,
	unsigned int& uAverageColor, double& dAspectRatio) {
	uHash = 0;
	uAverageColor = 0;
	dAspectRatio = 0.0;

	Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromFile(sFileName);
	if (pBitmap == NULL) return false;
	if (pBitmap->GetLastStatus() != Gdiplus::Ok) { delete pBitmap; return false; }

	int nW = pBitmap->GetWidth();
	int nH = pBitmap->GetHeight();
	if (nW <= 0 || nH <= 0) { delete pBitmap; return false; }
	dAspectRatio = static_cast<double>(nW) / nH;

	Gdiplus::Bitmap thumb(32, 32, PixelFormat32bppARGB);
	Gdiplus::Graphics g(&thumb);
	g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
	const Gdiplus::Status drawStatus = g.DrawImage(pBitmap, 0, 0, 32, 32);
	delete pBitmap;
	if (drawStatus != Gdiplus::Ok) return false;

	double gray[32][32] = {};
	Gdiplus::BitmapData bmpData;
	Gdiplus::Rect rect(0, 0, 32, 32);
	if (thumb.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bmpData) != Gdiplus::Ok)
		return false;
	unsigned long long sumR = 0, sumG = 0, sumB = 0;
	for (int y = 0; y < 32; y++) {
			uint32* pRow = (uint32*)((uint8*)bmpData.Scan0 + y * bmpData.Stride);
			for (int x = 0; x < 32; x++) {
				uint32 px = pRow[x];
				uint8 r = (px >> 16) & 0xFF;
				uint8 green = (px >> 8) & 0xFF;
				uint8 b = px & 0xFF;
				gray[y][x] = (r * 299.0 + green * 587.0 + b * 114.0) / 1000.0;
				sumR += r; sumG += green; sumB += b;
			}
		}
	thumb.UnlockBits(&bmpData);
	uAverageColor = (static_cast<unsigned int>(sumR / 1024) << 16) |
		(static_cast<unsigned int>(sumG / 1024) << 8) | static_cast<unsigned int>(sumB / 1024);

	double coefficients[63] = {};
	int coefficientIndex = 0;
	for (int v = 0; v < 8; v++) {
		for (int u = 0; u < 8; u++) {
			if (u == 0 && v == 0) continue;
			double value = 0.0;
			for (int y = 0; y < 32; y++) {
				const double cosY = std::cos((2 * y + 1) * v * 3.14159265358979323846 / 64.0);
				for (int x = 0; x < 32; x++)
					value += gray[y][x] * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 64.0) * cosY;
			}
			coefficients[coefficientIndex++] = value;
	}
	}
	double sorted[63];
	memcpy(sorted, coefficients, sizeof(sorted));
	std::nth_element(sorted, sorted + 31, sorted + 63);
	const double median = sorted[31];
	for (int i = 0; i < 63; i++)
		if (coefficients[i] > median) uHash |= (1ULL << i);

	return true;
}

int CDuplicateFinderDlg::HammingDistance(unsigned long long a, unsigned long long b) {
	unsigned long long x = a ^ b;
	int nDist = 0;
	while (x) { nDist += (int)(x & 1); x >>= 1; }
	return nDist;
}

bool CDuplicateFinderDlg::PumpMessages() {
	MSG msg;
	while (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
		if (msg.message == WM_QUIT) {
			::PostQuitMessage(static_cast<int>(msg.wParam));
			m_bCancelRequested = true;
			break;
		}
		if (!::IsDialogMessage(m_hWnd, &msg)) {
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
		}
	}
	return !m_bCancelRequested;
}

LRESULT CDuplicateFinderDlg::OnScan(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	if (m_bScanning) return 0;
	m_bScanning = true;
	m_bCancelRequested = false;
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

		unsigned long long uHash = 0;
		unsigned int uAverageColor = 0;
		double dAspectRatio = 0.0;
		const bool bHashValid = ComputePHash(sPath, uHash, uAverageColor, dAspectRatio);

		SEntry entry;
		entry.sPath = sPath;
		entry.sTitle = sTitle;
		entry.uHash = uHash;
		entry.uAverageColor = uAverageColor;
		entry.dAspectRatio = dAspectRatio;
		entry.bHashValid = bHashValid;
		entry.nGroup = -1;
		entry.bIsDup = false;
		m_entries.push_back(entry);

		m_progress.SetPos(nIdx + 1);
		CString sStatus;
		sStatus.Format(_T("Scanning %d / %d: %s"), nIdx + 1, nTotal, sTitle.GetString());
		m_lblStatus.SetWindowText(sStatus);

		if (!PumpMessages()) break;
	}
	if (m_bCancelRequested) {
		m_bScanning = false;
		EndDialog(IDCANCEL);
		return 0;
	}

	// A BK-tree avoids the quadratic all-pairs comparison. Union-find preserves
	// transitive groups (A~B and B~C means all three appear in one group).
	const int nThreshold = 5;
	struct BKNode {
		unsigned long long hash;
		std::vector<size_t> entries;
		std::array<int, 65> children;
		BKNode(unsigned long long value, size_t entry) : hash(value), entries(1, entry) { children.fill(-1); }
	};
	std::vector<BKNode> tree;
	std::vector<size_t> parent(m_entries.size());
	for (size_t i = 0; i < parent.size(); i++) parent[i] = i;
	auto findRoot = [&parent](size_t value) {
		size_t root = value;
		while (parent[root] != root) root = parent[root];
		while (parent[value] != value) { size_t next = parent[value]; parent[value] = root; value = next; }
		return root;
	};
	auto unionEntries = [&parent, &findRoot](size_t a, size_t b) {
		a = findRoot(a); b = findRoot(b);
		if (a != b) parent[b] = a;
	};
	for (size_t i = 0; i < m_entries.size(); i++) {
		if (!m_entries[i].bHashValid) continue;
		if (!tree.empty()) {
			std::vector<int> pending(1, 0);
			while (!pending.empty()) {
				const int nodeIndex = pending.back(); pending.pop_back();
				const BKNode& node = tree[nodeIndex];
				const int distance = HammingDistance(m_entries[i].uHash, node.hash);
				if (distance <= nThreshold) {
					for (size_t candidate : node.entries) {
						const unsigned int colorA = m_entries[i].uAverageColor;
						const unsigned int colorB = m_entries[candidate].uAverageColor;
						const int colorDistance = abs(static_cast<int>((colorA >> 16) & 255) - static_cast<int>((colorB >> 16) & 255)) +
							abs(static_cast<int>((colorA >> 8) & 255) - static_cast<int>((colorB >> 8) & 255)) +
							abs(static_cast<int>(colorA & 255) - static_cast<int>(colorB & 255));
						const double aspectA = m_entries[i].dAspectRatio;
						const double aspectB = m_entries[candidate].dAspectRatio;
						if (colorDistance <= 90 && std::abs(aspectA - aspectB) <= max(aspectA, aspectB) * 0.03)
							unionEntries(i, candidate);
					}
				}
				const int first = max(0, distance - nThreshold);
				const int last = min(64, distance + nThreshold);
				for (int d = first; d <= last; d++) if (node.children[d] >= 0) pending.push_back(node.children[d]);
			}
		}
		if (tree.empty()) {
			tree.emplace_back(m_entries[i].uHash, i);
		} else {
			int nodeIndex = 0;
			for (;;) {
				const int distance = HammingDistance(m_entries[i].uHash, tree[nodeIndex].hash);
				if (distance == 0) { tree[nodeIndex].entries.push_back(i); break; }
				int& child = tree[nodeIndex].children[distance];
				if (child < 0) { child = static_cast<int>(tree.size()); tree.emplace_back(m_entries[i].uHash, i); break; }
				nodeIndex = child;
			}
		}
		if ((i & 31) == 0 && !PumpMessages()) break;
	}
	if (m_bCancelRequested) {
		m_bScanning = false;
		EndDialog(IDCANCEL);
		return 0;
	}

	std::map<size_t, int> groupNumbers;
	std::map<size_t, int> groupSizes;
	for (size_t i = 0; i < m_entries.size(); i++) groupSizes[findRoot(i)]++;
	int nGroup = 0;
	for (size_t i = 0; i < m_entries.size(); i++) {
		const size_t root = findRoot(i);
		if (groupSizes[root] < 2) continue;
		std::map<size_t, int>::iterator groupIt = groupNumbers.find(root);
		if (groupIt == groupNumbers.end()) groupIt = groupNumbers.insert(std::make_pair(root, nGroup++)).first;
		m_entries[i].nGroup = groupIt->second;
		m_entries[i].bIsDup = true;
	}

	PopulateList();

	int nDupCount = 0;
	for (size_t i = 0; i < m_entries.size(); i++) if (m_entries[i].bIsDup) nDupCount++;

	CString sFinal;
	sFinal.Format(_T("Done. %d image(s) in %d duplicate group(s)."), nDupCount, nGroup);
	m_lblStatus.SetWindowText(sFinal);

	m_bScanning = false;
	m_btnScan.EnableWindow(TRUE);
	m_btnCancel.EnableWindow(TRUE);
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
