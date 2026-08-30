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
#include "SVGWrapper.h"
#include "PSDWrapper.h"
#include "DDSWrapper.h"
#include "JP2Wrapper.h"
#include "EXRWrapper.h"
#include "HDRWrapper.h"
#include "JXRWrapper.h"
#include "ReaderBMP.h"
#include "ReaderTGA.h"
#include "EXIFReader.h"
#include "ThumbnailCache.h"
#include <gdiplus.h>
#include <shlobj.h>
#include <memory>
#include <vector>

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

static bool CheckedImageBytes(int nWidth, int nHeight, int nChannels, size_t& nBytes) {
	if (nWidth <= 0 || nHeight <= 0 || nChannels <= 0) return false;
	const size_t width = static_cast<size_t>(nWidth);
	const size_t height = static_cast<size_t>(nHeight);
	const size_t channels = static_cast<size_t>(nChannels);
	if (width > SIZE_MAX / channels || width * channels > SIZE_MAX / height) return false;
	nBytes = width * height * channels;
	return true;
}

static std::unique_ptr<uint8[]> CopyPixelsToBGRA(const void* pPixels, int nWidth, int nHeight, int nBPP) {
	if (pPixels == NULL || (nBPP != 3 && nBPP != 4)) return nullptr;
	size_t nOutputBytes = 0;
	if (!CheckedImageBytes(nWidth, nHeight, 4, nOutputBytes)) return nullptr;
	auto pBGRA = std::unique_ptr<uint8[]>(new(std::nothrow) uint8[nOutputBytes]);
	if (!pBGRA) return nullptr;

	const size_t nSourceStride = nBPP == 3
		? (static_cast<size_t>(nWidth) * 3 + 3) & ~static_cast<size_t>(3)
		: static_cast<size_t>(nWidth) * 4;
	const auto* pSource = static_cast<const uint8*>(pPixels);
	for (int y = 0; y < nHeight; ++y) {
		const uint8* pSrc = pSource + static_cast<size_t>(y) * nSourceStride;
		uint8* pDst = pBGRA.get() + static_cast<size_t>(y) * nWidth * 4;
		for (int x = 0; x < nWidth; ++x) {
			pDst[x * 4] = pSrc[x * nBPP];
			pDst[x * 4 + 1] = pSrc[x * nBPP + 1];
			pDst[x * 4 + 2] = pSrc[x * nBPP + 2];
			pDst[x * 4 + 3] = nBPP == 4 ? pSrc[x * 4 + 3] : 255;
		}
	}
	return pBGRA;
}

static std::unique_ptr<uint8[]> CopyJPEGImageToBGRA(CJPEGImage* pImage, int& nWidth, int& nHeight) {
	if (pImage == NULL) return nullptr;
	nWidth = pImage->OrigWidth();
	nHeight = pImage->OrigHeight();
	return CopyPixelsToBGRA(pImage->OriginalPixels(), nWidth, nHeight, pImage->OriginalChannels());
}

static std::unique_ptr<uint8[]> LoadWithGDIPlus(LPCTSTR sFileName, int& nWidth, int& nHeight) {
	std::unique_ptr<Gdiplus::Bitmap> source(Gdiplus::Bitmap::FromFile(sFileName));
	if (!source || source->GetLastStatus() != Gdiplus::Ok || source->GetWidth() == 0 || source->GetHeight() == 0 ||
		source->GetWidth() > INT_MAX || source->GetHeight() > INT_MAX) return nullptr;
	nWidth = static_cast<int>(source->GetWidth());
	nHeight = static_cast<int>(source->GetHeight());
	size_t nBytes = 0;
	if (!CheckedImageBytes(nWidth, nHeight, 4, nBytes)) return nullptr;

	Gdiplus::Bitmap converted(nWidth, nHeight, PixelFormat32bppARGB);
	Gdiplus::Graphics graphics(&converted);
	if (graphics.DrawImage(source.get(), 0, 0, nWidth, nHeight) != Gdiplus::Ok) return nullptr;
	Gdiplus::BitmapData data{};
	Gdiplus::Rect rect(0, 0, nWidth, nHeight);
	if (converted.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok) return nullptr;
	auto pixels = std::unique_ptr<uint8[]>(new(std::nothrow) uint8[nBytes]);
	if (pixels) {
		for (int y = 0; y < nHeight; ++y) {
			const int sourceY = data.Stride >= 0 ? y : nHeight - 1 - y;
			const uint8* row = static_cast<const uint8*>(data.Scan0) + static_cast<ptrdiff_t>(sourceY) * abs(data.Stride);
			memcpy(pixels.get() + static_cast<size_t>(y) * nWidth * 4, row, static_cast<size_t>(nWidth) * 4);
		}
	}
	converted.UnlockBits(&data);
	return pixels;
}

