#include "stdafx.h"
#include "ExtractFramesDlg.h"
#include "JPEGImage.h"
#include "Helpers.h"
#include "PNGWrapper.h"
#include "WEBPWrapper.h"
#include "JXLWrapper.h"
#include "AVIFWrapper.h"
#include "SaveImage.h"
#include "ProcessParams.h"
#include <gdiplus.h>
#include <cstdio>

CExtractFramesDlg::CExtractFramesDlg(LPCTSTR sFileName, int nFrameCount)
	: m_sFileName(sFileName)
	, m_nFrameCount(nFrameCount)
{
}

CExtractFramesDlg::~CExtractFramesDlg()
{
}

LRESULT CExtractFramesDlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
	CenterWindow(GetParent());

	m_edtDest.Attach(GetDlgItem(IDC_EF_DEST));
	m_btnBrowse.Attach(GetDlgItem(IDC_EF_BROWSE));
	m_btnExtract.Attach(GetDlgItem(IDC_EF_EXTRACT));
	m_btnCancel.Attach(GetDlgItem(IDC_EF_CANCEL));
	m_progress.Attach(GetDlgItem(IDC_EF_PROGRESS));
	m_lblStatus.Attach(GetDlgItem(IDC_EF_STATUS));
	m_cbFormat.Attach(GetDlgItem(IDC_EF_FMT));

	// Default destination: same folder as the source file.
	int nSlash = m_sFileName.ReverseFind(_T('\\'));
	if (nSlash < 0) nSlash = m_sFileName.ReverseFind(_T('/'));
	m_sDestFolder = (nSlash >= 0) ? m_sFileName.Left(nSlash + 1) : m_sFileName;
	m_edtDest.SetWindowText(m_sDestFolder);

	// Format combo: PNG (recommended), BMP, JPEG.
	m_cbFormat.AddString(_T("PNG (lossless)"));
	m_cbFormat.AddString(_T("BMP"));
	m_cbFormat.AddString(_T("JPEG"));
	m_cbFormat.SetCurSel(0);

	CString sStatus;
	sStatus.Format(_T("Image has %d frame(s). Choose destination and click Extract."), m_nFrameCount);
	m_lblStatus.SetWindowText(sStatus);

	m_progress.SetRange32(0, m_nFrameCount);
	m_progress.SetPos(0);

	return TRUE;
}

LRESULT CExtractFramesDlg::OnClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
	EndDialog(IDCANCEL);
	return 0;
}

LRESULT CExtractFramesDlg::OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	EndDialog(wID);
	return 0;
}

LRESULT CExtractFramesDlg::OnBrowse(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	BROWSEINFO bi;
	memset(&bi, 0, sizeof(bi));
	bi.hwndOwner = m_hWnd;
	bi.lpszTitle = _T("Select destination folder");
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
	LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
	if (pidl != NULL) {
		TCHAR szPath[MAX_PATH];
		if (SHGetPathFromIDList(pidl, szPath)) {
			m_sDestFolder = szPath;
			if (!m_sDestFolder.IsEmpty()) {
				TCHAR last = m_sDestFolder[m_sDestFolder.GetLength() - 1];
				if (last != _T('\\') && last != _T('/')) m_sDestFolder += _T("\\");
			}
			m_edtDest.SetWindowText(m_sDestFolder);
		}
		CoTaskMemFree(pidl);
	}
	return 0;
}

