#include "stdafx.h"
#include "EXRWrapper.h"
#include "MaxImageDef.h"
#include "Helpers.h"
#include <openexr.h>
#include <memory>
#include <cstring>
#include <cmath>

// OpenEXR reader using the C API (libopenexr 3.x).
// Reads RGBA half/float channels via a memory-backed stream and tone-maps to 8-bit BGRA.

struct ExrMemStream {
	const uint8_t* data;
	uint64_t size;
};

static int64_t exrReadCb(exr_const_context_t ctxt, void* userdata, void* buffer,
	uint64_t sz, uint64_t offset, exr_stream_error_func_ptr_t error_cb) {
	ExrMemStream* s = (ExrMemStream*)userdata;
	if (offset + sz > s->size) {
		if (error_cb) error_cb(ctxt, EXR_ERR_READ_IO, "read past end of buffer");
		return -1;
	}
	memcpy(buffer, s->data + offset, (size_t)sz);
	return (int64_t)sz;
}

static int64_t exrSizeCb(exr_const_context_t ctxt, void* userdata) {
	ExrMemStream* s = (ExrMemStream*)userdata;
	return (int64_t)s->size;
}

static inline unsigned char tonemap(float v) {
	// Reinhard tone mapping + sRGB gamma
	float mapped = v / (1.0f + v);
	float gamma = powf(mapped, 1.0f / 2.2f);
	int iv = (int)(gamma * 255.0f + 0.5f);
	if (iv < 0) iv = 0; if (iv > 255) iv = 255;
	return (unsigned char)iv;
}

