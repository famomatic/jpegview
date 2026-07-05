set(VCPKG_TARGET_ARCHITECTURE x86)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

# libraw's Debug build hits an MSVC internal compiler error (C1001) on objidl.h
# under the x86 cross-compiler with /Z7.  Disabling frame-pointer omit fixes it.
set(VCPKG_CXX_FLAGS "/d2FH4-")
set(VCPKG_C_FLAGS "/d2FH4-")
