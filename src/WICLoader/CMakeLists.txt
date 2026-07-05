# WICLoader.dll - WIC image decoding helper DLL statically linked into JPEGView.
# Its exports are __declspec(dllexport); imported by the main app via
# __declspec(dllimport) at compile time and linked normally (not delay-loaded).
add_library(WICLoader SHARED
    WICLoader.cpp
    WICLoader.rc)

# dllmain.cpp must not use the PCH; stdafx.cpp is the TU that creates it.
target_sources(WICLoader PRIVATE
    dllmain.cpp
    stdafx.cpp)

target_include_directories(WICLoader PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_precompile_headers(WICLoader PRIVATE stdafx.h)
set_source_files_properties(dllmain.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

set_target_properties(WICLoader PROPERTIES
    OUTPUT_NAME "WICLoader"
    PREFIX ""                  # avoid libWICLoader.dll
    VS_DPI_AWARE "PerMonitor")

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_definitions(WICLoader PRIVATE
        WIN32
        _WINDOWS
        STRICT
        WICLOADER_EXPORTS
        _CRT_SECURE_NO_DEPRECATE
        UNICODE
        _UNICODE)

    target_compile_options(WICLoader PRIVATE
        /utf-8
        /Zc:wchar_t
        /Zc:__cplusplus
        /permissive-
        /Zc:preprocessor
        /GR
        /EHsc)

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        target_compile_definitions(WICLoader PRIVATE WIN64)
    endif()

    # Static runtime to match the prebuilt third-party libs (/MT, /MTd).
    set(_rt "$<$<CONFIG:Debug>:MultiThreadedDebug>$<$<NOT:$<CONFIG:Debug>>:MultiThreaded>")
    set_property(TARGET WICLoader PROPERTY MSVC_RUNTIME_LIBRARY "${_rt}")

    target_compile_options(WICLoader PRIVATE
        $<$<CONFIG:Debug>:/Od>
        $<$<CONFIG:Release>:/O2>)

    target_link_options(WICLoader PRIVATE /SUBSYSTEM:WINDOWS)
endif()

target_link_libraries(WICLoader PRIVATE WindowsCodecs)