void* ExrReader::ReadImage(int& width, int& height, int& bpp, bool& outOfMemory,
	const void* buffer, int sizebytes)
{
	outOfMemory = false;
	bpp = 4;
	width = 0;
	height = 0;

	if (sizebytes <= 0) return NULL;

	exr_context_t ctx = NULL;
	ExrMemStream stream = { (const uint8_t*)buffer, (uint64_t)sizebytes };

	exr_context_initializer_t cinit = EXR_DEFAULT_CONTEXT_INITIALIZER;
	cinit.read_fn = exrReadCb;
	cinit.size_fn = exrSizeCb;
	cinit.user_data = &stream;

	exr_result_t rv = exr_start_read(&ctx, "memory.exr", &cinit);
	if (rv != EXR_ERR_SUCCESS) return NULL;

	exr_attr_box2i_t dw;
	rv = exr_get_data_window(ctx, 0, &dw);
	if (rv != EXR_ERR_SUCCESS) {
		exr_finish(&ctx);
		return NULL;
	}
	int w = dw.max.x - dw.min.x + 1;
	int h = dw.max.y - dw.min.y + 1;
	if (w <= 0 || h <= 0 || w > (int)MAX_IMAGE_DIMENSION || h > (int)MAX_IMAGE_DIMENSION) {
		exr_finish(&ctx);
		return NULL;
	}
	if ((double)w * h > MAX_IMAGE_PIXELS) {
		outOfMemory = true;
		exr_finish(&ctx);
		return NULL;
	}

	// Allocate per-channel float buffers
	float* pR = new(std::nothrow) float[(size_t)w * h];
	float* pG = new(std::nothrow) float[(size_t)w * h];
	float* pB = new(std::nothrow) float[(size_t)w * h];
	float* pA = new(std::nothrow) float[(size_t)w * h];
	if (!pR || !pG || !pB || !pA) {
		delete[] pR; delete[] pG; delete[] pB; delete[] pA;
		outOfMemory = true;
		exr_finish(&ctx);
		return NULL;
	}
	for (int i = 0; i < w * h; i++) pA[i] = 1.0f;

	// Initialize decode pipeline
	exr_chunk_info_t chunkInfo;
	memset(&chunkInfo, 0, sizeof(chunkInfo));

	rv = exr_read_scanline_chunk_info(ctx, 0, dw.min.y, &chunkInfo);
	if (rv != EXR_ERR_SUCCESS) {
		delete[] pR; delete[] pG; delete[] pB; delete[] pA;
		exr_finish(&ctx);
		return NULL;
	}

	exr_decode_pipeline_t decode;
	memset(&decode, 0, sizeof(decode));

	rv = exr_decoding_initialize(ctx, 0, &chunkInfo, &decode);
	if (rv != EXR_ERR_SUCCESS) {
		delete[] pR; delete[] pG; delete[] pB; delete[] pA;
		exr_finish(&ctx);
		return NULL;
	}

	// Detect grayscale (Y channel, no R/G/B)
	bool hasR = false, hasG = false, hasB = false, hasY = false;
	for (int c = 0; c < decode.channel_count; c++) {
		const char* name = decode.channels[c].channel_name;
		if (strcmp(name, "R") == 0) hasR = true;
		else if (strcmp(name, "G") == 0) hasG = true;
		else if (strcmp(name, "B") == 0) hasB = true;
		else if (strcmp(name, "Y") == 0) hasY = true;
	}
	bool isGray = hasY && !hasR && !hasG && !hasB;

	// Set up channel decode targets: request 32-bit float output
	for (int c = 0; c < decode.channel_count; c++) {
		exr_coding_channel_info_t* chan = &decode.channels[c];
		const char* name = chan->channel_name;
		float* base = NULL;
		if (isGray && strcmp(name, "Y") == 0) base = pR;
		else if (strcmp(name, "R") == 0) base = pR;
		else if (strcmp(name, "G") == 0) base = pG;
		else if (strcmp(name, "B") == 0) base = pB;
		else if (strcmp(name, "A") == 0) base = pA;
		if (base) {
			chan->decode_to_ptr = (uint8_t*)base;
			chan->user_data_type = EXR_PIXEL_FLOAT;
			chan->user_bytes_per_element = 4;
		}
	}

	rv = exr_decoding_choose_default_routines(ctx, 0, &decode);
	if (rv != EXR_ERR_SUCCESS) {
		exr_decoding_destroy(ctx, &decode);
		delete[] pR; delete[] pG; delete[] pB; delete[] pA;
		exr_finish(&ctx);
		return NULL;
	}

	// Read each scanline
	for (int y = 0; y < h; y++) {
		int32_t line = dw.min.y + y;
		rv = exr_read_scanline_chunk_info(ctx, 0, line, &chunkInfo);
		if (rv != EXR_ERR_SUCCESS) continue;

		// Update decode targets to point at the correct row offset
		for (int c = 0; c < decode.channel_count; c++) {
			exr_coding_channel_info_t* chan = &decode.channels[c];
			const char* name = chan->channel_name;
			float* base = NULL;
			if (isGray && strcmp(name, "Y") == 0) base = pR;
			else if (strcmp(name, "R") == 0) base = pR;
			else if (strcmp(name, "G") == 0) base = pG;
			else if (strcmp(name, "B") == 0) base = pB;
			else if (strcmp(name, "A") == 0) base = pA;
			if (base) chan->decode_to_ptr = (uint8_t*)(base + y * w);
		}

		rv = exr_decoding_run(ctx, 0, &decode);
		if (rv != EXR_ERR_SUCCESS) break;
	}

	if (isGray) {
		for (int i = 0; i < w * h; i++) { pG[i] = pR[i]; pB[i] = pR[i]; }
	}

	unsigned char* pPixelData = new(std::nothrow) unsigned char[(size_t)w * h * 4];
	if (pPixelData == NULL) {
		outOfMemory = true;
		exr_decoding_destroy(ctx, &decode);
		delete[] pR; delete[] pG; delete[] pB; delete[] pA;
		exr_finish(&ctx);
		return NULL;
	}

	for (int i = 0; i < w * h; i++) {
		pPixelData[i * 4 + 0] = tonemap(pB[i]);
		pPixelData[i * 4 + 1] = tonemap(pG[i]);
		pPixelData[i * 4 + 2] = tonemap(pR[i]);
		pPixelData[i * 4 + 3] = 255;
	}

	exr_decoding_destroy(ctx, &decode);
	delete[] pR; delete[] pG; delete[] pB; delete[] pA;
	exr_finish(&ctx);

	width = w;
	height = h;
	return (void*)pPixelData;
}

