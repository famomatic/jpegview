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
	std::lock_guard<std::mutex> lock(m_csLock);
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
	std::lock_guard<std::mutex> lock(m_csLock);
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
	std::lock_guard<std::mutex> lock(m_csLock);
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

// --- JSON recipe import/export --------------------------------------------
// Uses a minimal hand-rolled JSON writer/reader rather than pulling in a JSON
// dependency. The format is stable and round-trips through GetPreset/SetPreset.

static CString JsonEscape(LPCTSTR s) {
	CString out;
	for (const TCHAR* p = s; *p; p++) {
		if (*p == _T('"') || *p == _T('\\')) { out += _T('\\'); out += *p; }
		else if (*p == _T('\n')) out += _T("\\n");
		else if (*p == _T('\r')) out += _T("\\r");
		else if (*p == _T('\t')) out += _T("\\t");
		else out += *p;
	}
	return out;
}

bool CProcessingPresets::ExportPreset(LPCTSTR sName, LPCTSTR sJsonPath) {
	if (sName == NULL || *sName == 0 || sJsonPath == NULL || *sJsonPath == 0) return false;
	CImageProcessingParams params;
	EProcessingFlags eFlags = PFLAG_None;
	CRotationParams rotation(0);
	if (!GetPreset(sName, params, eFlags, rotation)) return false;

	CString s;
	s.Format(_T("{\r\n  \"name\": \"%s\",\r\n"), JsonEscape(sName).GetString());
	s += _T("  \"format\": \"jpegview-preset\",\r\n");
	s.AppendFormat(_T("  \"version\": 1,\r\n"));
	s.AppendFormat(_T("  \"Contrast\": %.6f,\r\n"), params.Contrast);
	s.AppendFormat(_T("  \"Gamma\": %.6f,\r\n"), params.Gamma);
	s.AppendFormat(_T("  \"Saturation\": %.6f,\r\n"), params.Saturation);
	s.AppendFormat(_T("  \"Sharpen\": %.6f,\r\n"), params.Sharpen);
	s.AppendFormat(_T("  \"ColorCorrectionFactor\": %.6f,\r\n"), params.ColorCorrectionFactor);
	s.AppendFormat(_T("  \"ContrastCorrectionFactor\": %.6f,\r\n"), params.ContrastCorrectionFactor);
	s.AppendFormat(_T("  \"LightenShadows\": %.6f,\r\n"), params.LightenShadows);
	s.AppendFormat(_T("  \"DarkenHighlights\": %.6f,\r\n"), params.DarkenHighlights);
	s.AppendFormat(_T("  \"LightenShadowSteepness\": %.6f,\r\n"), params.LightenShadowSteepness);
	s.AppendFormat(_T("  \"CyanRed\": %.6f,\r\n"), params.CyanRed);
	s.AppendFormat(_T("  \"MagentaGreen\": %.6f,\r\n"), params.MagentaGreen);
	s.AppendFormat(_T("  \"YellowBlue\": %.6f,\r\n"), params.YellowBlue);
	s.AppendFormat(_T("  \"ProcFlags\": %d,\r\n"), (int)eFlags);
	s.AppendFormat(_T("  \"Rotation\": %d,\r\n"), rotation.Rotation);
	s.AppendFormat(_T("  \"FreeRotation\": %.6f,\r\n"), rotation.FreeRotation);
	s.AppendFormat(_T("  \"RotationFlags\": %d\r\n"), (int)rotation.Flags);
	s += _T("}\r\n");

	FILE* fp = NULL;
	if (_tfopen_s(&fp, sJsonPath, _T("wb")) != 0 || fp == NULL) return false;
	// Write UTF-8 with BOM so Notepad and other editors detect encoding.
	unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
	fwrite(bom, 1, 3, fp);
	CT2CA conv(s, CP_UTF8);
	fwrite(conv, 1, strlen(conv), fp);
	fclose(fp);
	return true;
}

// Minimal JSON value extractor: finds "key": <number> and returns the value.
// Returns dDefault if the key is not found.
static double JsonGetDouble(LPCTSTR sJson, LPCTSTR sKey, double dDefault) {
	CString sFind;
	sFind.Format(_T("\"%s\":"), sKey);
	int nPos = CString(sJson).Find(sFind);
	if (nPos < 0) return dDefault;
	const TCHAR* p = sJson + nPos + sFind.GetLength();
	while (*p == _T(' ') || *p == _T('\t')) p++;
	return _tcstod(p, NULL);
}

