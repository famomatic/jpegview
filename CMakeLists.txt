cmake_minimum_required(VERSION 3.20)

project(JPEGView VERSION 1.3.46.0 LANGUAGES CXX
    DESCRIPTION "A fast image viewer with editing capabilities")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Build artifacts go under <build>/out, matching the old vcxproj output layout.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
endif()

option(JPEGVIEW_BUILD_SETUP "Build the WiX MSI installer (requires WiX Toolset v3.11+)" OFF)

# Visual Studio 2022 (v143 toolset) is the only supported toolchain.  If CMake
# selects a BuildTools instance that lacks ATL (JPEGView requires it), pin the
# full VS instance with -DCMAKE_GENERATOR_INSTANCE="C:/path/to/VS/2022/Community".

set(JPEGVIEW_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(JPEGVIEW_DEPS_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/deps")
set(WTL_INCLUDE_DIR     "${JPEGVIEW_DEPS_DIR}/WTL-sf/Include")

# ---------------------------------------------------------------------------
# vcpkg dependency resolution
# ---------------------------------------------------------------------------
# Configure with the vcpkg toolchain:
#   cmake -B build -S . -G "Visual Studio 17 2022" -A x64 \
#     -DCMAKE_TOOLCHAIN_FILE=D:/vcpkg/scripts/buildsystems/vcpkg.cmake \
#     -DVCPKG_TARGET_TRIPLET=x64-windows-static
#
# The static-* triplet is required: JPEGView links the static CRT (/MT, /MTd)
# and all dependencies must match, otherwise LNK2038 / runtime mismatches occur.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    message(FATAL_ERROR
        "JPEGView now builds against vcpkg dependencies.  Pass the vcpkg "
        "toolchain file, e.g.:\n"
        "  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake\n"
        "and a static triplet matching the static CRT:\n"
        "  -DVCPKG_TARGET_TRIPLET=x64-windows-static  (or x86-windows-static)")
endif()

# Find every image-codec dependency via the vcpkg-provided config packages.
# Most expose modern imported targets; libjxl ships only pkg-config in vcpkg,
# so it is resolved manually with find_library/find_path below.
find_package(libjpeg-turbo CONFIG REQUIRED)
find_package(PNG CONFIG REQUIRED)
find_package(ZLIB CONFIG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(lcms2 CONFIG REQUIRED)
find_package(LibRaw CONFIG REQUIRED)
find_package(libheif CONFIG REQUIRED)
find_package(libavif CONFIG REQUIRED)
# AOM is a transitive dependency of libheif/libavif; its config is AOMConfig.cmake
# and the heif target references AOM::aom, so it must be found explicitly.
find_package(AOM CONFIG REQUIRED)

# libjxl has no CMake config in vcpkg; locate the headers and libs directly.
# Its pkg-config (libjxl.pc) requires libhwy, libbrotlidec/enc, and libjxl_cms,
# so those transitive deps must be linked explicitly.  vcpkg installs release
# libs under lib/ and debug libs under debug/lib/; pick per-configuration so
# the _ITERATOR_DEBUG_LEVEL / RuntimeLibrary match (MT vs MTd).
find_package(hwy CONFIG REQUIRED)

# Resolve the vcpkg install root explicitly.  Classic mode (VCPKG_INSTALLED_DIR
# or <vcpkg>/installed) is preferred so the per-config MT/MTd libs are already
# present at configure time; manifest mode also works via <build>/vcpkg_installed.
set(_vcpkg_inst_candidates
    "${VCPKG_INSTALLED_DIR}"
    "${_VCPKG_INSTALLED_DIR}"
    "${CMAKE_BINARY_DIR}/vcpkg_installed")
get_filename_component(_vcpkg_tc_dir "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
get_filename_component(_vcpkg_tc_root "${_vcpkg_tc_dir}/../.." ABSOLUTE)
list(APPEND _vcpkg_inst_candidates "${_vcpkg_tc_root}/installed")
set(_vcpkg_installed "")
foreach(_c ${_vcpkg_inst_candidates})
    if(_c AND EXISTS "${_c}/${VCPKG_TARGET_TRIPLET}/lib/jxl.lib")
        set(_vcpkg_installed "${_c}")
        break()
    endif()
endforeach()
if(NOT _vcpkg_installed)
    message(FATAL_ERROR
        "Could not locate the vcpkg installed tree for triplet "
        "'${VCPKG_TARGET_TRIPLET}'.  Run the vcpkg install first or set "
        "VCPKG_INSTALLED_DIR.")
endif()
message(STATUS "Using vcpkg installed tree: ${_vcpkg_installed}")
set(_vcpkg_lib    "${_vcpkg_installed}/${VCPKG_TARGET_TRIPLET}/lib")
set(_vcpkg_libdbg "${_vcpkg_installed}/${VCPKG_TARGET_TRIPLET}/debug/lib")

set(LIBJXL_INCLUDE_DIR "${_vcpkg_installed}/${VCPKG_TARGET_TRIPLET}/include")
set(LIBJXL_LIBRARY_REL     "${_vcpkg_lib}/jxl.lib")
set(LIBJXL_LIBRARY_DBG     "${_vcpkg_libdbg}/jxl.lib")
set(LIBJXL_CMS_LIBRARY_REL "${_vcpkg_lib}/jxl_cms.lib")
set(LIBJXL_CMS_LIBRARY_DBG "${_vcpkg_libdbg}/jxl_cms.lib")
set(LIBJXL_THREADS_REL     "${_vcpkg_lib}/jxl_threads.lib")
set(LIBJXL_THREADS_DBG     "${_vcpkg_libdbg}/jxl_threads.lib")
set(BROTLIDEC_REL     "${_vcpkg_lib}/brotlidec.lib")
set(BROTLIDEC_DBG     "${_vcpkg_libdbg}/brotlidec.lib")
set(BROTLIENC_REL     "${_vcpkg_lib}/brotlienc.lib")
set(BROTLIENC_DBG     "${_vcpkg_libdbg}/brotlienc.lib")
set(BROTLICOMMON_REL  "${_vcpkg_lib}/brotlicommon.lib")
set(BROTLICOMMON_DBG  "${_vcpkg_libdbg}/brotlicommon.lib")
foreach(_v LIBJXL_LIBRARY_REL LIBJXL_LIBRARY_DBG LIBJXL_CMS_LIBRARY_REL LIBJXL_CMS_LIBRARY_DBG
          LIBJXL_THREADS_REL LIBJXL_THREADS_DBG BROTLIDEC_REL BROTLIDEC_DBG
          BROTLIENC_REL BROTLIENC_DBG BROTLICOMMON_REL BROTLICOMMON_DBG)
    if(NOT EXISTS "${${_v}}")
        message(FATAL_ERROR "Expected vcpkg lib not found: ${${_v}}")
    endif()
endforeach()
# Wrap the raw lib/path into imported targets with per-config locations so the
# linker picks MT (release) or MTd (debug) matching the active configuration.
add_library(JpegXL::jxl STATIC IMPORTED)
set_target_properties(JpegXL::jxl PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LIBJXL_INCLUDE_DIR}"
    INTERFACE_COMPILE_DEFINITIONS "JXL_STATIC_DEFINE"
    IMPORTED_LOCATION_RELEASE "${LIBJXL_LIBRARY_REL}"
    IMPORTED_LOCATION_DEBUG   "${LIBJXL_LIBRARY_DBG}")
set_property(TARGET JpegXL::jxl PROPERTY IMPORTED_CONFIGURATIONS RELEASE DEBUG)
add_library(JpegXL::jxl_threads STATIC IMPORTED)
set_target_properties(JpegXL::jxl_threads PROPERTIES
    IMPORTED_LOCATION_RELEASE "${LIBJXL_THREADS_REL}"
    IMPORTED_LOCATION_DEBUG   "${LIBJXL_THREADS_DBG}")
set_property(TARGET JpegXL::jxl_threads PROPERTY IMPORTED_CONFIGURATIONS RELEASE DEBUG)
# Brotli + jxl_cms have no config either; link them per-configuration.  Each
# library is a separate generator expression so list expansion stays clean.
set_property(TARGET JpegXL::jxl APPEND PROPERTY INTERFACE_LINK_LIBRARIES
    "hwy::hwy"
    "$<$<CONFIG:Release>:${LIBJXL_CMS_LIBRARY_REL}>"
    "$<$<CONFIG:Debug>:${LIBJXL_CMS_LIBRARY_DBG}>"
    "$<$<CONFIG:Release>:${BROTLIDEC_REL}>"
    "$<$<CONFIG:Debug>:${BROTLIDEC_DBG}>"
    "$<$<CONFIG:Release>:${BROTLIENC_REL}>"
    "$<$<CONFIG:Debug>:${BROTLIENC_DBG}>"
    "$<$<CONFIG:Release>:${BROTLICOMMON_REL}>"
    "$<$<CONFIG:Debug>:${BROTLICOMMON_DBG}>")

# Collect the dependency targets into a single variable for the JPEGView target.
set(JPEGVIEW_DEP_TARGETS
    libjpeg-turbo::turbojpeg-static
    PNG::PNG
    ZLIB::ZLIB
    WebP::webp
    WebP::webpdemux
    lcms2::lcms2
    libraw::raw_r
    heif
    avif
    JpegXL::jxl
    JpegXL::jxl_threads
)

add_subdirectory(src/WICLoader)
add_subdirectory(src/JPEGView)

if(JPEGVIEW_BUILD_SETUP)
    add_subdirectory(src/JPEGView.Setup)
endif()