// Load an image file into an owned 32bpp BGRA buffer.
static std::unique_ptr<uint8[]> LoadImageToBGRA(LPCTSTR sFileName, int& nWidth, int& nHeight, int& nChannels) {
	nWidth = nHeight = 0;
	nChannels = 4;
	EImageFormat eFormat = Helpers::GetImageFormat(sFileName);

	// Read file into memory
	HANDLE hFile = ::CreateFile(sFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return nullptr;
	LARGE_INTEGER fileSize{};
	if (!::GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0 || fileSize.QuadPart > 500LL * 1024 * 1024 || fileSize.QuadPart > INT_MAX) {
		::CloseHandle(hFile);
		return nullptr;
	}
	const int nFileSize = static_cast<int>(fileSize.QuadPart);
	std::vector<uint8> buffer;
	try { buffer.resize(static_cast<size_t>(nFileSize)); }
	catch (const std::bad_alloc&) { ::CloseHandle(hFile); return nullptr; }
	if (buffer.empty()) {
		::CloseHandle(hFile);
		return nullptr;
	}
	DWORD nRead = 0;
	BOOL bRead = ::ReadFile(hFile, buffer.data(), static_cast<DWORD>(nFileSize), &nRead, NULL);
	::CloseHandle(hFile);
	if (!bRead || nRead != static_cast<DWORD>(nFileSize)) return nullptr;

	void* pPixels = NULL;
	bool bPixelsAllocatedWithMalloc = false;
	bool bOutOfMemory = false;
	int nBPP = 0;
	bool bHasAnimation = false;
	int nFrameCount = 0, nFrameTime = 0;
	void* pEXIF = NULL;

	switch (eFormat) {
		case IF_JPEG:
		case IF_JPEG_Embedded: {
			TJSAMP chromoSubsampling;
			pPixels = TurboJpeg::ReadImage(nWidth, nHeight, nBPP, chromoSubsampling, bOutOfMemory, buffer.data(), nFileSize);
			break;
		}
		case IF_WEBP:
			pPixels = WebpReaderWriter::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTime, pEXIF, bOutOfMemory, buffer.data(), nFileSize);
			WebpReaderWriter::DeleteCache();
			break;
		case IF_PNG:
			pPixels = PngReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTime, pEXIF, bOutOfMemory, buffer.data(), nFileSize);
			PngReader::DeleteCache();
			bPixelsAllocatedWithMalloc = true;
			break;
		case IF_JXL:
			pPixels = JxlReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, nFrameCount, nFrameTime, pEXIF, bOutOfMemory, buffer.data(), nFileSize);
			JxlReader::DeleteCache();
			break;
		case IF_AVIF:
			pPixels = AvifReader::ReadImage(nWidth, nHeight, nBPP, bHasAnimation, 0, nFrameCount, nFrameTime, pEXIF, bOutOfMemory, buffer.data(), nFileSize);
			AvifReader::DeleteCache();
			break;
		case IF_HEIF:
			pPixels = HeifReader::ReadImage(nWidth, nHeight, nBPP, nFrameCount, pEXIF, bOutOfMemory, 0, buffer.data(), nFileSize);
			break;
		case IF_QOI:
			pPixels = QoiReaderWriter::ReadImage(nWidth, nHeight, nBPP, bOutOfMemory, buffer.data(), nFileSize);
			break;
		case IF_DDS:
			pPixels = DdsReader::ReadImage(nWidth, nHeight, nBPP, bOutOfMemory, buffer.data(), nFileSize);
			break;
		case IF_JP2:
			pPixels = Jp2Reader::ReadImage(nWidth, nHeight, nBPP, bOutOfMemory, buffer.data(), nFileSize);
			break;
		case IF_EXR:
			pPixels = ExrReader::ReadImage(nWidth, nHeight, nBPP, bOutOfMemory, buffer.data(), nFileSize);
			break;
		case IF_HDR:
			pPixels = HdrReader::ReadImage(nWidth, nHeight, nBPP, bOutOfMemory, buffer.data(), nFileSize);
			break;
		case IF_JXR:
			pPixels = JxrReader::ReadImage(nWidth, nHeight, nBPP, bOutOfMemory, buffer.data(), nFileSize);
			break;
		case IF_SVG:
			pPixels = SvgReader::ReadImage(nWidth, nHeight, nBPP, bOutOfMemory, sFileName);
			break;
		case IF_WindowsBMP:
		case IF_TGA:
		case IF_TIFF:
		case IF_PSD: {
			std::unique_ptr<CJPEGImage> image;
			if (eFormat == IF_WindowsBMP) image.reset(CReaderBMP::ReadBmpImage(sFileName, bOutOfMemory));
			else if (eFormat == IF_TGA) image.reset(CReaderTGA::ReadTgaImage(sFileName, 0, bOutOfMemory));
			else if (eFormat == IF_TIFF) image.reset(TiffReader::ReadImage(sFileName, 0, bOutOfMemory));
			else image.reset(PsdReader::ReadImage(sFileName, bOutOfMemory));
			if (pEXIF) free(pEXIF);
			return CopyJPEGImageToBGRA(image.get(), nWidth, nHeight);
		}
		default: {
			if (pEXIF) free(pEXIF);
			return LoadWithGDIPlus(sFileName, nWidth, nHeight);
		}
	}

	if (pEXIF) free(pEXIF);
	if (pPixels == NULL) return nullptr;
	auto result = CopyPixelsToBGRA(pPixels, nWidth, nHeight, nBPP);
	if (bPixelsAllocatedWithMalloc) free(pPixels);
	else delete[] static_cast<uint8*>(pPixels);
	return result;
}

