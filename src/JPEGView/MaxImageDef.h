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
const unsigned long long MAX_IMAGE_PIXELS = 1000000ULL * 1000000ULL;
#else
const unsigned int MAX_IMAGE_PIXELS = 1024 * 1024 * 100;
#endif

// x64 빌드에서는 리샘플러 고정소수점이 uintfp(64비트)로 동작하므로 65535 제한이 해제된다.
// 부분 로딩으로 전체 디코드가 발생하지 않으므로 RAM 가드는 MAX_IMAGE_PIXELS로 충분하다.
#ifdef _WIN64
const unsigned int MAX_IMAGE_DIMENSION = 1000000;
#else
const unsigned int MAX_IMAGE_DIMENSION = 65535;
#endif
