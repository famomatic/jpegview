#pragma once

// Target Windows 10 (0x0A00). The planned GPU backend (D3D11 compute +
// Direct2D 1.1 output) needs APIs that only exist on Win10+. The old
// Win7 (0x0601) baseline is no longer supported.

#define WINVER        0x0A00
#define _WIN32_WINNT  0x0A00
#define _WIN32_IE     0x0A00
#define _RICHEDIT_VER 0x0300

#include <SDKDDKVer.h>
