#include "stdafx.h"

#include "AVIFWrapper.h"
#include "avif/avif.h"
#include "BasicProcessing.h"
#include "MaxImageDef.h"
#include "ICCProfileTransform.h"

struct AvifReader::avif_cache {
	avifDecoder* decoder;
	avifRGBImage rgb;
	uint8_t* data;
	size_t data_size;
	void* transform;
};

thread_local AvifReader::avif_cache AvifReader::cache = { 0 };

static int GetAvifThreadCount()
{
	SYSTEM_INFO systemInfo = {};
	GetSystemInfo(&systemInfo);
	return max(1, min(16, static_cast<int>(systemInfo.dwNumberOfProcessors)));
}

void* AvifReader::ReadImage(int& width,
	int& height,
	int& nchannels,
	bool& has_animation,
	int frame_index,
	int& frame_count,
	int& frame_time,
	void*& exif_chunk,
	bool& outOfMemory,
	const void* buffer,
	int sizebytes)
{
	outOfMemory = false;
	width = height = 0;
	nchannels = 4;
	has_animation = false;
	exif_chunk = NULL;
	if (buffer == NULL || sizebytes <= 0 || frame_index < 0)
		return NULL;

	avifResult result;
	const int nthreads = GetAvifThreadCount();

	// Cache animations
	if (cache.decoder == NULL) {
		cache.data = (uint8_t*)malloc(sizebytes);
		if (cache.data == NULL) {
			outOfMemory = true;
			return NULL;
		}
		memcpy(cache.data, buffer, sizebytes);
		cache.decoder = avifDecoderCreate();
		if (cache.decoder == NULL) {
			DeleteCache();
			outOfMemory = true;
			return NULL;
		}
		cache.decoder->maxThreads = nthreads;
		cache.decoder->strictFlags = AVIF_STRICT_DISABLED;
		result = avifDecoderSetIOMemory(cache.decoder, cache.data, sizebytes);
		if (result != AVIF_RESULT_OK) {
			DeleteCache();
			return NULL;
		}
		result = avifDecoderParse(cache.decoder);
		if (result != AVIF_RESULT_OK) {
			DeleteCache();
			return NULL;
		}
		cache.rgb = { 0 };
		
		cache.data_size = sizebytes;
	}
	
	// Decode a frame
	result = avifDecoderNthImage(cache.decoder, frame_index);
	if (result != AVIF_RESULT_OK) {
		DeleteCache();
		return NULL;
	}
	avifRGBImageSetDefaults(&cache.rgb, cache.decoder->image);
	cache.rgb.depth = 8;
	cache.rgb.format = AVIF_RGB_FORMAT_BGRA;
	cache.rgb.maxThreads = nthreads;

	const uint64_t decodedWidth = cache.rgb.width;
	const uint64_t decodedHeight = cache.rgb.height;
	if (decodedWidth == 0 || decodedHeight == 0 ||
		decodedWidth > MAX_IMAGE_DIMENSION || decodedHeight > MAX_IMAGE_DIMENSION ||
		decodedWidth * decodedHeight > MAX_IMAGE_PIXELS) {
		outOfMemory = true;
		DeleteCache();
		return NULL;
	}
	width = (int)decodedWidth;
	height = (int)decodedHeight;
	has_animation = cache.decoder->imageCount > 1;
	frame_count = cache.decoder->imageCount;
	frame_time = (int)(cache.decoder->imageTiming.duration * 1000.0);

	if (static_cast<size_t>(width) > SIZE_MAX / static_cast<size_t>(height) / static_cast<size_t>(nchannels)) {
		outOfMemory = true;
		DeleteCache();
		return NULL;
	}
	size_t size = static_cast<size_t>(width) * height * nchannels;
	cache.rgb.pixels = new(std::nothrow) unsigned char[size];
	if (cache.rgb.pixels == NULL) {
		outOfMemory = true;
		return NULL;
	}
	cache.rgb.rowBytes = width * nchannels;
	result = avifImageYUVToRGB(cache.decoder->image, &cache.rgb);
	if (result != AVIF_RESULT_OK) {
		delete[] cache.rgb.pixels;
		DeleteCache();
		return NULL;
	}

	// Handle clap, irot and imir boxes
	avifTransformFlags flags = cache.decoder->image->transformFlags;
	if (flags & AVIF_TRANSFORM_CLAP) {
		avifCleanApertureBox* clap = &cache.decoder->image->clap;
		avifCropRect crop;
		avifDiagnostics diag;
		if (avifCropRectConvertCleanApertureBox(&crop, clap, width, height, cache.decoder->image->yuvFormat, &diag)) {
			POINT point = { (LONG)crop.x, (LONG)crop.y };
			SIZE sz = { (LONG)crop.width, (LONG)crop.height };
			void* pixels = CBasicProcessing::Crop32bpp(width, height, cache.rgb.pixels, CRect(point, sz));
			if (pixels != NULL) {
				delete[] cache.rgb.pixels;
				cache.rgb.pixels = (uint8_t*)pixels;
				width = crop.width;
				height = crop.height;
			}
		}
	}
	if (flags & AVIF_TRANSFORM_IROT) {
		int angle = 360 - cache.decoder->image->irot.angle * 90;
		void* pixels = CBasicProcessing::Rotate32bpp(width, height, cache.rgb.pixels, angle);
		if (pixels != NULL) {
			delete[] cache.rgb.pixels;
			cache.rgb.pixels = (uint8_t*)pixels;
			if (angle != 180) {
				int temp = width;
				width = height;
				height = temp;
			}
		}
	}
	if (flags & AVIF_TRANSFORM_IMIR) {
		void* pixels = CBasicProcessing::Mirror32bpp(width, height, cache.rgb.pixels, cache.decoder->image->imir.axis);
		if (pixels != NULL) {
			delete[] cache.rgb.pixels;
			cache.rgb.pixels = (uint8_t*)pixels;
		}
	}

	avifRWData icc = cache.decoder->image->icc;
	if (cache.transform == NULL)
		cache.transform = ICCProfileTransform::CreateTransform(icc.data, icc.size, ICCProfileTransform::FORMAT_BGRA);
	ICCProfileTransform::DoTransform(cache.transform, cache.rgb.pixels, cache.rgb.pixels, width, height);

	avifRWData exif = cache.decoder->image->exif;
	if (exif.size > 8 && exif.size < 65528 && exif.data != NULL) {
		exif_chunk = malloc(exif.size + 10);
		if (exif_chunk != NULL) {
			memcpy(exif_chunk, "\xFF\xE1\0\0Exif\0\0", 10);
			*((unsigned short*)exif_chunk + 1) = _byteswap_ushort(exif.size + 8);
			memcpy((uint8_t*)exif_chunk + 10, exif.data, exif.size);
		}
	}

	void* pPixelData = cache.rgb.pixels;
	if (!has_animation)
		DeleteCache();

	return pPixelData;
}

