#pragma once

#include "ProcessParams.h"
#include <mutex>
#include <list>

// Named image-processing presets, persisted to an INI file next to the
// JPEGView settings. A preset captures the full set of tunable processing
// parameters (contrast, gamma, sharpen, color balance, LDC, ...) plus the
// processing flags and rotation, so a look can be saved once and re-applied
// to any image or folder. This is distinct from the per-image ParameterDB,
// which keys on pixel hash; presets are named and apply on demand.
class CProcessingPresets
{
public:
	// Singleton access.
	static CProcessingPresets& This();

	// Returns the names of all stored presets, in storage order.
	const std::list<CString> GetPresetNames() const {
		std::lock_guard<std::mutex> lock(m_csLock);
		return m_names;
	}

	// Loads the processing parameters + flags + rotation for the named preset.
	// Returns false if the preset does not exist.
	bool GetPreset(LPCTSTR sName, CImageProcessingParams& params, EProcessingFlags& eFlags, CRotationParams& rotation);

	// Stores the given parameters as a preset, overwriting any existing preset
	// with the same name. Returns true on success.
	bool SetPreset(LPCTSTR sName, const CImageProcessingParams& params, EProcessingFlags eFlags, const CRotationParams& rotation);

	// Removes a preset by name. Returns true if it existed.
	bool DeletePreset(LPCTSTR sName);

	// Exports the named preset to a JSON file. Returns true on success.
	// The JSON includes all processing params, flags, and rotation so a
	// look can be shared between machines or archived.
	bool ExportPreset(LPCTSTR sName, LPCTSTR sJsonPath);

	// Imports a preset from a JSON file created by ExportPreset().
	// The preset name embedded in the JSON is used unless sOverrideName is
	// non-empty. Returns true on success, false if the file is invalid or
	// unreadable.
	bool ImportPreset(LPCTSTR sJsonPath, LPCTSTR sOverrideName);

	// Returns true if the named preset exists.
	bool HasPreset(LPCTSTR sName) {
		std::lock_guard<std::mutex> lock(m_csLock);
		for (const auto& n : m_names) if (n.CompareNoCase(sName) == 0) return true;
		return false;
	}

private:
	CProcessingPresets();
	CProcessingPresets(const CProcessingPresets&);
	CProcessingPresets& operator=(const CProcessingPresets&);

	// Returns the full path to the presets INI file, creating the directory
	// on first call if needed.
	LPCTSTR GetIniPath() const;

	// Reloads the list of preset names from the INI file.
	void RefreshNames();

	mutable CString m_sIniPath;
	std::list<CString> m_names;
	// Guards m_names and the INI file writes; presets can be touched from the
	// UI thread while a background load touches the image processing params.
	mutable std::mutex m_csLock;
};
