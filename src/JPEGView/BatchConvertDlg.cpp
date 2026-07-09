#include "stdafx.h"
#include "BatchConvertDlg.h"
#include "FileList.h"
#include "Helpers.h"
#include "NLS.h"
#include "SettingsProvider.h"
#include "SaveImage.h"
#include "JPEGImage.h"
#include "BasicProcessing.h"
#include "TJPEGWrapper.h"
#include "WEBPWrapper.h"
#include "QOIWrapper.h"
#include "JXLWrapper.h"
#include "AVIFWrapper.h"
#include "HEIFWrapper.h"
#include "PNGWrapper.h"
#include "TIFFWrapper.h"
#include "ReaderBMP.h"
#include "ReaderTGA.h"
#include "EXIFReader.h"
#include "ThumbnailCache.h"
#include <gdiplus.h>
#include <shlobj.h>

// Supported output formats
static const LPCTSTR g_szFormats[] = {
	_T("JPEG (.jpg)"), _T("PNG (.png)"), _T("WebP (.webp)"),
	_T("JPEG XL (.jxl)"), _T("AVIF (.avif)"), _T("HEIF (.heic)"),
	_T("TIFF (.tif)"), _T("BMP (.bmp)"), _T("QOI (.qoi)"), _T("TGA (.tga)")
};
static const LPCTSTR g_szExtensions[] = {
	_T("jpg"), _T("png"), _T("webp"),
	_T("jxl"), _T("avif"), _T("heic"),
	_T("tif"), _T("bmp"), _T("qoi"), _T("tga")
};
static const int g_nNumFormats = 10;

static bool FormatHasQuality(int nFormatIdx) {
	return nFormatIdx == 0 || nFormatIdx == 2 || nFormatIdx == 3 || nFormatIdx == 4 || nFormatIdx == 5;
}

CBatchConvertDlg::CBatchConvertDlg(CFileList& fileList) : m_fileList(fileList) {
	m_nNumFiles = 0;
	m_bConverting = false;
}

CBatchConvertDlg::~CBatchConvertDlg() {
}

LRESULT CBatchConvertDlg::OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
	CenterWindow(GetParent());

	m_lvFiles.Attach(GetDlgItem(IDC_BC_LIST));
	m_cbFormat.Attach(GetDlgItem(IDC_BC_FORMAT));
	m_edtQuality.Attach(GetDlgItem(IDC_BC_QUALITY));
	m_edtDest.Attach(GetDlgItem(IDC_BC_DEST));
	m_btnBrowse.Attach(GetDlgItem(IDC_BC_BROWSE));
	m_btnConvert.Attach(GetDlgItem(IDC_BC_CONVERT));
	m_btnCancel.Attach(GetDlgItem(IDC_BC_CANCEL));
	m_btnSelectAll.Attach(GetDlgItem(IDC_BC_SELECTALL));
	m_btnSelectNone.Attach(GetDlgItem(IDC_BC_SELECTNONE));
	m_btnKeepOrig.Attach(GetDlgItem(IDC_BC_KEEP_ORIG));
	m_btnResize.Attach(GetDlgItem(IDC_BC_RESIZE));
	m_edtMaxWidth.Attach(GetDlgItem(IDC_BC_MAXWIDTH));
	m_edtMaxHeight.Attach(GetDlgItem(IDC_BC_MAXHEIGHT));
	m_progress.Attach(GetDlgItem(IDC_BC_PROGRESS));
	m_lblStatus.Attach(GetDlgItem(IDC_BC_STATUS));

	for (int i = 0; i < g_nNumFormats; i++) {
		m_cbFormat.AddString(g_szFormats[i]);
	}
	m_cbFormat.SetCurSel(0);

	CString sQuality;
	sQuality.Format(_T("%d"), CSettingsProvider::This().BatchConvertQuality());
	m_edtQuality.SetWindowText(sQuality);

	m_edtDest.SetWindowText(m_fileList.CurrentDirectory());
	m_edtMaxWidth.SetWindowText(_T("0"));
	m_edtMaxHeight.SetWindowText(_T("0"));

	m_lvFiles.SetExtendedListViewStyle(LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
	m_lvFiles.InsertColumn(0, CNLS::GetString(_T("File name")), LVCFMT_LEFT, 300, 0);
	m_lvFiles.InsertColumn(1, CNLS::GetString(_T("Size")), LVCFMT_RIGHT, 80, 1);

	CreateItemList();

	// Select all by default
	for (int i = 0; i < m_nNumFiles; i++) {
		ListView_SetCheckState(m_lvFiles.m_hWnd, i, TRUE);
	}

	UpdateQualityEnabled();

	return 1;
}