void AvifReader::DeleteCache() {
	if (cache.decoder)
		avifDecoderDestroy(cache.decoder);
	free(cache.data);
	ICCProfileTransform::DeleteTransform(cache.transform);
	cache = { 0 };
}

// Compress 24-bit BGR DIB (rows padded to 4-byte boundary) into AVIF.
// nQuality: 0-100. Returns malloc'd buffer (caller frees with free()).
void* AvifReader::Compress(const void* pBGRData, int nWidth, int nHeight, size_t& nSize, int nQuality, int nChannels) {
	nSize = 0;
	if (pBGRData == NULL || nWidth <= 0 || nHeight <= 0 || (nChannels != 3 && nChannels != 4)) return NULL;

	avifEncoder* encoder = avifEncoderCreate();
	if (encoder == NULL) return NULL;
	encoder->maxThreads = GetAvifThreadCount();
	encoder->quality = nQuality;

	avifImage* image = avifImageCreate(nWidth, nHeight, 8, AVIF_PIXEL_FORMAT_YUV444);
	if (image == NULL) { avifEncoderDestroy(encoder); return NULL; }
	image->colorPrimaries = AVIF_COLOR_PRIMARIES_SRGB;
	image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
	image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT601;

	// Convert BGR(A) to packed RGB(A) for libavif.
	const size_t nRowPadded = nChannels == 3 ? (static_cast<size_t>(nWidth) * 3 + 3) & ~static_cast<size_t>(3) : static_cast<size_t>(nWidth) * 4;
	if (static_cast<size_t>(nWidth) > SIZE_MAX / static_cast<size_t>(nHeight) / static_cast<size_t>(nChannels)) { avifImageDestroy(image); avifEncoderDestroy(encoder); return NULL; }
	std::vector<uint8_t> rgbBuf(static_cast<size_t>(nWidth) * nHeight * nChannels);
	for (int y = 0; y < nHeight; y++) {
		const uint8_t* src = (const uint8_t*)pBGRData + y * nRowPadded;
		uint8_t* dst = rgbBuf.data() + static_cast<size_t>(y) * nWidth * nChannels;
		for (int x = 0; x < nWidth; x++) {
			dst[x * nChannels + 0] = src[x * nChannels + 2];
			dst[x * nChannels + 1] = src[x * nChannels + 1];
			dst[x * nChannels + 2] = src[x * nChannels + 0];
			if (nChannels == 4) dst[x * 4 + 3] = src[x * 4 + 3];
		}
	}

	avifRGBImage rgb;
	memset(&rgb, 0, sizeof(rgb));
	avifRGBImageSetDefaults(&rgb, image);
	rgb.format = nChannels == 4 ? AVIF_RGB_FORMAT_RGBA : AVIF_RGB_FORMAT_RGB;
	rgb.depth = 8;
	rgb.pixels = rgbBuf.data();
	rgb.rowBytes = nWidth * nChannels;

	void* pResult = NULL;
	if (avifImageRGBToYUV(image, &rgb) == AVIF_RESULT_OK) {
		avifRWData output = AVIF_DATA_EMPTY;
		if (avifEncoderWrite(encoder, image, &output) == AVIF_RESULT_OK) {
			pResult = malloc(output.size);
			if (pResult) {
				memcpy(pResult, output.data, output.size);
				nSize = output.size;
			}
		}
		avifRWDataFree(&output);
	}

	avifImageDestroy(image);
	avifEncoderDestroy(encoder);
	return pResult;
}
