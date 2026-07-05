// libjxl (release, static) still emits references to _CrtDbgReport through
// its assert path.  The release CRT (LIBCMT) does not provide that symbol,
// so a no-op stub satisfies the linker without pulling in the debug UCRT.
#ifndef _DEBUG
#include <crtdbg.h>

extern "C" int __cdecl _CrtDbgReport(int, const char*, int, const char*, const char*, ...)
{
    return 0;
}
#endif