int CBatchConvertDlg::CreateItemList() {
	m_lvFiles.DeleteAllItems();
	int nItem = 0;
	const std::list<CFileDesc>& fileList = m_fileList.GetFileList();
	for (std::list<CFileDesc>::const_iterator iter = fileList.begin(); iter != fileList.end(); iter++) {
		CString sTitle = iter->GetTitle();
		m_lvFiles.InsertItem(nItem, sTitle);
		CString sSize;
		__int64 nSize = iter->GetFileSize();
		if (nSize < 1024) sSize.Format(_T("%d B"), (int)nSize);
		else if (nSize < 1024 * 1024) sSize.Format(_T("%.1f KB"), nSize / 1024.0);
		else sSize.Format(_T("%.1f MB"), nSize / (1024.0 * 1024.0));
		m_lvFiles.SetItemText(nItem, 1, sSize);
		nItem++;
	}
	m_nNumFiles = nItem;
	return nItem;
}

LRESULT CBatchConvertDlg::OnClose(UINT, WPARAM, LPARAM, BOOL&) {
	EndDialog(IDCANCEL);
	return 0;
}

LRESULT CBatchConvertDlg::OnCancel(WORD, WORD wID, HWND, BOOL&) {
	EndDialog(wID);
	return 0;
}

LRESULT CBatchConvertDlg::OnSelectAll(WORD, WORD, HWND, BOOL&) {
	for (int i = 0; i < m_nNumFiles; i++) {
		ListView_SetCheckState(m_lvFiles.m_hWnd, i, TRUE);
	}
	return 0;
}

LRESULT CBatchConvertDlg::OnSelectNone(WORD, WORD, HWND, BOOL&) {
	for (int i = 0; i < m_nNumFiles; i++) {
		ListView_SetCheckState(m_lvFiles.m_hWnd, i, FALSE);
	}
	return 0;
}

LRESULT CBatchConvertDlg::OnBrowse(WORD, WORD, HWND, BOOL&) {
	BROWSEINFO bi;
	memset(&bi, 0, sizeof(bi));
	bi.hwndOwner = m_hWnd;
	bi.lpszTitle = CNLS::GetString(_T("Select output folder"));
	bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
	LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
	if (pidl != NULL) {
		TCHAR szPath[MAX_PATH];
		if (SHGetPathFromIDList(pidl, szPath)) {
			m_edtDest.SetWindowText(szPath);
		}
		CoTaskMemFree(pidl);
	}
	return 0;
}

LRESULT CBatchConvertDlg::OnFormatChanged(WORD, WORD, HWND, BOOL&) {
	UpdateQualityEnabled();
	return 0;
}

void CBatchConvertDlg::UpdateQualityEnabled() {
	int nSel = m_cbFormat.GetCurSel();
	BOOL bEnable = FormatHasQuality(nSel) ? TRUE : FALSE;
	m_edtQuality.EnableWindow(bEnable);
	GetDlgItem(IDC_BC_QUALITY_LBL).EnableWindow(bEnable);
}

LRESULT CBatchConvertDlg::OnListViewItemChanged(WPARAM, LPNMHDR, BOOL&) {
	return 0;
}

