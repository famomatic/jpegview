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
# The prebuilt third-party libraries live next to the JPEGView sources.
set(JPEGVIEW_LIBS_DIR   "${JPEGVIEW_SOURCE_DIR}/JPEGView")

# Each prebuilt dependency provides an include dir shared by both arches and
# separate lib dirs (lib = x86, lib64 = x64).  We resolve them through
# generator expressions so a single configure can target both architectures.
function(jpegview_third_party_paths)
    set(_includes
        "${WTL_INCLUDE_DIR}"
        "${JPEGVIEW_LIBS_DIR}/libpng-apng/include"
        "${JPEGVIEW_LIBS_DIR}/libjxl/include"
        "${JPEGVIEW_LIBS_DIR}/libheif/include"
        "${JPEGVIEW_LIBS_DIR}/libwebp/include"
        "${JPEGVIEW_LIBS_DIR}/libavif/include"
        "${JPEGVIEW_LIBS_DIR}/lcms2/include"
        "${JPEGVIEW_LIBS_DIR}/libraw/include")
    set(_libs_x86
        "${JPEGVIEW_LIBS_DIR}/libjpeg-turbo/lib"
        "${JPEGVIEW_LIBS_DIR}/libpng-apng/lib"
        "${JPEGVIEW_LIBS_DIR}/libjxl/lib"
        "${JPEGVIEW_LIBS_DIR}/libheif/lib"
        "${JPEGVIEW_LIBS_DIR}/libwebp/lib"
        "${JPEGVIEW_LIBS_DIR}/libavif/lib"
        "${JPEGVIEW_LIBS_DIR}/lcms2/lib"
        "${JPEGVIEW_LIBS_DIR}/libraw/lib")
    set(_libs_x64
        "${JPEGVIEW_LIBS_DIR}/libjpeg-turbo/lib64"
        "${JPEGVIEW_LIBS_DIR}/libpng-apng/lib64"
        "${JPEGVIEW_LIBS_DIR}/libjxl/lib64"
        "${JPEGVIEW_LIBS_DIR}/libheif/lib64"
        "${JPEGVIEW_LIBS_DIR}/libwebp/lib64"
        "${JPEGVIEW_LIBS_DIR}/libavif/lib64"
        "${JPEGVIEW_LIBS_DIR}/lcms2/lib64"
        "${JPEGVIEW_LIBS_DIR}/libraw/lib64")
    set(JPEGVIEW_THIRDPARTY_INCLUDE_DIRS "${_includes}" PARENT_SCOPE)
    set(JPEGVIEW_THIRDPARTY_LIB_DIRS_X86  "${_libs_x86}"  PARENT_SCOPE)
    set(JPEGVIEW_THIRDPARTY_LIB_DIRS_X64  "${_libs_x64}"  PARENT_SCOPE)
endfunction()
jpegview_third_party_paths()

add_subdirectory(src/WICLoader)
add_subdirectory(src/JPEGView)

if(JPEGVIEW_BUILD_SETUP)
    add_subdirectory(src/JPEGView.Setup)
endif()
