# WiX v3 MSI installer.  Invokes candle.exe / light.exe directly so the build
# no longer depends on the deleted JPEGView.vcxproj ProjectReference.
#
# The WiX source (Product.wxs) consumes these preprocessor variables:
#   JPEGView.TargetDir   - dir holding JPEGView.exe + post-build copies
#   JPEGView.ProjectDir  - the src/JPEGView source tree (for res/ icon, Config)
#   WICLoader.TargetDir   - dir holding WICLoader.dll  (== JPEGView.TargetDir)
#   DllBinFolder          - "bin" (x86) or "bin64" (x64)
#
# Enable with: cmake -DJPEGVIEW_BUILD_SETUP=ON ...
# Requires WiX Toolset v3.11+ on PATH or WIX env var set.

find_program(WIX_CANDLE candle.exe)
find_program(WIX_LIGHT  light.exe)
find_program(WIX_HEAT   heat.exe)

if(NOT WIX_CANDLE OR NOT WIX_LIGHT)
    message(FATAL_ERROR
        "WiX Toolset v3.11+ (candle.exe / light.exe) not found on PATH. "
        "Install it from https://wixtoolset.org/releases/ and ensure its bin "
        "directory is on PATH, or set the WIX environment variable.")
endif()

set(_wix_source_dir "${CMAKE_CURRENT_SOURCE_DIR}")

# We produce one installer per platform/configuration the caller requests.
# JPEGVIEW_SETUP_PLATFORM selects x86 or x64; defaults to the build platform.
if(NOT DEFINED JPEGVIEW_SETUP_PLATFORM)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(JPEGVIEW_SETUP_PLATFORM "x64")
    else()
        set(JPEGVIEW_SETUP_PLATFORM "x86")
    endif()
endif()

set(_plat "${JPEGVIEW_SETUP_PLATFORM}")
if(_plat STREQUAL "x64")
    set(_dllbin "bin64")
else()
    set(_dllbin "bin")
endif()

# The staged install dir: CMake copies JPEGView.exe + WICLoader.dll + all the
# post-build files here.  Point WiX at this single directory.
set(_stagedir "${CMAKE_BINARY_DIR}/staged/${_plat}/$<CONFIG>")

# Name the output MSI after platform/config.
set(_msi_name "JPEGView-${_plat}-$<CONFIG>.msi")

set(_wix_obj "${CMAKE_CURRENT_BINARY_DIR}/Product-$<CONFIG>.wixobj")

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Product-$<CONFIG>.wixobj.placeholder"
    COMMENT "Compiling WiX sources (${_plat}, $<CONFIG>)"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}"
    COMMAND ${WIX_CANDLE}
        -nologo
        -dWixVSExtension
        "-dJPEGView.TargetDir=${_stagedir}/"
        "-dJPEGView.ProjectDir=${CMAKE_SOURCE_DIR}/src/JPEGView/"
        "-dWICLoader.TargetDir=${_stagedir}/"
        "-dDllBinFolder=${_dllbin}"
        -arch "${_plat}"
        # $(var.Platform) is a WiX preprocessor variable, not set by -arch.
        # Define it explicitly so the x86/x64 conditionals in Product.wxs
        # (<?if $(var.Platform) = x64 ?>) resolve under the CMake-driven build.
        "-dPlatform=${_plat}"
        -out "${_wix_obj}"
        "${_wix_source_dir}/Product.wxs"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/Product-$<CONFIG>.wixobj.placeholder"
    DEPENDS "${_wix_source_dir}/Product.wxs"
    VERBATIM)

add_custom_target(JPEGView.Setup ALL
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/Product-$<CONFIG>.wixobj.placeholder"
    COMMENT "Linking WiX MSI (${_plat}, $<CONFIG>)"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/msi"
    COMMAND ${WIX_LIGHT}
        -nologo
        -ext WixUIExtension
        -out "${CMAKE_BINARY_DIR}/msi/${_msi_name}"
        # Product.wxs references !(loc.*) localization variables defined in
        # Product_en-us.wxl; light does not auto-discover .wxl files, so pass
        # the en-us localization explicitly via -loc.
        -loc Product_en-us.wxl
        "${_wix_obj}"
    WORKING_DIRECTORY "${_wix_source_dir}"
    VERBATIM)

add_dependencies(JPEGView.Setup JPEGView)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}")
target_sources(JPEGView.Setup PRIVATE
    Product.wxs
    Product_en-us.wxl
    res/License.rtf
    res/BannerTop.bmp
    res/Dialog.bmp)