CString CBatchConvertDlg::GetOutputPath(LPCTSTR sInputPath, LPCTSTR sInputFile, LPCTSTR sExt, bool bKeepOriginal) {
	CString sTitle = sInputFile;
	int nDot = sTitle.ReverseFind(_T('.'));
	if (nDot >= 0) sTitle = sTitle.Left(nDot);
	int nSlash = sTitle.ReverseFind(_T('\\'));
	if (nSlash >= 0) sTitle = sTitle.Mid(nSlash + 1);

	CString sOutput;
	if (bKeepOriginal) {
		sOutput.Format(_T("%s\\%s_converted.%s"), sInputPath, sTitle, sExt);
	} else {
		sOutput.Format(_T("%s\\%s.%s"), sInputPath, sTitle, sExt);
	}
	return sOutput;
}

// Load an image file into a 32bpp BGRA buffer. Returns pixel data (caller frees with delete[]).
static void* LoadImageToBGRA(LPCTSTR sFileName, int& nWidth, int& nHeight, int& nChannels) {
	nWidth = nHeight = 0;
	nChannels = 4;
	EImageFormat eFormat = Helpers::GetImageFormat(sFileName);

	// Read file into memory
	HANDLE hFile = ::CreateFile(sFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return NULL;
	DWORD nFileSize = ::GetFileSize(hFile, NULL);
	if (nFileSize == 0 || nFileSize > 500 * 1024 * 1024) {
		::CloseHandle(hFile);
		return NULL;
	}
	uint8* pBuffer = new(std::nothrow) uint8[nFileSize];
	if (pBuffer == NULL) {
		::CloseHandle(hFile);
		return NULL;
	}
	DWORD nRead = 0;
	BOOL bRead = ::ReadFile(hFile, pBuffer, nFileSize, &nRead, NULL);
	::CloseHandle(hFile);
	if (!bRead || nRead != nFileSize) {
		delete[] pBuffer;
		return NULL;
	}

	void* pPixels = NULL;
	bool bOutOfMemory = false;
	int nBPP;
	bool bHasAnimation;
	int nFrameCount, nFrameTime;
	void* pEXIF = NULL;

	switch (eFormat) {
		case IF_JPEG:
		case IF_JPEG_Embedded: {
			TJSAMP chromoSubsampling;
			pPixels = TurboJpeg::ReadImage(nWidth, nHeight, nBPP, chromoSubsampling, bOutOfMemory, pBuffer, nFileSize);
			break;
		}
		case IF_WEBP:
			pPixels = WebpReaderWriter::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTime, pEXIF, bOutOfMemory, pBuffer, nFileSize);
			break;
		case IF_PNG:
			pPixels = PngReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTime, pEXIF, bOutOfMemory, pBuffer, nFileSize);
			break;
		case IF_JXL:
			pPixels = JxlReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTime, pEXIF, bOutOfMemory, pBuffer, nFileSize);
			break;
		case IF_AVIF:
			pPixels = AvifReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, 0, nFrameCount, nFrameTime, pEXIF, bOutOfMemory, pBuffer, nFileSize);
			break;
		case IF_HEIF:
			pPixels = HeifReader::ReadImage(nWidth, nHeight, nBPP, nFrameCount, pEXIF, bOutOfMemory, 0, pBuffer, nFileSize);
			break;
		case IF_QOI:
			pPixels = QoiReaderWriter::ReadImage(nWidth, nHeight, nBPP, bOutOfMemory, pBuffer, nFileSize);
			break;
		case IF_WindowsBMP: {
			CJPEGImage* pImg = CReaderBMP::ReadBmpImage(sFileName, bOutOfMemory);
			if (pImg) {
				nWidth = pImg->OrigWidth();
				nHeight = pImg->OrigHeight();
				nBPP = pImg->OriginalChannels();
				// BMP reader returns 3-channel BGR; copy the buffer
				int nRowPadded = Helpers::DoPadding(nWidth * nBPP, 4);
				int nSize = nRowPadded * nHeight;
				pPixels = new uint8[nSize];
				memcpy(pPixels, pImg->OriginalPixels(), nSize);
				pImg->DetachOriginalPixels();
				delete pImg;
			}
			break;
		}
		case IF_TGA: {
			CJPEGImage* pImg = CReaderTGA::ReadTgaImage(sFileName, 0, bOutOfMemory);
			if (pImg) {
				nWidth = pImg->OrigWidth();
				nHeight = pImg->OrigHeight();
				nBPP = pImg->OriginalChannels();
				int nRowPadded = Helpers::DoPadding(nWidth * nBPP, 4);
				int nSize = nRowPadded * nHeight;
				pPixels = new uint8[nSize];
				memcpy(pPixels, pImg->OriginalPixels(), nSize);
				pImg->DetachOriginalPixels();
				delete pImg;
			}
			break;
		}
		default:
			delete[] pBuffer;
			return NULL;
	}

	delete[] pBuffer;
	if (pEXIF) free(pEXIF);

	if (pPixels == NULL) return NULL;

	// Ensure BGRA 4-channel
	if (nBPP == 3) {
		int nSize = nWidth * nHeight * 4;
		uint8* pBGRA = new(std::nothrow) uint8[nSize];
		if (pBGRA == NULL) {
			free(pPixels);
			return NULL;
		}
		uint8* pSrc = (uint8*)pPixels;
		int nSrcRowPadded = (nWidth * 3 + 3) & ~3;
		for (int y = 0; y < nHeight; y++) {
			uint8* pS = pSrc + y * nSrcRowPadded;
			uint8* pD = pBGRA + y * nWidth * 4;
			for (int x = 0; x < nWidth; x++) {
				pD[x * 4 + 0] = pS[x * 3 + 0]; // B
				pD[x * 4 + 1] = pS[x * 3 + 1]; // G
				pD[x * 4 + 2] = pS[x * 3 + 2]; // R
				pD[x * 4 + 3] = 255;           // A
			}
		}
		free(pPixels);
		pPixels = pBGRA;
		nChannels = 4;
	} else {
		nChannels = 4;
	}

	return pPixels;
}

