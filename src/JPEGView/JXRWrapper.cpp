#include "stdafx.h"
#include "JXRWrapper.h"
#include "MaxImageDef.h"
#include "Helpers.h"
#include <JXRGlue.h>
#include <memory>

// JPEG XR reader using jxrlib (C API).
// Uses CreateWS_Memory to wrap the in-memory buffer in a WMPStream.

void* JxrReader::ReadImage(int& width, int& height, int& bpp, bool& outOfMemory,
	const void* buffer, int sizebytes)
{
	outOfMemory = false;
	bpp = 4;
	width = 0;
	height = 0;

	if (sizebytes <= 0) return NULL;

	PKImageDecode* pDecoder = NULL;
	WMPStream* pStream = NULL;
	void* pPixelData = NULL;

	// Create a memory-backed stream. CreateWS_Memory copies the data internally.
	ERR err = CreateWS_Memory(&pStream, (void*)buffer, sizebytes);
	if (err != WMP_errSuccess || pStream == NULL) return NULL;

	err = PKImageDecode_Create_WMP(&pDecoder);
	if (err != WMP_errSuccess) { CloseWS_Memory(&pStream); return NULL; }

	err = PKImageDecode_Initialize(pDecoder, pStream);
	if (err != WMP_errSuccess) goto cleanup;

	{
		I32 w32 = 0, h32 = 0;
		err = PKImageDecode_GetSize(pDecoder, &w32, &h32);
		if (err != WMP_errSuccess) goto cleanup;
		int w = (int)w32;
		int h = (int)h32;
		if (w <= 0 || h <= 0 || w > (int)MAX_IMAGE_DIMENSION || h > (int)MAX_IMAGE_DIMENSION) goto cleanup;
		if ((double)w * h > MAX_IMAGE_PIXELS) { outOfMemory = true; goto cleanup; }

		pPixelData = new(std::nothrow) unsigned char[(size_t)w * h * 4];
		if (pPixelData == NULL) { outOfMemory = true; goto cleanup; }

		// Copy decodes the full image into the given buffer at the requested stride.
		// jxrlib outputs in the decoder's native format; we request 32bpp BGR.
		unsigned int stride = w * 4;
		err = PKImageDecode_Copy(pDecoder, NULL, (U8*)pPixelData, stride);
		if (err != WMP_errSuccess) { delete[] (unsigned char*)pPixelData; pPixelData = NULL; goto cleanup; }
		// Set alpha to 255 (BGRX -> BGRA)
		for (int i = 0; i < w * h; i++) ((unsigned char*)pPixelData)[i * 4 + 3] = 255;

		width = w;
		height = h;
	}

cleanup:
	if (pDecoder) PKImageDecode_Release(&pDecoder);
	if (pStream) CloseWS_Memory(&pStream);
	return pPixelData;
}
