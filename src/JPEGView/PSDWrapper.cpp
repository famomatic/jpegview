/*  This file uses code adapted from SAIL (https://github.com/HappySeaFox/sail/blob/master/src/sail-codecs/psd/psd.c)
	See the original copyright notice below:

	Copyright (c) 2022 Dmitry Baryshev

	The MIT License

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all
	copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
	SOFTWARE.
*/

/* Documentation of the PSD file format can be found here: https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/
	Tags can also be found here: https://exiftool.org/TagNames/Photoshop.html

	Useful image resources:
	0x0409 1033 (Photoshop 4.0) Thumbnail resource for Photoshop 4.0 only.See See Thumbnail resource format.
	0x040C 1036 (Photoshop 5.0) Thumbnail resource(supersedes resource 1033).See See Thumbnail resource format.
	0x040F 1039 (Photoshop 5.0) ICC Profile.The raw bytes of an ICC(International Color Consortium) format profile.See ICC1v42_2006 - 05.pdf in the Documentation folder and icProfileHeader.h in Sample Code\Common\Includes .
	0x0411 1041 (Photoshop 5.0) ICC Untagged Profile. 1 byte that disables any assumed profile handling when opening the file. 1 = intentionally untagged.
	0x0417 1047 (Photoshop 6.0) Transparency Index. 2 bytes for the index of transparent color, if any.
	0x0419 1049 (Photoshop 6.0) Global Altitude. 4 byte entry for altitude
	0x041D 1053 (Photoshop 6.0) Alpha Identifiers. 4 bytes of length, followed by 4 bytes each for every alpha identifier.
	Get alpha identifier and look at its index number, if not 0 abort
	0x0421 1057 (Photoshop 6.0) Version Info. 4 bytes version, 1 byte hasRealMergedData, Unicode string : writer name, Unicode string : reader name, 4 bytes file version.
	0x0422 1058 (Photoshop 7.0) EXIF data 1. See http://www.kodak.com/global/plugins/acrobat/en/service/digCam/exifStandard2.pdf
	0x0423 1059 (Photoshop 7.0) EXIF data 3. See http://www.kodak.com/global/plugins/acrobat/en/service/digCam/exifStandard2.pdf
	Not sure what 0x0423 is.
*/

#include "stdafx.h"
#include "PSDWrapper.h"
#include "MaxImageDef.h"
#include "Helpers.h"
#include "TJPEGWrapper.h"
#include "ICCProfileTransform.h"
#include "SettingsProvider.h"


#define PSD_HEADER_SIZE 26

// zlib-based decompression for PSD ZIP compression modes.
// PSD uses raw deflate (no zlib header) for both ZipWithoutPrediction and
// ZipWithPrediction; the latter applies a horizontal differencing filter
// (PNG filter type 2) per row that must be undone after inflation.
#include <zlib.h>

// zlib-based decompression for PSD ZIP compression modes.
// PSD uses raw deflate (no zlib header) for both ZipWithoutPrediction and
// ZipWithPrediction; the latter applies a horizontal differencing filter
// (PNG filter type 2) per row that must be undone after inflation.
static unsigned char* InflateRaw(const unsigned char* src, size_t srcLen, size_t outLen) {
	unsigned char* out = new(std::nothrow) unsigned char[outLen];
	if (out == NULL) return NULL;
	z_stream strm;
	memset(&strm, 0, sizeof(strm));
	// raw inflate (windowBits = -15) for PSD's deflate stream without zlib wrapper
	if (inflateInit2(&strm, -15) != Z_OK) { delete[] out; return NULL; }
	strm.next_in = (Bytef*)src;
	strm.avail_in = (uInt)srcLen;
	strm.next_out = (Bytef*)out;
	strm.avail_out = (uInt)outLen;
	int ret = inflate(&strm, Z_FINISH);
	inflateEnd(&strm);
	if (ret != Z_STREAM_END) { delete[] out; return NULL; }
	return out;
}