// Resize BGRA buffer to fit within max dimensions, preserving aspect ratio.
static void* ResizeBGRA(void* pPixels, int& nWidth, int& nHeight, int nMaxWidth, int nMaxHeight) {
	if (nMaxWidth <= 0 && nMaxHeight <= 0) return pPixels;
	if (nMaxWidth <= 0) nMaxWidth = nWidth;
	if (nMaxHeight <= 0) nMaxHeight = nHeight;

	double dScaleX = (double)nMaxWidth / nWidth;
	double dScaleY = (double)nMaxHeight / nHeight;
	double dScale = min(dScaleX, dScaleY);
	if (dScale >= 1.0) return pPixels;

	int nNewW = (int)(nWidth * dScale + 0.5);
	int nNewH = (int)(nHeight * dScale + 0.5);
	if (nNewW < 1) nNewW = 1;
	if (nNewH < 1) nNewH = 1;

	uint8* pResized = new(std::nothrow) uint8[nNewW * nNewH * 4];
	if (pResized == NULL) return pPixels;

	uint8* pSrc = (uint8*)pPixels;
	uint8* pDst = pResized;
	for (int y = 0; y < nNewH; y++) {
		double srcY = (double)y * nHeight / nNewH;
		int y0 = (int)srcY;
		int y1 = min(y0 + 1, nHeight - 1);
		double fy = srcY - y0;
		for (int x = 0; x < nNewW; x++) {
			double srcX = (double)x * nWidth / nNewW;
			int x0 = (int)srcX;
			int x1 = min(x0 + 1, nWidth - 1);
			double fx = srcX - x0;
			for (int c = 0; c < 4; c++) {
				double v00 = pSrc[y0 * nWidth * 4 + x0 * 4 + c];
				double v01 = pSrc[y0 * nWidth * 4 + x1 * 4 + c];
				double v10 = pSrc[y1 * nWidth * 4 + x0 * 4 + c];
				double v11 = pSrc[y1 * nWidth * 4 + x1 * 4 + c];
				double v = v00 * (1 - fx) * (1 - fy) + v01 * fx * (1 - fy) +
					v10 * (1 - fx) * fy + v11 * fx * fy;
				pDst[y * nNewW * 4 + x * 4 + c] = (uint8)(v + 0.5);
			}
		}
	}

	delete[](uint8*)pPixels;
	nWidth = nNewW;
	nHeight = nNewH;
	return pResized;
}

