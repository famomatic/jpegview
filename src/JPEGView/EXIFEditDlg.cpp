#include "stdafx.h"
#include "EXIFEditDlg.h"
#include "Helpers.h"
#include "NLS.h"
#include "EXIFReader.h"
#include "JPEGImage.h"
#include "SettingsProvider.h"
#include <shlobj.h>

CEXIFEditDlg::CEXIFEditDlg(LPCTSTR sFileName) {
	m_sFileName = sFileName;
	m_bHasEXIF = false;
	m_bHasGPS = false;
	m_nOrientation = 1;
}

CEXIFEditDlg::~CEXIFEditDlg() {
}

LRESULT CEXIFEditDlg::OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
	CenterWindow(GetParent());

	m_btnGPS.Attach(GetDlgItem(IDC_EE_GPS));
	m_cbOrientation.Attach(GetDlgItem(IDC_EE_ORIENTATION));
	m_btnApply.Attach(GetDlgItem(IDC_EE_APPLY));
	m_btnCancel.Attach(GetDlgItem(IDC_EE_CANCEL));

	// Populate orientation combo
	m_cbOrientation.AddString(_T("1 - Normal"));
	m_cbOrientation.AddString(_T("2 - Mirror horizontal"));
	m_cbOrientation.AddString(_T("3 - Rotate 180"));
	m_cbOrientation.AddString(_T("4 - Mirror vertical"));
	m_cbOrientation.AddString(_T("5 - Mirror horizontal + rotate 270"));
	m_cbOrientation.AddString(_T("6 - Rotate 90"));
	m_cbOrientation.AddString(_T("7 - Mirror horizontal + rotate 90"));
	m_cbOrientation.AddString(_T("8 - Rotate 270"));

	// Read the JPEG file and parse EXIF
	HANDLE hFile = ::CreateFile(m_sFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		::MessageBox(m_hWnd, CNLS::GetString(_T("Cannot open file.")), CNLS::GetString(_T("EXIF Edit")), MB_OK | MB_ICONERROR);
		m_btnApply.EnableWindow(FALSE);
		return 1;
	}
	DWORD nFileSize = ::GetFileSize(hFile, NULL);
	if (nFileSize == 0 || nFileSize > 100 * 1024 * 1024) {
		::CloseHandle(hFile);
		::MessageBox(m_hWnd, CNLS::GetString(_T("File too large or empty.")), CNLS::GetString(_T("EXIF Edit")), MB_OK | MB_ICONERROR);
		m_btnApply.EnableWindow(FALSE);
		return 1;
	}
	uint8* pBuffer = new uint8[nFileSize];
	DWORD nRead = 0;
	::ReadFile(hFile, pBuffer, nFileSize, &nRead, NULL);
	::CloseHandle(hFile);
	if (nRead != nFileSize) {
		delete[] pBuffer;
		m_btnApply.EnableWindow(FALSE);
		return 1;
	}

	// Find EXIF APP1 block
	uint8* pEXIF = (uint8*)Helpers::FindJPEGMarker(pBuffer, nFileSize, 0xE1);
	if (pEXIF == NULL || pEXIF + 10 > pBuffer + nFileSize) {
		delete[] pBuffer;
		::MessageBox(m_hWnd, CNLS::GetString(_T("No EXIF data found in this JPEG file.")), CNLS::GetString(_T("EXIF Edit")), MB_OK | MB_ICONWARNING);
		m_btnApply.EnableWindow(FALSE);
		return 1;
	}

	// Verify it's an EXIF block (not XMP or other APP1)
	if (memcmp(pEXIF + 4, "Exif\0\0", 6) != 0) {
		// Try to find the next APP1 that is EXIF
		uint8* pNext = pEXIF;
		while (pNext != NULL) {
			pNext = (uint8*)Helpers::FindJPEGMarker(pNext + 1, nFileSize - (pNext - pBuffer) - 1, 0xE1);
			if (pNext != NULL && pNext + 10 <= pBuffer + nFileSize && memcmp(pNext + 4, "Exif\0\0", 6) == 0) {
				pEXIF = pNext;
				break;
			}
		}
		if (pNext == NULL) {
			delete[] pBuffer;
			::MessageBox(m_hWnd, CNLS::GetString(_T("No EXIF data found in this JPEG file.")), CNLS::GetString(_T("EXIF Edit")), MB_OK | MB_ICONWARNING);
			m_btnApply.EnableWindow(FALSE);
			return 1;
		}
	}

	m_bHasEXIF = true;

	// Parse EXIF to check for GPS and orientation
	CEXIFReader exifReader(pEXIF, IF_JPEG);
	m_bHasGPS = exifReader.IsGPSInformationPresent();
	m_nOrientation = exifReader.ImageOrientationPresent() ? exifReader.GetImageOrientation() : 1;

	delete[] pBuffer;

	// Update UI
	m_btnGPS.SetCheck(m_bHasGPS ? BST_UNCHECKED : BST_UNCHECKED);
	m_btnGPS.EnableWindow(m_bHasGPS ? TRUE : FALSE);
	if (!m_bHasGPS) {
		m_btnGPS.SetWindowText(CNLS::GetString(_T("No GPS data")));
	} else {
		m_btnGPS.SetWindowText(CNLS::GetString(_T("Remove GPS data")));
	}

	m_cbOrientation.SetCurSel(m_nOrientation - 1);

	return 1;
}