static int JsonGetInt(LPCTSTR sJson, LPCTSTR sKey, int nDefault) {
	return (int)(JsonGetDouble(sJson, sKey, (double)nDefault) + 0.5);
}

// Extracts a quoted string value for a key. Returns empty on failure.
static CString JsonGetString(LPCTSTR sJson, LPCTSTR sKey) {
	CString sFind;
	sFind.Format(_T("\"%s\": \""), sKey);
	int nPos = CString(sJson).Find(sFind);
	if (nPos < 0) return CString();
	const TCHAR* p = sJson + nPos + sFind.GetLength();
	CString out;
	while (*p && *p != _T('"')) {
		if (*p == _T('\\') && *(p + 1)) {
			p++;
			if (*p == _T('n')) out += _T('\n');
			else if (*p == _T('r')) out += _T('\r');
			else if (*p == _T('t')) out += _T('\t');
			else out += *p;
		} else {
			out += *p;
		}
		p++;
	}
	return out;
}

bool CProcessingPresets::ImportPreset(LPCTSTR sJsonPath, LPCTSTR sOverrideName) {
	if (sJsonPath == NULL || *sJsonPath == 0) return false;
	FILE* fp = NULL;
	if (_tfopen_s(&fp, sJsonPath, _T("rb")) != 0 || fp == NULL) return false;
	fseek(fp, 0, SEEK_END);
	long nLen = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (nLen <= 0) { fclose(fp); return false; }
	char* pBuf = new char[nLen + 1];
	size_t nRead = fread(pBuf, 1, nLen, fp);
	fclose(fp);
	pBuf[nRead] = 0;
	// Skip UTF-8 BOM if present.
	const char* pStart = pBuf;
	if (nRead >= 3 && (unsigned char)pBuf[0] == 0xEF && (unsigned char)pBuf[1] == 0xBB && (unsigned char)pBuf[2] == 0xBF) {
		pStart = pBuf + 3;
	}
	CString sJson(CA2CT(pStart, CP_UTF8));
	delete[] pBuf;

	CString sName = (sOverrideName != NULL && *sOverrideName != 0) ? CString(sOverrideName) : JsonGetString(sJson, _T("name"));
	if (sName.IsEmpty()) return false;

	CImageProcessingParams params;
	params.Contrast = JsonGetDouble(sJson, _T("Contrast"), -1.0);
	params.Gamma = JsonGetDouble(sJson, _T("Gamma"), -1.0);
	params.Saturation = JsonGetDouble(sJson, _T("Saturation"), -1.0);
	params.Sharpen = JsonGetDouble(sJson, _T("Sharpen"), -1.0);
	params.ColorCorrectionFactor = JsonGetDouble(sJson, _T("ColorCorrectionFactor"), -1.0);
	params.ContrastCorrectionFactor = JsonGetDouble(sJson, _T("ContrastCorrectionFactor"), -1.0);
	params.LightenShadows = JsonGetDouble(sJson, _T("LightenShadows"), -1.0);
	params.DarkenHighlights = JsonGetDouble(sJson, _T("DarkenHighlights"), -1.0);
	params.LightenShadowSteepness = JsonGetDouble(sJson, _T("LightenShadowSteepness"), -1.0);
	params.CyanRed = JsonGetDouble(sJson, _T("CyanRed"), -1.0);
	params.MagentaGreen = JsonGetDouble(sJson, _T("MagentaGreen"), -1.0);
	params.YellowBlue = JsonGetDouble(sJson, _T("YellowBlue"), -1.0);

	EProcessingFlags eFlags = (EProcessingFlags)JsonGetInt(sJson, _T("ProcFlags"), 0);
	CRotationParams rotation(0);
	rotation.Rotation = JsonGetInt(sJson, _T("Rotation"), 0);
	rotation.FreeRotation = JsonGetDouble(sJson, _T("FreeRotation"), 0.0);
	rotation.Flags = (ERotationFlags)JsonGetInt(sJson, _T("RotationFlags"), 0);

	return SetPreset(sName, params, eFlags, rotation);
}