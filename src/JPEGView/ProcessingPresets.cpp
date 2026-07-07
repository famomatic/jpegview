#include "stdafx.h"
#include "ProcessingPresets.h"
#include "Helpers.h"
#include <string>

// INI section name under which preset groups are stored.
static const TCHAR* kSectionPrefix = _T("Preset ");
// Special index section listing all preset names, one per line.
static const TCHAR* kNamesSection = _T("PresetNames");
static const TCHAR* kNamesKey = _T("Names");

CProcessingPresets& CProcessingPresets::This() {
	static CProcessingPresets instance;
	return instance;
}

CProcessingPresets::CProcessingPresets() {
	RefreshNames();
}

// GetPrivateProfileString returns a string; parse it as a double since there
// is no GetPrivateProfileDouble in the Win32 API.
static double GetProfileDouble(LPCTSTR sSection, LPCTSTR sKey, double dDefault, LPCTSTR sPath) {
	TCHAR szVal[64];
	::GetPrivateProfileString(sSection, sKey, NULL, szVal, 64, sPath);
	if (szVal[0] == 0) return dDefault;
	return _tcstod(szVal, NULL);
}

LPCTSTR CProcessingPresets::GetIniPath() const {
	if (m_sIniPath.IsEmpty()) {
		m_sIniPath = CString(Helpers::JPEGViewAppDataPath()) + _T("ProcessingPresets.ini");
	}
	return m_sIniPath;
}

void CProcessingPresets::RefreshNames() {
	m_names.clear();
	TCHAR szNames[8192] = _T("");
	::GetPrivateProfileString(kNamesSection, kNamesKey, _T(""), szNames, 8192, GetIniPath());
	// Names are newline-separated.
	TCHAR* p = szNames;
	while (*p != 0) {
		TCHAR* pEnd = p;
		while (*pEnd != 0 && *pEnd != _T('\n')) pEnd++;
		TCHAR cSaved = *pEnd;
		*pEnd = 0;
		if (*p != 0) {
			m_names.push_back(CString(p));
		}
		*pEnd = cSaved;
		p = (*pEnd == 0) ? pEnd : pEnd + 1;
	}
}

static CString SectionFor(LPCTSTR sName) {
	return CString(kSectionPrefix) + sName;
}

bool CProcessingPresets::GetPreset(LPCTSTR sName, CImageProcessingParams& params, EProcessingFlags& eFlags, CRotationParams& rotation) {
	if (sName == NULL || *sName == 0) return false;
	CString sSection = SectionFor(sName);
	LPCTSTR sPath = GetIniPath();

	// If the section does not exist, the preset is missing.
	TCHAR szDummy[2] = _T("");
	if (::GetPrivateProfileString(sSection, _T("Contrast"), NULL, szDummy, 1, sPath) == 0) {
		return false;
	}

	params.Contrast = GetProfileDouble(sSection, _T("Contrast"), -1.0, sPath);
	params.Gamma = GetProfileDouble(sSection, _T("Gamma"), -1.0, sPath);
	params.Saturation = GetProfileDouble(sSection, _T("Saturation"), -1.0, sPath);
	params.Sharpen = GetProfileDouble(sSection, _T("Sharpen"), -1.0, sPath);
	params.ColorCorrectionFactor = GetProfileDouble(sSection, _T("ColorCorrectionFactor"), -1.0, sPath);
	params.ContrastCorrectionFactor = GetProfileDouble(sSection, _T("ContrastCorrectionFactor"), -1.0, sPath);
	params.LightenShadows = GetProfileDouble(sSection, _T("LightenShadows"), -1.0, sPath);
	params.DarkenHighlights = GetProfileDouble(sSection, _T("DarkenHighlights"), -1.0, sPath);
	params.LightenShadowSteepness = GetProfileDouble(sSection, _T("LightenShadowSteepness"), -1.0, sPath);
	params.CyanRed = GetProfileDouble(sSection, _T("CyanRed"), -1.0, sPath);
	params.MagentaGreen = GetProfileDouble(sSection, _T("MagentaGreen"), -1.0, sPath);
	params.YellowBlue = GetProfileDouble(sSection, _T("YellowBlue"), -1.0, sPath);

	int nFlags = ::GetPrivateProfileInt(sSection, _T("ProcFlags"), 0, sPath);
	eFlags = (EProcessingFlags)nFlags;
	rotation.Rotation = ::GetPrivateProfileInt(sSection, _T("Rotation"), 0, sPath);
	rotation.FreeRotation = GetProfileDouble(sSection, _T("FreeRotation"), 0.0, sPath);
	int nRotFlags = ::GetPrivateProfileInt(sSection, _T("RotationFlags"), 0, sPath);
	rotation.Flags = (ERotationFlags)nRotFlags;

	return true;
}

