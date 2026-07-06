#include "stdafx.h"
#include "JP2Wrapper.h"
#include "MaxImageDef.h"
#include "Helpers.h"
#include <openjpeg.h>

// JPEG 2000 reader using OpenJPEG.
// Supports .jp2, .j2k, .j2c, .jpx containers and codestreams.

// Memory stream for OpenJPEG
struct MemStream {
	const OPJ_UINT8* data;
	OPJ_SIZE_T size;
	OPJ_SIZE_T pos;
};

static OPJ_SIZE_T memRead(void* buf, OPJ_SIZE_T count, void* userdata) {
	MemStream* s = (MemStream*)userdata;
	OPJ_SIZE_T remaining = s->size - s->pos;
	if (remaining == 0) return (OPJ_SIZE_T)-1;
	OPJ_SIZE_T toRead = (count < remaining) ? count : remaining;
	memcpy(buf, s->data + s->pos, toRead);
	s->pos += toRead;
	return toRead;
}

static OPJ_BOOL memSeek(OPJ_OFF_T offset, void* userdata) {
	MemStream* s = (MemStream*)userdata;
	if (offset < 0 || (OPJ_SIZE_T)offset > s->size) return OPJ_FALSE;
	s->pos = (OPJ_SIZE_T)offset;
	return OPJ_TRUE;
}

static OPJ_OFF_T memSkip(OPJ_OFF_T offset, void* userdata) {
	MemStream* s = (MemStream*)userdata;
	if (offset < 0) {
		OPJ_SIZE_T back = (OPJ_SIZE_T)(-offset);
		if (back > s->pos) return (OPJ_OFF_T)-1;
		s->pos -= back;
	} else {
		OPJ_SIZE_T newpos = s->pos + (OPJ_SIZE_T)offset;
		if (newpos > s->size) return (OPJ_OFF_T)-1;
		s->pos = newpos;
	}
	return offset;
}

void* Jp2Reader::ReadImage(int& width, int& height, int& bpp, bool& outOfMemory,
	const void* buffer, int sizebytes)
{
	outOfMemory = false;
	bpp = 4;
	width = 0;
	height = 0;

	if (sizebytes <= 0) return NULL;

	opj_stream_t* pStream = opj_stream_default_create(OPJ_TRUE);
	if (!pStream) return NULL;

	MemStream mem = { (const OPJ_UINT8*)buffer, (OPJ_SIZE_T)sizebytes, 0 };
	opj_stream_set_read_function(pStream, memRead);
	opj_stream_set_skip_function(pStream, memSkip);
	opj_stream_set_seek_function(pStream, memSeek);
	opj_stream_set_user_data(pStream, &mem, NULL);
	opj_stream_set_user_data_length(pStream, (OPJ_UINT64)sizebytes);

	// Detect codec: JP2 container starts with signature 00 00 00 0C 6A 50 20 20
	OPJ_CODEC_FORMAT format = OPJ_CODEC_J2K;
	if (sizebytes >= 12 && memcmp(buffer, "\x00\x00\x00\x0C\x6A\x50\x20\x20", 8) == 0) {
		format = OPJ_CODEC_JP2;
	}

	opj_codec_t* pCodec = opj_create_decompress(format);
	if (!pCodec) { opj_stream_destroy(pStream); return NULL; }

	opj_dparameters_t params;
	opj_set_default_decoder_parameters(&params);
	if (!opj_setup_decoder(pCodec, &params)) {
		opj_destroy_codec(pCodec);
		opj_stream_destroy(pStream);
		return NULL;
	}

	opj_image_t* pImage = NULL;
	if (!opj_read_header(pStream, pCodec, &pImage)) {
		opj_destroy_codec(pCodec);
		opj_stream_destroy(pStream);
		return NULL;
	}

	OPJ_UINT32 w = pImage->x1 - pImage->x0;
	OPJ_UINT32 h = pImage->y1 - pImage->y0;
	if (w == 0 || h == 0 || w > MAX_IMAGE_DIMENSION || h > MAX_IMAGE_DIMENSION) {
		opj_image_destroy(pImage);
		opj_destroy_codec(pCodec);
		opj_stream_destroy(pStream);
		return NULL;
	}
	if ((double)w * h > MAX_IMAGE_PIXELS) {
		outOfMemory = true;
		opj_image_destroy(pImage);
		opj_destroy_codec(pCodec);
		opj_stream_destroy(pStream);
		return NULL;
	}

	if (!opj_decode(pCodec, pStream, pImage)) {
		opj_image_destroy(pImage);
		opj_destroy_codec(pCodec);
		opj_stream_destroy(pStream);
		return NULL;
	}
	opj_end_decompress(pCodec, pStream);

	// Convert to BGRA
	unsigned char* pPixelData = new(std::nothrow) unsigned char[(size_t)w * h * 4];
	if (pPixelData == NULL) {
		outOfMemory = true;
		opj_image_destroy(pImage);
		opj_destroy_codec(pCodec);
		opj_stream_destroy(pStream);
		return NULL;
	}

	int numComps = pImage->numcomps;
	OPJ_INT32* comps[4] = { NULL, NULL, NULL, NULL };
	int compIndices[4] = { 0, 1, 2, -1 }; // R, G, B, A
	int hasAlpha = 0;

	// Find R, G, B, A components by their color space mapping
	if (numComps >= 3) {
		comps[0] = pImage->comps[0].data; // R
		comps[1] = pImage->comps[1].data; // G
		comps[2] = pImage->comps[2].data; // B
		if (numComps >= 4 && pImage->comps[3].alpha) {
			comps[3] = pImage->comps[3].data;
			hasAlpha = 1;
		}
	} else if (numComps == 1) {
		// Grayscale
		comps[0] = comps[1] = comps[2] = pImage->comps[0].data;
	} else {
		// 2 components: gray + alpha
		comps[0] = comps[1] = comps[2] = pImage->comps[0].data;
		if (pImage->comps[1].alpha) {
			comps[3] = pImage->comps[1].data;
			hasAlpha = 1;
		}
	}

	int bpp0 = pImage->comps[0].bpp;
	int shift = (bpp0 > 8) ? (bpp0 - 8) : 0;

	for (OPJ_UINT32 i = 0; i < w * h; i++) {
		OPJ_INT32 r = comps[0] ? (comps[0][i] >> shift) : 0;
		OPJ_INT32 g = comps[1] ? (comps[1][i] >> shift) : 0;
		OPJ_INT32 b = comps[2] ? (comps[2][i] >> shift) : 0;
		OPJ_INT32 a = hasAlpha ? (comps[3][i] >> shift) : 255;
		if (r < 0) r = 0; if (r > 255) r = 255;
		if (g < 0) g = 0; if (g > 255) g = 255;
		if (b < 0) b = 0; if (b > 255) b = 255;
		if (a < 0) a = 0; if (a > 255) a = 255;
		pPixelData[i * 4 + 0] = (unsigned char)b;
		pPixelData[i * 4 + 1] = (unsigned char)g;
		pPixelData[i * 4 + 2] = (unsigned char)r;
		pPixelData[i * 4 + 3] = (unsigned char)a;
	}

	width = w;
	height = h;

	opj_image_destroy(pImage);
	opj_destroy_codec(pCodec);
	opj_stream_destroy(pStream);

	return (void*)pPixelData;
}