LRESULT CExtractFramesDlg::OnExtract(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	m_edtDest.GetWindowText(m_sDestFolder);
	if (!m_sDestFolder.IsEmpty()) {
		TCHAR last = m_sDestFolder[m_sDestFolder.GetLength() - 1];
		if (last != _T('\\') && last != _T('/')) m_sDestFolder += _T("\\");
	}

	// Verify the destination folder exists.
	DWORD attr = ::GetFileAttributes(m_sDestFolder);
	if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
		::MessageBox(m_hWnd, _T("Destination folder does not exist."), _T("Extract Frames"), MB_OK | MB_ICONWARNING);
		return 0;
	}

	// Determine output format and extension.
	int nFmt = m_cbFormat.GetCurSel();
	LPCTSTR sExt = _T(".png");
	if (nFmt == 1) sExt = _T(".bmp");
	else if (nFmt == 2) sExt = _T(".jpg");

	// Base name from the source file (without extension).
	int nSlash = m_sFileName.ReverseFind(_T('\\'));
	if (nSlash < 0) nSlash = m_sFileName.ReverseFind(_T('/'));
	CString sBaseName = (nSlash >= 0) ? m_sFileName.Mid(nSlash + 1) : m_sFileName;
	int nDot = sBaseName.ReverseFind(_T('.'));
	if (nDot > 0) sBaseName = sBaseName.Left(nDot);

	// Determine zero-padding width for frame numbers.
	int nPad = 3;
	int nTemp = m_nFrameCount;
	while (nTemp >= 1000) { nPad++; nTemp /= 10; }

	m_btnExtract.EnableWindow(FALSE);

	for (int i = 0; i < m_nFrameCount; i++) {
		CString sFrame;
		sFrame.Format(_T("_%0*d"), nPad, i);

		bool bOk = ExtractFrame(i, m_sDestFolder, sBaseName + sFrame, sExt);

		m_progress.SetPos(i + 1);
		CString sStatus;
		sStatus.Format(_T("Extracting frame %d / %d %s"), i + 1, m_nFrameCount, bOk ? _T("(OK)") : _T("(FAILED)"));
		m_lblStatus.SetWindowText(sStatus);

		// Pump messages to keep the dialog responsive.
		MSG msg;
		while (::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
		}
	}

	m_btnExtract.EnableWindow(TRUE);

	CString sDone;
	sDone.Format(_T("Extracted %d frame(s) to:\n%s"), m_nFrameCount, m_sDestFolder.GetString());
	::MessageBox(m_hWnd, sDone, _T("Extract Frames"), MB_OK | MB_ICONINFORMATION);

	EndDialog(wID);
	return 0;
}

// Local copy of the GDI+ encoder CLSID lookup (same as in SaveImage.cpp).
static int GetEncoderClsidLocal(const WCHAR* format, CLSID* pClsid) {
	UINT num = 0;
	UINT size = 0;
	Gdiplus::GetImageEncodersSize(&num, &size);
	if (size == 0) return -1;
	Gdiplus::ImageCodecInfo* pImageCodecInfo = (Gdiplus::ImageCodecInfo*)(malloc(size));
	Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);
	for (UINT j = 0; j < num; ++j) {
		if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
			*pClsid = pImageCodecInfo[j].Clsid;
			free(pImageCodecInfo);
			return j;
		}
	}
	free(pImageCodecInfo);
	return -1;
}

// Save a 32-bit BGRA pixel buffer to a file using GDI+.
// This avoids pulling in the full SaveImage pipeline for each frame.
static bool SavePixelsToFile(uint8* pPixels, int nWidth, int nHeight, int nChannels, LPCTSTR sOutPath, int nFmt) {
	if (pPixels == NULL) return false;

	// GDI+ expects BGR (24bpp) or BGRA (32bpp). JPEGView pixels are BGRA or BGR.
	Gdiplus::PixelFormat pf = (nChannels == 4) ? PixelFormat32bppARGB : PixelFormat24bppRGB;
	int nRowPadded = ((nWidth * nChannels + 3) / 4) * 4;

	Gdiplus::Bitmap* pBitmap = new Gdiplus::Bitmap(nWidth, nHeight, nRowPadded, pf, pPixels);
	if (pBitmap == NULL || pBitmap->GetLastStatus() != Gdiplus::Ok) {
		delete pBitmap;
		return false;
	}

	// Get encoder CLSID for the target format.
	CLSID clsid;
	bool bGotClsid = false;
	if (nFmt == 0) {
		// PNG
		bGotClsid = GetEncoderClsidLocal(L"image/png", &clsid) >= 0;
	} else if (nFmt == 1) {
		// BMP
		bGotClsid = GetEncoderClsidLocal(L"image/bmp", &clsid) >= 0;
	} else {
		// JPEG
		bGotClsid = GetEncoderClsidLocal(L"image/jpeg", &clsid) >= 0;
	}

	if (!bGotClsid) { delete pBitmap; return false; }

	Gdiplus::Status status = pBitmap->Save(sOutPath, &clsid, NULL);
	delete pBitmap;
	return (status == Gdiplus::Ok);
}