LRESULT CEXIFEditDlg::OnClose(UINT, WPARAM, LPARAM, BOOL&) {
	EndDialog(IDCANCEL);
	return 0;
}

LRESULT CEXIFEditDlg::OnCancel(WORD, WORD wID, HWND, BOOL&) {
	EndDialog(wID);
	return 0;
}

LRESULT CEXIFEditDlg::OnApply(WORD, WORD, HWND, BOOL&) {
	if (!m_bHasEXIF) {
		EndDialog(IDCANCEL);
		return 0;
	}

	bool bRemoveGPS = (m_btnGPS.GetCheck() == BST_CHECKED) && m_bHasGPS;
	int nNewOrientation = m_cbOrientation.GetCurSel() + 1;
	bool bOrientationChanged = (nNewOrientation != m_nOrientation);

	if (!bRemoveGPS && !bOrientationChanged) {
		EndDialog(IDOK);
		return 0;
	}

	// Read the file
	HANDLE hFile = ::CreateFile(m_sFileName, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		::MessageBox(m_hWnd, CNLS::GetString(_T("Cannot open file for writing.")), CNLS::GetString(_T("EXIF Edit")), MB_OK | MB_ICONERROR);
		return 0;
	}
	DWORD nFileSize = ::GetFileSize(hFile, NULL);
	uint8* pBuffer = new uint8[nFileSize];
	DWORD nRead = 0;
	::ReadFile(hFile, pBuffer, nFileSize, &nRead, NULL);

	// Find EXIF APP1 block
	uint8* pEXIF = (uint8*)Helpers::FindJPEGMarker(pBuffer, nFileSize, 0xE1);
	if (pEXIF == NULL || memcmp(pEXIF + 4, "Exif\0\0", 6) != 0) {
		uint8* pNext = pEXIF;
		while (pNext != NULL) {
			pNext = (uint8*)Helpers::FindJPEGMarker(pNext + 1, nFileSize - (pNext - pBuffer) - 1, 0xE1);
			if (pNext != NULL && memcmp(pNext + 4, "Exif\0\0", 6) == 0) {
				pEXIF = pNext;
				break;
			}
		}
	}

	if (pEXIF == NULL) {
		delete[] pBuffer;
		::CloseHandle(hFile);
		::MessageBox(m_hWnd, CNLS::GetString(_T("EXIF data not found.")), CNLS::GetString(_T("EXIF Edit")), MB_OK | MB_ICONERROR);
		return 0;
	}

	// Modify EXIF in-place
	CEXIFReader exifReader(pEXIF, IF_JPEG);
	if (bRemoveGPS) {
		exifReader.RemoveGPSData();
	}
	if (bOrientationChanged) {
		exifReader.WriteImageOrientation(nNewOrientation);
	}

	// Write back
	::SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
	DWORD nWritten = 0;
	::WriteFile(hFile, pBuffer, nFileSize, &nWritten, NULL);
	::CloseHandle(hFile);
	delete[] pBuffer;

	if (nWritten == nFileSize) {
		::MessageBox(m_hWnd, CNLS::GetString(_T("EXIF data updated successfully.")), CNLS::GetString(_T("EXIF Edit")), MB_OK | MB_ICONINFORMATION);
		EndDialog(IDOK);
	} else {
		::MessageBox(m_hWnd, CNLS::GetString(_T("Failed to write file.")), CNLS::GetString(_T("EXIF Edit")), MB_OK | MB_ICONERROR);
	}

	return 0;
}