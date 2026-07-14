#pragma once

// Presents a linear float RGBA image in HDR (scRGB, FP16) on an HDR-enabled monitor.
// Creates a borderless child window covering the parent's client area with its own
// D3D11 flip-model swap chain in DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709.
// Used as an explicit HDR preview mode for EXR / Radiance HDR images.
class CHDRDisplay
{
public:
	// Returns if the monitor the given window is displayed on currently runs in HDR mode
	static bool IsHDRAvailable(HWND hWnd);

	// Shows the given image (interleaved linear RGBA float, Rec.709 primaries, 4 floats
	// per pixel) fitted into the client area of the parent window. The pixel data is
	// uploaded to the GPU and does not need to stay alive after the call.
	// Replaces the currently shown HDR image if already active. Returns false on failure.
	static bool Show(HWND hWndParent, const float* pPixelsRGBA, int nWidth, int nHeight);

	// Destroys the HDR overlay window and all GPU resources
	static void Hide();

	// Returns if the HDR overlay is currently shown
	static bool IsActive();

private:
	CHDRDisplay();
};
