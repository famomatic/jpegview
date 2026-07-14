#pragma once

// Provides a cached lcms2 color transform from sRGB to the ICC profile of the
// monitor the main window is currently displayed on. Used to color manage the
// final rendered DIB when the 'UseDisplayColorProfile' INI setting is enabled.
class CDisplayColorProfile
{
public:
	// Sets the window used to determine the current monitor. Must be called once after
	// the main window has been created, before GetTransform() is used.
	static void SetWindow(HWND hWnd);

	// Returns the cached BGRA -> BGRA transform (sRGB to monitor profile) or NULL if
	// display color management is disabled, no valid monitor profile is installed or
	// the monitor profile is sRGB. The returned handle is owned by this class.
	// The transform is recreated automatically when the window moves to another monitor.
	static void* GetTransform();

	// Version number, incremented whenever the returned transform changes identity.
	// Callers caching pixels transformed with GetTransform() must recompute when it changes.
	static int GetVersion();

	// Invalidates the cached transform, e.g. on WM_DISPLAYCHANGE when the monitor
	// profile may have been changed.
	static void Invalidate();

	// Applies the given transform in-place (or source to target) on 32 bpp BGRA pixels.
	// Returns false if transform is NULL.
	static bool ApplyTransform(void* hTransform, const void* pSource, void* pTarget, int nWidth, int nHeight);

private:
	CDisplayColorProfile();
};