static void WriteDouble(LPCTSTR sSection, LPCTSTR sKey, double dValue, LPCTSTR sPath) {
	TCHAR szVal[32];
	_stprintf_s(szVal, _T("%.6f"), dValue);
	::WritePrivateProfileString(sSection, sKey, szVal, sPath);
}

bool CProcessingPresets::SetPreset(LPCTSTR sName, const CImageProcessingParams& params, EProcessingFlags eFlags, const CRotationParams& rotation) {
	if (sName == NULL || *sName == 0) return false;
	CString sSection = SectionFor(sName);
	LPCTSTR sPath = GetIniPath();

	WriteDouble(sSection, _T("Contrast"), params.Contrast, sPath);
	WriteDouble(sSection, _T("Gamma"), params.Gamma, sPath);
	WriteDouble(sSection, _T("Saturation"), params.Saturation, sPath);
	WriteDouble(sSection, _T("Sharpen"), params.Sharpen, sPath);
	WriteDouble(sSection, _T("ColorCorrectionFactor"), params.ColorCorrectionFactor, sPath);
	WriteDouble(sSection, _T("ContrastCorrectionFactor"), params.ContrastCorrectionFactor, sPath);
	WriteDouble(sSection, _T("LightenShadows"), params.LightenShadows, sPath);
	WriteDouble(sSection, _T("DarkenHighlights"), params.DarkenHighlights, sPath);
	WriteDouble(sSection, _T("LightenShadowSteepness"), params.LightenShadowSteepness, sPath);
	WriteDouble(sSection, _T("CyanRed"), params.CyanRed, sPath);
	WriteDouble(sSection, _T("MagentaGreen"), params.MagentaGreen, sPath);
	WriteDouble(sSection, _T("YellowBlue"), params.YellowBlue, sPath);

	TCHAR szFlags[16];
	_stprintf_s(szFlags, _T("%d"), (int)eFlags);
	::WritePrivateProfileString(sSection, _T("ProcFlags"), szFlags, sPath);
	_stprintf_s(szFlags, _T("%d"), rotation.Rotation);
	::WritePrivateProfileString(sSection, _T("Rotation"), szFlags, sPath);
	WriteDouble(sSection, _T("FreeRotation"), rotation.FreeRotation, sPath);
	_stprintf_s(szFlags, _T("%d"), (int)rotation.Flags);
	::WritePrivateProfileString(sSection, _T("RotationFlags"), szFlags, sPath);

	// Add to the names list if not already present.
	bool bExists = false;
	for (std::list<CString>::iterator it = m_names.begin(); it != m_names.end(); ++it) {
		if (it->CompareNoCase(sName) == 0) { bExists = true; break; }
	}
	if (!bExists) {
		m_names.push_back(CString(sName));
	}

	// Persist the names list.
	CString sNames;
	for (std::list<CString>::iterator it = m_names.begin(); it != m_names.end(); ++it) {
		if (!sNames.IsEmpty()) sNames += _T("\n");
		sNames += *it;
	}
	::WritePrivateProfileString(kNamesSection, kNamesKey, sNames, sPath);
	return true;
}

bool CProcessingPresets::DeletePreset(LPCTSTR sName) {
	if (sName == NULL || *sName == 0) return false;
	CString sSection = SectionFor(sName);
	LPCTSTR sPath = GetIniPath();
	BOOL bOk = ::WritePrivateProfileString(sSection, NULL, NULL, sPath);

	// Remove from the names list.
	for (std::list<CString>::iterator it = m_names.begin(); it != m_names.end(); ++it) {
		if (it->CompareNoCase(sName) == 0) { m_names.erase(it); break; }
	}
	CString sNames;
	for (std::list<CString>::iterator it = m_names.begin(); it != m_names.end(); ++it) {
		if (!sNames.IsEmpty()) sNames += _T("\n");
		sNames += *it;
	}
	::WritePrivateProfileString(kNamesSection, kNamesKey, sNames, sPath);
	return bOk != FALSE;
}