// Save BGRA buffer to file in the specified format.
static bool SaveBGRAToFile(LPCTSTR sFileName, void* pBGRA, int nWidth, int nHeight, int nFormatIdx, int nQuality) {
	uint32 nRowPadded = Helpers::DoPadding(nWidth * 3, 4);
	uint32 nSizeBytes = nRowPadded * nHeight;
	char* pDIB24 = new(std::nothrow) char[nSizeBytes];
	if (pDIB24 == NULL) return false;

	uint8* pSrc = (uint8*)pBGRA;
	for (int y = 0; y < nHeight; y++) {
		uint8* pS = pSrc + y * nWidth * 4;
		char* pD = pDIB24 + y * nRowPadded;
		for (int x = 0; x < nWidth; x++) {
			pD[x * 3 + 0] = pS[x * 4 + 0]; // B
			pD[x * 3 + 1] = pS[x * 4 + 1]; // G
			pD[x * 3 + 2] = pS[x * 4 + 2]; // R
		}
	}

	bool bSuccess = false;
	FILE* fptr = NULL;

	switch (nFormatIdx) {
		case 0: { // JPEG
			int nStreamLen;
			bool bOOM;
			unsigned char* pStream = (unsigned char*)TurboJpeg::Compress(pDIB24, nWidth, nHeight, nStreamLen, bOOM, nQuality);
			if (pStream) {
				fptr = _tfopen(sFileName, _T("wb"));
				if (fptr) {
					bSuccess = fwrite(pStream, 1, nStreamLen, fptr) == (size_t)nStreamLen;
					fclose(fptr);
				}
				TurboJpeg::Free(pStream);
			}
			break;
		}
		case 1: case 6: case 7: { // PNG / TIFF / BMP via GDI+
			Gdiplus::Bitmap* pBitmap = new Gdiplus::Bitmap(nWidth, nHeight, nRowPadded, PixelFormat24bppRGB, (BYTE*)pDIB24);
			if (pBitmap->GetLastStatus() == Gdiplus::Ok) {
				CLSID clsid;
				UINT num = 0, size = 0;
				Gdiplus::GetImageEncodersSize(&num, &size);
				Gdiplus::ImageCodecInfo* pCodecs = (Gdiplus::ImageCodecInfo*)malloc(size);
				Gdiplus::GetImageEncoders(num, size, pCodecs);
				const wchar_t* mime = (nFormatIdx == 1) ? L"image/png" : (nFormatIdx == 6) ? L"image/tiff" : L"image/bmp";
				for (UINT j = 0; j < num; j++) {
					if (wcscmp(pCodecs[j].MimeType, mime) == 0) {
						clsid = pCodecs[j].Clsid;
						bSuccess = pBitmap->Save((const wchar_t*)sFileName, &clsid, NULL) == Gdiplus::Ok;
						break;
					}
				}
				free(pCodecs);
			}
			delete pBitmap;
			break;
		}
		case 2: { // WebP
			fptr = _tfopen(sFileName, _T("wb"));
			if (fptr) {
				size_t nSize;
				void* pOut = WebpReaderWriter::Compress(pDIB24, nWidth, nHeight, nSize, nQuality, false);
				if (pOut) {
					bSuccess = fwrite(pOut, 1, nSize, fptr) == nSize;
					WebpReaderWriter::FreeMemory(pOut);
				}
				fclose(fptr);
			}
			break;
		}
		case 3: { // JXL
			fptr = _tfopen(sFileName, _T("wb"));
			if (fptr) {
				size_t nSize;
				void* pOut = JxlReader::Compress(pDIB24, nWidth, nHeight, nSize, nQuality);
				if (pOut) {
					bSuccess = fwrite(pOut, 1, nSize, fptr) == nSize;
					free(pOut);
				}
				fclose(fptr);
			}
			break;
		}
		case 4: { // AVIF
			fptr = _tfopen(sFileName, _T("wb"));
			if (fptr) {
				size_t nSize;
				void* pOut = AvifReader::Compress(pDIB24, nWidth, nHeight, nSize, nQuality);
				if (pOut) {
					bSuccess = fwrite(pOut, 1, nSize, fptr) == nSize;
					free(pOut);
				}
				fclose(fptr);
			}
			break;
		}
		case 5: { // HEIF
			fptr = _tfopen(sFileName, _T("wb"));
			if (fptr) {
				size_t nSize;
				void* pOut = HeifReader::Compress(pDIB24, nWidth, nHeight, nSize, nQuality);
				if (pOut) {
					bSuccess = fwrite(pOut, 1, nSize, fptr) == nSize;
					free(pOut);
				}
				fclose(fptr);
			}
			break;
		}
		case 8: { // QOI
			fptr = _tfopen(sFileName, _T("wb"));
			if (fptr) {
				int nSize;
				void* pOut = QoiReaderWriter::Compress(pDIB24, nWidth, nHeight, nSize);
				if (pOut) {
					bSuccess = fwrite(pOut, 1, nSize, fptr) == (size_t)nSize;
					QoiReaderWriter::FreeMemory(pOut);
				}
				fclose(fptr);
			}
			break;
		}
		case 9: { // TGA
			fptr = _tfopen(sFileName, _T("wb"));
			if (fptr) {
				unsigned char header[18] = { 0 };
				header[2] = 2;
				header[12] = nWidth & 0xFF;
				header[13] = (nWidth >> 8) & 0xFF;
				header[14] = nHeight & 0xFF;
				header[15] = (nHeight >> 8) & 0xFF;
				header[16] = 24;
				header[17] = 0x20;
				if (fwrite(header, 1, 18, fptr) == 18) {
					bSuccess = true;
					for (int y = 0; y < nHeight; y++) {
						unsigned char* pRow = (unsigned char*)pDIB24 + y * nRowPadded;
						if (fwrite(pRow, 1, nWidth * 3, fptr) != (size_t)(nWidth * 3)) { bSuccess = false; break; }
					}
				}
				fclose(fptr);
			}
			break;
		}
	}

	delete[] pDIB24;
	if (!bSuccess) _tunlink(sFileName);
	return bSuccess;
}