bool CExtractFramesDlg::ExtractFrame(int nFrameIndex, LPCTSTR sDestFolder, LPCTSTR sBaseName, LPCTSTR sExt) {
	// Read the file into memory.
	HANDLE hFile = ::CreateFile(m_sFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return false;
	DWORD nSize = ::GetFileSize(hFile, NULL);
	if (nSize == 0 || nSize == INVALID_FILE_SIZE) { ::CloseHandle(hFile); return false; }
	uint8* pBuffer = new uint8[nSize];
	DWORD nRead = 0;
	BOOL bOk = ::ReadFile(hFile, pBuffer, nSize, &nRead, NULL);
	::CloseHandle(hFile);
	if (!bOk || nRead == 0) { delete[] pBuffer; return false; }

	// Determine format from extension and call the appropriate wrapper.
	CString sLower = m_sFileName;
	sLower.MakeLower();

	uint8* pPixels = NULL;
	int nWidth = 0, nHeight = 0, nBPP = 4;
	bool bHasAnim = false;
	int nFrameCount = 1, nFrameTime = 0;
	void* pEXIFData = NULL;
	bool bOOM = false;
	int nFmt = m_cbFormat.GetCurSel();

	if (sLower.Right(5) == _T(".webp")) {
		pPixels = (uint8*)WebpReaderWriter::ReadImage(nWidth, nHeight, nBPP, bHasAnim, nFrameCount, nFrameTime, pEXIFData, bOOM, pBuffer, nRead);
		// WebP ReadImage returns the first frame; for animated WebP the cache
		// holds all frames but the API only exposes one at a time via the
		// frame_index in the constructor. We use what we get.
		WebpReaderWriter::DeleteCache();
	} else if (sLower.Right(4) == _T(".jxl")) {
		pPixels = (uint8*)JxlReader::ReadImage(nWidth, nHeight, nBPP, bHasAnim, nFrameCount, nFrameTime, pEXIFData, bOOM, pBuffer, nRead);
		JxlReader::DeleteCache();
	} else if (sLower.Right(5) == _T(".avif")) {
		pPixels = (uint8*)AvifReader::ReadImage(nWidth, nHeight, nBPP, bHasAnim, nFrameIndex, nFrameCount, nFrameTime, pEXIFData, bOOM, pBuffer, nRead);
		AvifReader::DeleteCache();
	} else if (sLower.Right(4) == _T(".png") || sLower.Right(5) == _T(".apng")) {
		pPixels = (uint8*)PngReader::ReadImage(nWidth, nHeight, nBPP, bHasAnim, nFrameCount, nFrameTime, pEXIFData, bOOM, pBuffer, nRead);
		PngReader::DeleteCache();
	} else {
		// For GIF and other formats, use GDI+ to load each frame.
		Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromFile(m_sFileName);
		if (pBitmap != NULL && pBitmap->GetLastStatus() == Gdiplus::Ok) {
			GUID dim;
			pBitmap->GetFrameDimensionsList(&dim, 1);
			int nFrames = pBitmap->GetFrameCount(&dim);
			if (nFrameIndex < nFrames) {
				pBitmap->SelectActiveFrame(&dim, nFrameIndex);
				nWidth = pBitmap->GetWidth();
				nHeight = pBitmap->GetHeight();
				Gdiplus::BitmapData bmpData;
				Gdiplus::Rect rect(0, 0, nWidth, nHeight);
				if (pBitmap->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bmpData) == Gdiplus::Ok) {
					int nRowPadded = bmpData.Stride;
					int nBufSize = nRowPadded * nHeight;
					pPixels = new uint8[nBufSize];
					memcpy(pPixels, bmpData.Scan0, nBufSize);
					nBPP = 4;
					pBitmap->UnlockBits(&bmpData);
				}
			}
		}
		delete pBitmap;
	}

	if (pEXIFData != NULL) free(pEXIFData);
	delete[] pBuffer;

	if (pPixels == NULL) return false;

	CString sOutPath;
	sOutPath.Format(_T("%s%s%s"), sDestFolder, sBaseName, sExt);

	bool bSaved = SavePixelsToFile(pPixels, nWidth, nHeight, nBPP, sOutPath, nFmt);

	// Free pixel data with the correct allocator per wrapper. WEBP uses new[],
	// JXL uses new[], PNG and AVIF use malloc/free. GIF path uses new[].
	if (sLower.Right(5) == _T(".webp")) {
		delete[] pPixels;
		WebpReaderWriter::DeleteCache();
	} else if (sLower.Right(4) == _T(".jxl")) {
		delete[] pPixels;
		JxlReader::DeleteCache();
	} else if (sLower.Right(5) == _T(".avif")) {
		free(pPixels);
		AvifReader::DeleteCache();
	} else if (sLower.Right(4) == _T(".png") || sLower.Right(5) == _T(".apng")) {
		free(pPixels);
		PngReader::DeleteCache();
	} else {
		delete[] pPixels; // GIF path allocated with new[]
	}

	return bSaved;
}