// Resize BGRA buffer to fit within max dimensions, preserving aspect ratio.
static std::unique_ptr<uint8[]> ResizeBGRA(std::unique_ptr<uint8[]> pPixels, int& nWidth, int& nHeight, int nMaxWidth, int nMaxHeight) {
	if (!pPixels || (nMaxWidth <= 0 && nMaxHeight <= 0)) return pPixels;
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

	size_t nBytes = 0;
	if (!CheckedImageBytes(nNewW, nNewH, 4, nBytes)) return pPixels;
	auto pResized = std::unique_ptr<uint8[]>(new(std::nothrow) uint8[nBytes]);
	if (!pResized) return pPixels;

	uint8* pSrc = pPixels.get();
	uint8* pDst = pResized.get();
	for (int y = 0; y < nNewH; y++) {
		double srcY = (double)y * nHeight / nNewH;
		int y0 = (int)srcY;
		int y1 = min(y0 + 1, nHeight - 1);
		double fy = srcY - y0;
		const uint8* sourceRow0 = pSrc + static_cast<size_t>(y0) * nWidth * 4;
		const uint8* sourceRow1 = pSrc + static_cast<size_t>(y1) * nWidth * 4;
		uint8* destinationRow = pDst + static_cast<size_t>(y) * nNewW * 4;
		for (int x = 0; x < nNewW; x++) {
			double srcX = (double)x * nWidth / nNewW;
			int x0 = (int)srcX;
			int x1 = min(x0 + 1, nWidth - 1);
			double fx = srcX - x0;
			for (int c = 0; c < 4; c++) {
				double v00 = sourceRow0[static_cast<size_t>(x0) * 4 + c];
				double v01 = sourceRow0[static_cast<size_t>(x1) * 4 + c];
				double v10 = sourceRow1[static_cast<size_t>(x0) * 4 + c];
				double v11 = sourceRow1[static_cast<size_t>(x1) * 4 + c];
				double v = v00 * (1 - fx) * (1 - fy) + v01 * fx * (1 - fy) +
					v10 * (1 - fx) * fy + v11 * fx * fy;
				destinationRow[static_cast<size_t>(x) * 4 + c] = (uint8)(v + 0.5);
			}
		}
	}

	nWidth = nNewW;
	nHeight = nNewH;
	return pResized;
}

static CString MakeUniqueSiblingPath(LPCTSTR sFileName) {
	static volatile LONG sequence = 0;
	for (int attempt = 0; attempt < 100; ++attempt) {
		CString candidate;
		candidate.Format(_T("%s.jvtmp.%lu.%ld"), sFileName, ::GetCurrentProcessId(), ::InterlockedIncrement(&sequence));
		if (::GetFileAttributes(candidate) == INVALID_FILE_ATTRIBUTES) return candidate;
	}
	return CString();
}