LRESULT CBatchConvertDlg::OnConvert(WORD, WORD, HWND, BOOL&) {
	if (m_bConverting) return 0;

	int nFormatIdx = m_cbFormat.GetCurSel();
	if (nFormatIdx < 0 || nFormatIdx >= g_nNumFormats) return 0;

	int nQuality = CSettingsProvider::This().BatchConvertQuality();
	if (FormatHasQuality(nFormatIdx)) {
		CString sQuality;
		m_edtQuality.GetWindowText(sQuality);
		nQuality = _ttoi(sQuality);
		if (nQuality < 1) nQuality = 1;
		if (nQuality > 100) nQuality = 100;
	}

	CString sDestDir;
	m_edtDest.GetWindowText(sDestDir);
	sDestDir.Trim();
	if (sDestDir.IsEmpty()) {
		sDestDir = m_fileList.CurrentDirectory();
	}
	if (sDestDir.Right(1) != _T("\\")) sDestDir += _T("\\");

	bool bKeepOriginal = (m_btnKeepOrig.GetCheck() == BST_CHECKED);

	int nMaxWidth = 0, nMaxHeight = 0;
	bool bResize = (m_btnResize.GetCheck() == BST_CHECKED);
	if (bResize) {
		CString sW, sH;
		m_edtMaxWidth.GetWindowText(sW);
		m_edtMaxHeight.GetWindowText(sH);
		nMaxWidth = _ttoi(sW);
		nMaxHeight = _ttoi(sH);
	}

	int nSelected = 0;
	for (int i = 0; i < m_nNumFiles; i++) {
		if (ListView_GetCheckState(m_lvFiles.m_hWnd, i)) nSelected++;
	}
	if (nSelected == 0) {
		::MessageBox(m_hWnd, CNLS::GetString(_T("No files selected for conversion.")),
			CNLS::GetString(_T("Batch Convert")), MB_OK | MB_ICONWARNING);
		return 0;
	}

	CString sConfirm;
	sConfirm.Format(CNLS::GetString(_T("Convert %d files to %s format?")), nSelected, g_szFormats[nFormatIdx]);
	if (::MessageBox(m_hWnd, sConfirm, CNLS::GetString(_T("Batch Convert")), MB_YESNO | MB_ICONQUESTION) != IDYES) {
		return 0;
	}

	m_bConverting = true;
	m_btnConvert.EnableWindow(FALSE);
	m_btnCancel.EnableWindow(FALSE);
	m_btnSelectAll.EnableWindow(FALSE);
	m_btnSelectNone.EnableWindow(FALSE);
	m_progress.SetRange(0, nSelected);
	m_progress.SetPos(0);

	int nConverted = 0;
	int nFailed = 0;
	int nProgress = 0;

	const std::list<CFileDesc>& fileList = m_fileList.GetFileList();
	int nItem = 0;
	for (std::list<CFileDesc>::const_iterator iter = fileList.begin(); iter != fileList.end(); iter++, nItem++) {
		if (!ListView_GetCheckState(m_lvFiles.m_hWnd, nItem)) continue;

		CString sInputFile = iter->GetName();
		CString sStatus;
		sStatus.Format(CNLS::GetString(_T("Converting: %s")), iter->GetTitle());
		m_lblStatus.SetWindowText(sStatus);

		int nWidth, nHeight, nChannels;
		void* pPixels = LoadImageToBGRA(sInputFile, nWidth, nHeight, nChannels);
		if (pPixels == NULL) {
			nFailed++;
			nProgress++;
			m_progress.SetPos(nProgress);
			continue;
		}

		if (bResize) {
			pPixels = ResizeBGRA(pPixels, nWidth, nHeight, nMaxWidth, nMaxHeight);
		}

		CString sOutputFile = GetOutputPath(sDestDir, sInputFile, g_szExtensions[nFormatIdx], bKeepOriginal);

		if (SaveBGRAToFile(sOutputFile, pPixels, nWidth, nHeight, nFormatIdx, nQuality)) {
			nConverted++;
			CThumbnailCache::This().Invalidate(sOutputFile);
		} else {
			nFailed++;
		}

		delete[](uint8*)pPixels;

		nProgress++;
		m_progress.SetPos(nProgress);
	}

	m_bConverting = false;
	m_btnConvert.EnableWindow(TRUE);
	m_btnCancel.EnableWindow(TRUE);
	m_btnSelectAll.EnableWindow(TRUE);
	m_btnSelectNone.EnableWindow(TRUE);

	CString sDone;
	sDone.Format(CNLS::GetString(_T("Done. %d files converted, %d failed.")), nConverted, nFailed);
	m_lblStatus.SetWindowText(sDone);

	::MessageBox(m_hWnd, sDone, CNLS::GetString(_T("Batch Convert")), MB_OK | MB_ICONINFORMATION);

	return 0;
}