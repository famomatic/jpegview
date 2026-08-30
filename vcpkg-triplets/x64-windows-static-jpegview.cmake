set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

# Keep vcpkg dependencies ABI-compatible with the Visual Studio 2022
# generator used by CMakePresets.json.  Without this, vcpkg selects a newer
# installed Visual Studio toolset and its STL helper symbols cannot be linked
# by v143.
set(VCPKG_PLATFORM_TOOLSET v143)