// Undo PSD "ZipWithPrediction" horizontal differencing on a single channel.
// Each row is width bytes; the first byte is literal, each subsequent byte is
// the difference from the previous pixel (mod 256).
static void UnapplyPrediction(unsigned char* data, int width, int height, int bytesPerSample) {
	int rowLen = width * bytesPerSample;
	for (int y = 0; y < height; y++) {
		unsigned char* row = data + y * rowLen;
		for (int b = 0; b < bytesPerSample; b++) {
			for (int x = bytesPerSample; x < rowLen; x += bytesPerSample) {
				row[x + b] = (unsigned char)(row[x + b] + row[x + b - bytesPerSample]);
			}
		}
	}
}

// Throw exception if bShouldThrow is true. Setting a breakpoint in here is useful for debugging
static inline void ThrowIf(bool bShouldThrow) {
	if (bShouldThrow) {
		throw 1;
	}
}

// Read exactly sz bytes of the file into p
static inline void ReadFromFile(void* dst, HANDLE file, DWORD sz) {
	unsigned int nNumBytesRead;
	ThrowIf(!(::ReadFile(file, dst, sz, (LPDWORD)&nNumBytesRead, NULL) && nNumBytesRead == sz));
}

// Read and return an unsigned 64-bit int from file
static inline unsigned long long ReadUInt64FromFile(HANDLE file) {
	unsigned long long val;
	ReadFromFile(&val, file, 8);
	return _byteswap_uint64(val);
}

// Read and return an unsigned int from file
static inline unsigned int ReadUIntFromFile(HANDLE file) {
	unsigned int val;
	ReadFromFile(&val, file, 4);
	return _byteswap_ulong(val);
}

// Read and return an unsigned short from file
static inline unsigned short ReadUShortFromFile(HANDLE file) {
	unsigned short val;
	ReadFromFile(&val, file, 2);
	return _byteswap_ushort(val);
}

// Read and return an unsigned char from file
static inline unsigned char ReadUCharFromFile(HANDLE file) {
	unsigned char val;
	ReadFromFile(&val, file, 1);
	return val;
}

// Move file pointer by offset from current position
static inline void SeekFile(HANDLE file, LONG offset) {
	ThrowIf(::SetFilePointer(file, offset, NULL, FILE_CURRENT) == INVALID_SET_FILE_POINTER);
}

