#pragma once

#ifdef _WIN64
const unsigned long long MAX_IMAGE_PIXELS = 1000000ULL * 1000000ULL;
#else
const unsigned int MAX_IMAGE_PIXELS = 1024 * 1024 * 100;
#endif

// x64 빌드에서는 리샘플러 고정소수점이 uintfp(64비트)로 동작하므로 65535 제한이 해제된다.
#ifdef _WIN64
const unsigned int MAX_IMAGE_DIMENSION = 1000000;
#else
const unsigned int MAX_IMAGE_DIMENSION = 65535;
#endif