static bool CommitTemporaryFile(const CString& sTempFile, LPCTSTR sFileName) {
	if (sTempFile.IsEmpty()) return false;
	HANDLE tempHandle = ::CreateFile(sTempFile, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (tempHandle == INVALID_HANDLE_VALUE) { ::DeleteFile(sTempFile); return false; }
	const bool flushed = ::FlushFileBuffers(tempHandle) != FALSE;
	::CloseHandle(tempHandle);
	if (!flushed) { ::DeleteFile(sTempFile); return false; }
	if (::GetFileAttributes(sFileName) != INVALID_FILE_ATTRIBUTES) {
		if (::ReplaceFile(sFileName, sTempFile, NULL, REPLACEFILE_WRITE_THROUGH, NULL, NULL)) return true;
	} else if (::MoveFileEx(sTempFile, sFileName, MOVEFILE_WRITE_THROUGH)) {
		return true;
	}
	::DeleteFile(sTempFile);
	return false;
}

// Save BGRA buffer to a sibling temporary file, then atomically commit it.
static bool SaveBGRAToFile(LPCTSTR sFileName, const void* pBGRA, int nWidth, int nHeight, int nFormatIdx, int nQuality) {
	if (pBGRA == NULL || nWidth <= 0 || nHeight <= 0 || nWidth > INT_MAX / 4) return false;
	const size_t nRowPadded = (static_cast<size_t>(nWidth) * 3 + 3) & ~static_cast<size_t>(3);
	if (nRowPadded > SIZE_MAX / static_cast<size_t>(nHeight)) return false;
	const size_t nSizeBytes = nRowPadded * static_cast<size_t>(nHeight);
	auto pDIB24 = std::unique_ptr<char[]>(new(std::nothrow) char[nSizeBytes]);
	if (!pDIB24) return false;
	const CString sTempFile = MakeUniqueSiblingPath(sFileName);
	if (sTempFile.IsEmpty()) return false;

	const uint8* pSrc = static_cast<const uint8*>(pBGRA);
	for (int y = 0; y < nHeight; y++) {
		const uint8* pS = pSrc + static_cast<size_t>(y) * nWidth * 4;
		char* pD = pDIB24.get() + static_cast<size_t>(y) * nRowPadded;
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
			unsigned char* pStream = (unsigned char*)TurboJpeg::Compress(pDIB24.get(), nWidth, nHeight, nStreamLen, bOOM, nQuality);
			if (pStream) {
				fptr = _tfopen(sTempFile, _T("wb"));
				if (fptr) {
					bSuccess = fwrite(pStream, 1, nStreamLen, fptr) == (size_t)nStreamLen;
					fclose(fptr);
				}
				TurboJpeg::Free(pStream);
			}
			break;
		}
		case 1: case 6: case 7: { // PNG / TIFF / BMP via GDI+
			const bool supportsAlpha = nFormatIdx == 1 || nFormatIdx == 6;
			const INT stride = supportsAlpha ? nWidth * 4 : static_cast<INT>(nRowPadded);
			BYTE* pixels = supportsAlpha ? const_cast<BYTE*>(static_cast<const BYTE*>(pBGRA)) : reinterpret_cast<BYTE*>(pDIB24.get());
			const INT pixelFormat = supportsAlpha ? PixelFormat32bppARGB : PixelFormat24bppRGB;
			Gdiplus::Bitmap* pBitmap = new Gdiplus::Bitmap(nWidth, nHeight, stride, pixelFormat, pixels);
			if (pBitmap != NULL && pBitmap->GetLastStatus() == Gdiplus::Ok) {
				CLSID clsid;
				UINT num = 0, size = 0;
				Gdiplus::GetImageEncodersSize(&num, &size);
				Gdiplus::ImageCodecInfo* pCodecs = size > 0 ? static_cast<Gdiplus::ImageCodecInfo*>(malloc(size)) : NULL;
				if (pCodecs != NULL) Gdiplus::GetImageEncoders(num, size, pCodecs);
				const wchar_t* mime = (nFormatIdx == 1) ? L"image/png" : (nFormatIdx == 6) ? L"image/tiff" : L"image/bmp";
				for (UINT j = 0; pCodecs != NULL && j < num; j++) {
					if (wcscmp(pCodecs[j].MimeType, mime) == 0) {
						clsid = pCodecs[j].Clsid;
						bSuccess = pBitmap->Save((const wchar_t*)sTempFile.GetString(), &clsid, NULL) == Gdiplus::Ok;
						break;
					}
				}
				free(pCodecs);
			}
			delete pBitmap;
			break;
		}
		case 2: { // WebP
			fptr = _tfopen(sTempFile, _T("wb"));
			if (fptr) {
				size_t nSize;
				void* pOut = WebpReaderWriter::CompressBGRA(pBGRA, nWidth, nHeight, nSize, nQuality, false);
				if (pOut) {
					bSuccess = fwrite(pOut, 1, nSize, fptr) == nSize;
					WebpReaderWriter::FreeMemory(pOut);
				}
				fclose(fptr);
			}
			break;
		}
		case 3: { // JXL
			fptr = _tfopen(sTempFile, _T("wb"));
			if (fptr) {
				size_t nSize;
				void* pOut = JxlReader::Compress(pBGRA, nWidth, nHeight, nSize, nQuality, 4);
				if (pOut) {
					bSuccess = fwrite(pOut, 1, nSize, fptr) == nSize;
					free(pOut);
				}
				fclose(fptr);
			}
			break;
		}
		case 4: { // AVIF
			fptr = _tfopen(sTempFile, _T("wb"));
			if (fptr) {
				size_t nSize;
				void* pOut = AvifReader::Compress(pBGRA, nWidth, nHeight, nSize, nQuality, 4);
				if (pOut) {
					bSuccess = fwrite(pOut, 1, nSize, fptr) == nSize;
					free(pOut);
				}
				fclose(fptr);
			}
			break;
		}
		case 5: { // HEIF
			fptr = _tfopen(sTempFile, _T("wb"));
			if (fptr) {
				size_t nSize;
				void* pOut = HeifReader::Compress(pBGRA, nWidth, nHeight, nSize, nQuality, 4);
				if (pOut) {
					bSuccess = fwrite(pOut, 1, nSize, fptr) == nSize;
					free(pOut);
				}
				fclose(fptr);
			}
			break;
		}
		case 8: { // QOI
			fptr = _tfopen(sTempFile, _T("wb"));
			if (fptr) {
				int nSize;
				void* pOut = QoiReaderWriter::Compress(pBGRA, nWidth, nHeight, nSize, 4);
				if (pOut) {
					bSuccess = fwrite(pOut, 1, nSize, fptr) == (size_t)nSize;
					QoiReaderWriter::FreeMemory(pOut);
				}
				fclose(fptr);
			}
			break;
		}
		case 9: { // TGA
			fptr = _tfopen(sTempFile, _T("wb"));
			if (fptr) {
				unsigned char header[18] = { 0 };
				header[2] = 2;
				header[12] = nWidth & 0xFF;
				header[13] = (nWidth >> 8) & 0xFF;
				header[14] = nHeight & 0xFF;
				header[15] = (nHeight >> 8) & 0xFF;
				header[16] = 32;
				header[17] = 0x28; // top-left origin, 8 alpha bits
				if (fwrite(header, 1, 18, fptr) == 18) {
					bSuccess = true;
					for (int y = 0; y < nHeight; y++) {
						const unsigned char* pRow = static_cast<const unsigned char*>(pBGRA) + static_cast<size_t>(y) * nWidth * 4;
						if (fwrite(pRow, 1, static_cast<size_t>(nWidth) * 4, fptr) != static_cast<size_t>(nWidth) * 4) { bSuccess = false; break; }
					}
				}
				fclose(fptr);
			}
			break;
		}
	}

	if (!bSuccess) {
		::DeleteFile(sTempFile);
		return false;
	}
	return CommitTemporaryFile(sTempFile, sFileName);
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
		auto pPixels = LoadImageToBGRA(sInputFile, nWidth, nHeight, nChannels);
		if (!pPixels) {
			nFailed++;
			nProgress++;
			m_progress.SetPos(nProgress);
			continue;
		}

		if (bResize) {
			pPixels = ResizeBGRA(std::move(pPixels), nWidth, nHeight, nMaxWidth, nMaxHeight);
		}

		CString sOutputFile = GetOutputPath(sDestDir, sInputFile, g_szExtensions[nFormatIdx], bKeepOriginal);

		if (SaveBGRAToFile(sOutputFile, pPixels.get(), nWidth, nHeight, nFormatIdx, nQuality)) {
			nConverted++;
			CThumbnailCache::This().Invalidate(sOutputFile);
		} else {
			nFailed++;
		}

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