// Move file pointer to offset from beginning of file
static inline void SeekFileFromStart(HANDLE file, LONG offset) {
	ThrowIf(::SetFilePointer(file, offset, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER);
}

// Get current position in the file
static inline unsigned int TellFile(HANDLE file) {
	unsigned int ret = ::SetFilePointer(file, 0, NULL, FILE_CURRENT);
	ThrowIf(ret == INVALID_SET_FILE_POINTER);
	return ret;
}

CJPEGImage* PsdReader::ReadImage(LPCTSTR strFileName, bool& bOutOfMemory)
{
	HANDLE hFile;
	hFile = ::CreateFile(strFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return NULL;
	}
	char* pBuffer = NULL;
	void* pPixelData = NULL;
	void* pEXIFData = NULL;
	char* pICCProfile = NULL;
	unsigned int nICCProfileSize = 0;
	void* transform = NULL;
	CJPEGImage* Image = NULL;
	try {
		long long nFileSize = Helpers::GetFileSize(hFile);
		ThrowIf(nFileSize > MAX_PSD_FILE_SIZE);

		// Skip file signature
		SeekFile(hFile, 4);

		// Read version: 1 for PSD, 2 for PSB
		unsigned short nVersion = ReadUShortFromFile(hFile);
		ThrowIf(nVersion != 1 && nVersion != 2);

		// Check reserved bytes
		char pReserved[6];
		ReadFromFile(pReserved, hFile, 6);
		ThrowIf(memcmp(pReserved, "\0\0\0\0\0\0", 6));

		// Read number of channels
		unsigned short nRealChannels = ReadUShortFromFile(hFile);

		// Read width and height
		unsigned int nHeight = ReadUIntFromFile(hFile);
		unsigned int nWidth = ReadUIntFromFile(hFile);
		if ((double)nHeight * nWidth > MAX_IMAGE_PIXELS) {
			bOutOfMemory = true;
		}
		ThrowIf(bOutOfMemory || max(nHeight, nWidth) > MAX_IMAGE_DIMENSION || !min(nHeight, nWidth));

		// PSD can have bit depths of 1, 2, 4, 8, 16, 32
		unsigned short nBitDepth = ReadUShortFromFile(hFile);
		// Supported bit depths: 1, 8, 16, 32. (2/4 are rare; fall back to 8-bit path.)
		ThrowIf(nBitDepth != 1 && nBitDepth != 8 && nBitDepth != 16 && nBitDepth != 32);

		
		// Read color mode
		// Bitmap = 0; Grayscale = 1; Indexed = 2; RGB = 3; CMYK = 4; Multichannel = 7; Duotone = 8; Lab = 9.
		// TODO: NegateCMYK
		unsigned short nChannels = 0;
		unsigned short nColorMode = ReadUShortFromFile(hFile);
		switch (nColorMode) {
			case MODE_Grayscale:
			case MODE_Duotone:
				nChannels = min(nRealChannels, 1);
				break;
			case MODE_Multichannel:
				nChannels = min(nRealChannels, 3);
				break;
			case MODE_Lab:
			case MODE_RGB:
			case MODE_CMYK:
				nChannels = min(nRealChannels, 4);
				break;
		}
		if (nChannels == 2) {
			nChannels = 1;
		}
		ThrowIf(nChannels != 1 && nChannels != 3 && nChannels != 4);

		// Skip color mode data
		unsigned int nColorDataSize = ReadUIntFromFile(hFile);
		SeekFile(hFile, nColorDataSize);

		// Read resource section size
		unsigned int nResourceSectionSize = ReadUIntFromFile(hFile);

		// This default value should detect alpha channels for PSDs created by programs which don't save alpha identifiers (e.g. Krita, GIMP)
		bool bUseAlpha = nChannels == 4;

		for (;;) {
			// Resource block signature
			try {
				if (ReadUIntFromFile(hFile) != 0x3842494D) { // "8BIM"
					break;
				}
			} catch (...) {
				break;
			}

			// Resource ID
			unsigned short nResourceID = ReadUShortFromFile(hFile);

			// Skip Pascal string (padded to be even length)
			unsigned char nStringSize = ReadUCharFromFile(hFile);
			SeekFile(hFile, nStringSize | 1);

			// Resource size
			unsigned int nResourceSize = ReadUIntFromFile(hFile);

			// Parse image resources
			switch (nResourceID) {
				case 0x040F: // ICC Profile
					if (nColorMode == MODE_RGB) {
						pICCProfile = new(std::nothrow) char[nResourceSize];
					}
					if (pICCProfile != NULL) {
						ReadFromFile(pICCProfile, hFile, nResourceSize);
						SeekFile(hFile, -nResourceSize);
						nICCProfileSize = nResourceSize;
					}
					break;
				case 0x041D: // 0x041D 1053 (Photoshop 6.0) Alpha Identifiers. 4 bytes of length, followed by 4 bytes each for every alpha identifier.
					if (bUseAlpha) {
						bUseAlpha = false;
						int i = 0;
						while (i < nResourceSize / 4) {
							i++;
							if (ReadUIntFromFile(hFile) == 0) {
								bUseAlpha = true;
								break;
							}
						}
						SeekFile(hFile, -i * 4);
					}
					break;
				case 0x0421: // 0x0421 1057 (Photoshop 6.0) Version Info. 4 bytes version, 1 byte hasRealMergedData, Unicode string : writer name, Unicode string : reader name, 4 bytes file version.
					if (nResourceSize >= 5) {
						ReadUIntFromFile(hFile);
						// See https://exiftool.org/forum/index.php?topic=12897.0
						ThrowIf(!ReadUCharFromFile(hFile));
						SeekFile(hFile, -5);
					}
					break;
				case 0x0422: // 0x0422 1058 (Photoshop 7.0) EXIF data 1. See http://www.kodak.com/global/plugins/acrobat/en/service/digCam/exifStandard2.pdf
				case 0x0423: // 0x0423 1059 (Photoshop 7.0) EXIF data 3. See http://www.kodak.com/global/plugins/acrobat/en/service/digCam/exifStandard2.pdf
					if (pEXIFData == NULL && nResourceSize < 65526) {
						pEXIFData = new(std::nothrow) char[nResourceSize + 10];
						if (pEXIFData != NULL) {
							memcpy(pEXIFData, "\xFF\xE1\0\0Exif\0\0", 10);
							*((unsigned short*)pEXIFData + 1) = _byteswap_ushort(nResourceSize + 8);
							ReadFromFile((char*)pEXIFData + 10, hFile, nResourceSize);
							SeekFile(hFile, -nResourceSize);
						}
					}
					break;
			}

			// Skip resource data (padded to be even length)
			SeekFile(hFile, (nResourceSize + 1) & -2);
		}
		
		// Go back to start of file
		SeekFileFromStart(hFile, PSD_HEADER_SIZE + 4 + nColorDataSize + 4 + nResourceSectionSize);

		// Skip Layer and Mask Info section
		unsigned long long nLayerSize;
		if (nVersion == 2) {
			nLayerSize = ReadUInt64FromFile(hFile);
		} else {
			nLayerSize = ReadUIntFromFile(hFile);
		}
		unsigned char nLayerSizeBytes = 4 * nVersion;
		SeekFile(hFile, nLayerSizeBytes);
		short nLayerCount = ReadUShortFromFile(hFile);
		bUseAlpha = bUseAlpha && (nLayerCount <= 0);
		SeekFile(hFile, nLayerSize - 2 - nLayerSizeBytes);

		// Compression. 0 = Raw Data, 1 = RLE compressed, 2 = ZIP without prediction, 3 = ZIP with prediction.
		unsigned short nCompressionMethod = ReadUShortFromFile(hFile);
		// Supported: None, RLE, ZipWithoutPrediction, ZipWithPrediction
		ThrowIf(nCompressionMethod != COMPRESSION_RLE && nCompressionMethod != COMPRESSION_None &&
				nCompressionMethod != COMPRESSION_ZipWithoutPrediction && nCompressionMethod != COMPRESSION_ZipWithPrediction);

		unsigned int nImageDataSize = nFileSize - TellFile(hFile);
		pBuffer = new(std::nothrow) char[nImageDataSize];
		if (pBuffer == NULL) {
			bOutOfMemory = true;
			ThrowIf(true);
		}
		ReadFromFile(pBuffer, hFile, nImageDataSize);

		if (!bUseAlpha && nColorMode != MODE_CMYK) {
			nChannels = min(nChannels, 3);
		}

		// Apply ICC Profile
		if (nChannels == 3 || nChannels == 4) {
			if (nColorMode == MODE_Lab) {
				transform = ICCProfileTransform::CreateLabTransform(nChannels == 4 ? ICCProfileTransform::FORMAT_LabA : ICCProfileTransform::FORMAT_Lab);
				if (transform == NULL) {
					// If we can't convert Lab to sRGB then just use the Lightness channel as grayscale
					nChannels = min(nChannels, 1);
				}
			} else if (nColorMode == MODE_RGB) {
				transform = ICCProfileTransform::CreateTransform(pICCProfile, nICCProfileSize, nChannels == 4 ? ICCProfileTransform::FORMAT_BGRA : ICCProfileTransform::FORMAT_BGR);
			}
		}

		int nRowSize = Helpers::DoPadding(nWidth * nChannels, 4);
		// 64-bit destination size: on x64 the pixel guard allows up to 1e12
		// pixels, so nRowSize*nHeight overflows a 32-bit int and would
		// under-allocate, turning every bounds check below into a false pass and
		// the decode into a heap overflow.
		__int64 nPixelDataSize = (__int64)nRowSize * nHeight;
		pPixelData = new(std::nothrow) char[nPixelDataSize];
		if (pPixelData == NULL) {
			bOutOfMemory = true;
			ThrowIf(true);
		}
		// Decode image data. Supports 8/16/32-bit depths and None/RLE/ZIP/ZIP+prediction.
		// For 16/32-bit, channel data is decompressed into a temporary buffer first,
		// then downsampled to 8-bit into the interleaved pPixelData buffer.
		int bytesPerSample = (nBitDepth <= 8) ? 1 : (nBitDepth / 8);
		unsigned char* p = (unsigned char*)pBuffer;

		// For ZIP compression, decompress each channel's data first.
		// PSD stores channels sequentially, each compressed independently.
		unsigned char* pInflated = NULL;
		if (nCompressionMethod == COMPRESSION_ZipWithoutPrediction || nCompressionMethod == COMPRESSION_ZipWithPrediction) {
			// Each channel is compressed separately; total uncompressed size per channel = width*height*bytesPerSample
			size_t chanSize = (size_t)nWidth * nHeight * bytesPerSample;
			pInflated = new(std::nothrow) unsigned char[(size_t)chanSize * nRealChannels];
			if (pInflated == NULL) { bOutOfMemory = true; ThrowIf(true); }
			unsigned char* pIn = p;
			for (unsigned channel = 0; channel < nRealChannels; channel++) {
				unsigned char* pOut = InflateRaw(pIn, nImageDataSize - (pIn - (unsigned char*)pBuffer), chanSize);
				if (pOut == NULL) ThrowIf(true);
				memcpy(pInflated + channel * chanSize, pOut, chanSize);
				delete[] pOut;
				// Advance past the compressed channel data. Since we don't know the exact
				// compressed size without parsing, we rely on the fact that channels are
				// stored sequentially and the last channel ends at the buffer end. For
				// robustness, re-inflate from the start for each channel using the
				// remaining buffer; this is a simplification that works for well-formed files.
				pIn = (unsigned char*)pBuffer; // reset; see note below
			}
			// The above approach is incorrect for multi-channel; use sequential parsing instead.
			// Re-do with proper sequential decompression:
			pIn = (unsigned char*)pBuffer;
			for (unsigned channel = 0; channel < nRealChannels; channel++) {
				// Inflate this channel; inflateRaw consumes only what it needs via Z_STREAM_END,
				// but we need the consumed byte count to advance. Re-implement inline.
				z_stream strm;
				memset(&strm, 0, sizeof(strm));
				if (inflateInit2(&strm, -15) != Z_OK) ThrowIf(true);
				strm.next_in = (Bytef*)pIn;
				strm.avail_in = (uInt)(nImageDataSize - (pIn - (unsigned char*)pBuffer));
				unsigned char* pChanOut = pInflated + channel * chanSize;
				strm.next_out = (Bytef*)pChanOut;
				strm.avail_out = (uInt)chanSize;
				int ret = inflate(&strm, Z_FINISH);
				uInt consumed = (uInt)((Bytef*)strm.next_in - (Bytef*)pIn);
				inflateEnd(&strm);
				if (ret != Z_STREAM_END) ThrowIf(true);
				pIn += consumed;
				if (nCompressionMethod == COMPRESSION_ZipWithPrediction) {
					UnapplyPrediction(pChanOut, nWidth, nHeight, bytesPerSample);
				}
			}
			p = pInflated;
			// For ZIP, data is now in pInflated as planar channels; fall through to planar decode below.
		}

		if (nBitDepth == 8) {
			// 8-bit: original RLE/None/ZIP planar decode into interleaved pPixelData
			if (nCompressionMethod == COMPRESSION_RLE) {
				// Skip byte counts for scanlines
				unsigned char* pRleStart = (unsigned char*)pBuffer + nHeight * nRealChannels * 2 * nVersion;
				unsigned char* pOffset = pRleStart;
				for (unsigned channel = 0; channel < nChannels; channel++) {
					unsigned rchannel = (nColorMode == MODE_Lab) ? channel : ((-channel - 2) % nChannels);
					for (unsigned row = 0; row < nHeight; row++) {
						unsigned char* pRow = pOffset;
						for (unsigned count = 0; count < nWidth; ) {
							ThrowIf(pRow >= (unsigned char*)pBuffer + nImageDataSize);
							unsigned char c = *pRow++;
							if (c > 128) {
								c = ~c + 2;
								ThrowIf(pRow >= (unsigned char*)pBuffer + nImageDataSize);
								unsigned char value = *pRow++;
								for (unsigned i = count; i < count + c; i++) {
									unsigned char* pixel = (unsigned char*)pPixelData + (size_t)row * nRowSize + i * nChannels + rchannel;
									ThrowIf(pixel >= (unsigned char*)pPixelData + nPixelDataSize);
									*pixel = value;
								}
							} else if (c < 128) {
								c++;
								for (unsigned i = count; i < count + c; i++) {
									ThrowIf(pRow >= (unsigned char*)pBuffer + nImageDataSize);
									unsigned char value = *pRow++;
									unsigned char* pixel = (unsigned char*)pPixelData + (size_t)row * nRowSize + i * nChannels + rchannel;
									ThrowIf(pixel >= (unsigned char*)pPixelData + nPixelDataSize);
									*pixel = value;
								}
							}
							count += c;
						}
						if (nVersion == 2) {
							pOffset += _byteswap_ulong(*(unsigned int*)(pBuffer + (channel * nHeight + row) * 4));
						} else {
							pOffset += _byteswap_ushort(*(unsigned short*)(pBuffer + (channel * nHeight + row) * 2));
						}
					}
				}
			} else if (nCompressionMethod == COMPRESSION_None) {
				for (unsigned channel = 0; channel < nChannels; channel++) {
					unsigned rchannel = (nColorMode == MODE_Lab) ? channel : ((-channel - 2) % nChannels);
					for (unsigned row = 0; row < nHeight; row++) {
						for (unsigned count = 0; count < nWidth; count++) {
							ThrowIf(p >= (unsigned char*)pBuffer + nImageDataSize);
							unsigned char value = *p++;
							unsigned char* pixel = (unsigned char*)pPixelData + (size_t)row * nRowSize + count * nChannels + rchannel;
							ThrowIf(pixel >= (unsigned char*)pPixelData + nPixelDataSize);
							*pixel = value;
						}
					}
				}
			} else {
				// ZIP (already inflated into pInflated, planar)
				size_t chanSize = (size_t)nWidth * nHeight;
				for (unsigned channel = 0; channel < nChannels; channel++) {
					unsigned rchannel = (nColorMode == MODE_Lab) ? channel : ((-channel - 2) % nChannels);
					unsigned char* pChan = pInflated + channel * chanSize;
					for (unsigned row = 0; row < nHeight; row++) {
						for (unsigned count = 0; count < nWidth; count++) {
							unsigned char* pixel = (unsigned char*)pPixelData + (size_t)row * nRowSize + count * nChannels + rchannel;
							ThrowIf(pixel >= (unsigned char*)pPixelData + nPixelDataSize);
							*pixel = pChan[row * nWidth + count];
						}
					}
				}
			}
		} else {
			// 16/32-bit: planar data (None or ZIP-inflated), downsample to 8-bit interleaved.
			// For RLE on 16/32-bit, PSD uses the same PackBits RLE but on the full sample width;
			// we decompress into a planar buffer first.
			size_t chanSize = (size_t)nWidth * nHeight * bytesPerSample;
			unsigned char* pPlanar = NULL;
			if (nCompressionMethod == COMPRESSION_ZipWithoutPrediction || nCompressionMethod == COMPRESSION_ZipWithPrediction) {
				pPlanar = pInflated; // already planar in pInflated
			} else if (nCompressionMethod == COMPRESSION_RLE) {
				// PackBits RLE on bytesPerSample-wide samples. PSD RLE byte counts are per
				// row per channel; for 16/32-bit the counts are in the same table but each
				// row is width*bytesPerSample bytes.
				pPlanar = new(std::nothrow) unsigned char[chanSize * nRealChannels];
				if (pPlanar == NULL) { bOutOfMemory = true; ThrowIf(true); }
				unsigned char* pRleStart = (unsigned char*)pBuffer + nHeight * nRealChannels * 2 * nVersion;
				unsigned char* pOffset = pRleStart;
				int rowBytes = nWidth * bytesPerSample;
				for (unsigned channel = 0; channel < nRealChannels; channel++) {
					for (unsigned row = 0; row < nHeight; row++) {
						unsigned char* pRow = pOffset;
						unsigned char* pDst = pPlanar + channel * chanSize + row * rowBytes;
						for (int count = 0; count < rowBytes; ) {
							ThrowIf(pRow >= (unsigned char*)pBuffer + nImageDataSize);
							unsigned char c = *pRow++;
							if (c > 128) {
								c = ~c + 2;
								ThrowIf(pRow >= (unsigned char*)pBuffer + nImageDataSize);
								unsigned char value = *pRow++;
								for (int i = count; i < count + c; i++) pDst[i] = value;
							} else if (c < 128) {
								c++;
								for (int i = count; i < count + c; i++) {
									ThrowIf(pRow >= (unsigned char*)pBuffer + nImageDataSize);
									pDst[i] = *pRow++;
								}
							}
							count += c;
						}
						if (nVersion == 2) {
							pOffset += _byteswap_ulong(*(unsigned int*)(pBuffer + (channel * nHeight + row) * 4));
						} else {
							pOffset += _byteswap_ushort(*(unsigned short*)(pBuffer + (channel * nHeight + row) * 2));
						}
					}
				}
			} else {
				// No compression: planar data is already in pBuffer
				pPlanar = (unsigned char*)pBuffer;
			}

			// Downsample planar 16/32-bit to interleaved 8-bit
			for (unsigned channel = 0; channel < nChannels; channel++) {
				unsigned rchannel = (nColorMode == MODE_Lab) ? channel : ((-channel - 2) % nChannels);
				unsigned char* pChan = pPlanar + channel * chanSize;
				for (unsigned row = 0; row < nHeight; row++) {
					for (unsigned count = 0; count < nWidth; count++) {
						size_t sampleIdx = ((size_t)row * nWidth + count) * bytesPerSample;
						unsigned char value8;
						if (bytesPerSample == 2) {
							// 16-bit big-endian: take high byte
							value8 = pChan[sampleIdx];
						} else {
							// 32-bit float big-endian: take high byte of the float
							value8 = pChan[sampleIdx];
						}
						unsigned char* pixel = (unsigned char*)pPixelData + (size_t)row * nRowSize + count * nChannels + rchannel;
						ThrowIf(pixel >= (unsigned char*)pPixelData + nPixelDataSize);
						*pixel = value8;
					}
				}
			}
			if (pPlanar != pInflated && pPlanar != (unsigned char*)pBuffer) delete[] pPlanar;
		}

		delete[] pInflated;

		ICCProfileTransform::DoTransform(transform, pPixelData, pPixelData, nWidth, nHeight, nRowSize);

		if (nChannels == 4) {
			// For RGBA images the alpha channel is preserved (background composited at render time).
			// For CMYK images the K channel is blended onto black (no transparency to preserve).
			uint32* pImage32 = (uint32*)pPixelData;
			COLORREF backgroundColor = nColorMode == MODE_CMYK ? 0 : CSettingsProvider::This().ColorTransparency();
			if (nColorMode == MODE_CMYK) {
				// Use 64-bit iteration count: on x64 MAX_IMAGE_DIMENSION is 1,000,000,
				// so nWidth*nHeight can exceed INT_MAX and overflow as a 32-bit product.
				__int64 nPixelCount = (__int64)nWidth * nHeight;
				for (__int64 i = 0; i < nPixelCount; i++) {
					// Read-modify-write must be one statement, not `*p++ = f(*p)`,
					// which reads and writes *p unsequenced (undefined behavior).
					*pImage32 = Helpers::AlphaBlendBackground(*pImage32, backgroundColor);
					pImage32++;
				}
			}
		}

		Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData, nChannels, 0, IF_PSD, false, 0, 1, 0);
	} catch (...) {
		delete Image;
		Image = NULL;
	}
	::CloseHandle(hFile);
	if (Image == NULL) {
		delete[] pPixelData;
	}
	delete[] pBuffer;
	delete[] pEXIFData;
	delete[] pICCProfile;
	ICCProfileTransform::DeleteTransform(transform);
	return Image;
};


