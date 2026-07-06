#pragma once

// Sizes are in bytes

#ifdef _WIN64
const unsigned int MAX_JPEG_FILE_SIZE = 1024 * 1024 * 300;
#else
const unsigned int MAX_JPEG_FILE_SIZE = 1024 * 1024 * 50;
#endif

#ifdef _WIN64
const unsigned int MAX_PNG_FILE_SIZE = 1024 * 1024 * 300;
#else
const unsigned int MAX_PNG_FILE_SIZE = 1024 * 1024 * 50;
#endif

#ifdef _WIN64
const unsigned int MAX_WEBP_FILE_SIZE = 1024 * 1024 * 150;
#else
const unsigned int MAX_WEBP_FILE_SIZE = 1024 * 1024 * 50;
#endif

#ifdef _WIN64
const unsigned int MAX_JXL_FILE_SIZE = 1024 * 1024 * 150;
#else
const unsigned int MAX_JXL_FILE_SIZE = 1024 * 1024 * 50;
#endif

#ifdef _WIN64
const unsigned int MAX_HEIF_FILE_SIZE = 1024 * 1024 * 150;
#else
const unsigned int MAX_HEIF_FILE_SIZE = 1024 * 1024 * 50;
#endif

#ifdef _WIN64
const unsigned int MAX_PSD_FILE_SIZE = 1024 * 1024 * 500;
#else
const unsigned int MAX_PSD_FILE_SIZE = 1024 * 1024 * 100;
#endif

#ifdef _WIN64
const unsigned int MAX_BMP_FILE_SIZE = 1024 * 1024 * 500;
#else
const unsigned int MAX_BMP_FILE_SIZE = 1024 * 1024 * 100;
#endif

// SVG (vector, rasterized on load)
#ifdef _WIN64
const unsigned int MAX_SVG_FILE_SIZE = 1024 * 1024 * 100;
#else
const unsigned int MAX_SVG_FILE_SIZE = 1024 * 1024 * 50;
#endif

// DDS (game textures)
#ifdef _WIN64
const unsigned int MAX_DDS_FILE_SIZE = 1024 * 1024 * 200;
#else
const unsigned int MAX_DDS_FILE_SIZE = 1024 * 1024 * 50;
#endif

// JPEG 2000
#ifdef _WIN64
const unsigned int MAX_JP2_FILE_SIZE = 1024 * 1024 * 200;
#else
const unsigned int MAX_JP2_FILE_SIZE = 1024 * 1024 * 50;
#endif

// OpenEXR (HDR)
#ifdef _WIN64
const unsigned int MAX_EXR_FILE_SIZE = 1024 * 1024 * 200;
#else
const unsigned int MAX_EXR_FILE_SIZE = 1024 * 1024 * 50;
#endif

// Radiance HDR
#ifdef _WIN64
const unsigned int MAX_HDR_FILE_SIZE = 1024 * 1024 * 100;
#else
const unsigned int MAX_HDR_FILE_SIZE = 1024 * 1024 * 50;
#endif

// JPEG XR
#ifdef _WIN64
const unsigned int MAX_JXR_FILE_SIZE = 1024 * 1024 * 200;
#else
const unsigned int MAX_JXR_FILE_SIZE = 1024 * 1024 * 50;
#endif

// this may be an artificial limitation and might make configurable, or ignore custom setting only for win32
#ifdef _WIN64
const unsigned int MAX_IMAGE_PIXELS = 65535 * 65535;
#else
const unsigned int MAX_IMAGE_PIXELS = 1024 * 1024 * 100;
#endif

// That must not be bigger than 65535 due to internal limitations
//
// unbounding (>65535) this causes crashes if HighQualityResampling=true
// but if it's false, some images load (so far png tested was corrupted)
const unsigned int MAX_IMAGE_DIMENSION = 65535;