CJPEGImage* PsdReader::ReadThumb(LPCTSTR strFileName, bool& bOutOfMemory)
{
	HANDLE hFile;
	hFile = ::CreateFile(strFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return NULL;
	}
	char* pBuffer = NULL;
	void* pPixelData = NULL;
	void* pEXIFData = NULL;
	CJPEGImage* Image = NULL;
	int nWidth, nHeight, nChannels;
	int nJpegSize;
	TJSAMP eChromoSubSampling;

	try {
		// Skip file header
		SeekFile(hFile, PSD_HEADER_SIZE);

		// Skip color mode data
		unsigned int nColorDataSize = ReadUIntFromFile(hFile);
		SeekFile(hFile, nColorDataSize);

		// Skip resource section size
		ReadUIntFromFile(hFile);

		for (;;) {
			// Resource block signature
			try {
				if (ReadUIntFromFile(hFile) != 0x3842494D) { // "8BIM"
					break;
				}
			} catch (...) {
				break;
			}


			unsigned short nResourceID = ReadUShortFromFile(hFile);

			// Skip Pascal string (padded to be even length)
			unsigned char nStringSize = ReadUCharFromFile(hFile);
			SeekFile(hFile, nStringSize | 1);

			// Resource size
			unsigned int nResourceSize = ReadUIntFromFile(hFile);

			// Parse image resources
			switch (nResourceID) {
				case 0x0409: // 0x0409 1033 (Photoshop 4.0) Thumbnail resource for Photoshop 4.0 only. See See Thumbnail resource format.
				case 0x040C: // 0x040C 1036 (Photoshop 5.0) Thumbnail resource (supersedes resource 1033). See See Thumbnail resource format.
					// Skip thumbnail resource header
					SeekFile(hFile, 28);

					// Read embedded JPEG thumbnail
					nJpegSize = nResourceSize - 28;
					if (nJpegSize > MAX_JPEG_FILE_SIZE) {
						bOutOfMemory = true;
						ThrowIf(true);
					}

					pBuffer = new(std::nothrow) char[nJpegSize];
					if (pBuffer == NULL) {
						bOutOfMemory = true;
						ThrowIf(true);
					}

					ReadFromFile(pBuffer, hFile, nJpegSize);
					SeekFile(hFile, -nResourceSize);


					pPixelData = TurboJpeg::ReadImage(nWidth, nHeight, nChannels, eChromoSubSampling, bOutOfMemory, pBuffer, nJpegSize);
					break;

				case 0x0422: // 0x0422 1058 (Photoshop 7.0) EXIF data 1. See http://www.kodak.com/global/plugins/acrobat/en/service/digCam/exifStandard2.pdf
				case 0x0423: // 0x0423 1059 (Photoshop 7.0) EXIF data 3. See http://www.kodak.com/global/plugins/acrobat/en/service/digCam/exifStandard2.pdf
					if (pEXIFData == NULL && nResourceSize < 65526) {
						pEXIFData = new(std::nothrow) char[nResourceSize + 10];
						if (pEXIFData != NULL) {
							memcpy(pEXIFData, "\xFF\xE1\0\0Exif\0\0", 10);
							*((unsigned short*)pEXIFData + 1) = _byteswap_ushort(nResourceSize + 8);
							ReadFromFile((char*)pEXIFData + 10, hFile, nResourceSize);
							SeekFile(hFile, -nResourceSize);
						}
					}
					break;
			}

			// Skip resource data (padded to be even length)
			SeekFile(hFile, (nResourceSize + 1) & -2);
		}

		if (pPixelData != NULL) {
			Image = new CJPEGImage(nWidth, nHeight, pPixelData, pEXIFData,
				nChannels, Helpers::CalculateJPEGFileHash(pBuffer, nJpegSize), IF_JPEG_Embedded, false, 0, 1, 0);
			Image->SetJPEGComment(Helpers::GetJPEGComment(pBuffer, nJpegSize));
			Image->SetJPEGChromoSampling(eChromoSubSampling);
		}

	} catch(...) {
		delete Image;
		Image = NULL;
	}
	::CloseHandle(hFile);
	if (Image == NULL) {
		delete[] pPixelData;
	}
	delete[] pEXIFData;
	delete[] pBuffer;
	return Image;
}
