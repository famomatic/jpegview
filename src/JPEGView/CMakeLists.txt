# JPEGView.exe - main application
add_executable(JPEGView WIN32
    AboutDlg.cpp
    ApplyFilterAVX.cpp
    AVIFWrapper.cpp
    BasicProcessing.cpp
    BatchCopyDlg.cpp
    Clipboard.cpp
    CropCtl.cpp
    CropSizeDlg.cpp
    dcraw_mod.cpp
    DesktopWallpaper.cpp
    DirectoryWatcher.cpp
    EXIFDisplay.cpp
    EXIFDisplayCtl.cpp
    EXIFHelpers.cpp
    EXIFReader.cpp
    FileExtensionsDlg.cpp
    FileExtensionsRegistry.cpp
    FileList.cpp
    FileOpenDialog.cpp
    GUIControls.cpp
    HashCompareLPCTSTR.cpp
    HEIFWrapper.cpp
    HelpDisplay.cpp
    HelpDisplayCtl.cpp
    HelpDlg.cpp
    Helpers.cpp
    HelpersGUI.cpp
    HistogramCorr.cpp
    ICCProfileTransform.cpp
    ImageLoadThread.cpp
    ImageProcessingPanel.cpp
    ImageProcPanelCtl.cpp
    InfoButtonPanel.cpp
    InfoButtonPanelCtl.cpp
    JPEGImage.cpp
    JPEGLosslessTransform.cpp
    JPEGProvider.cpp
    JPEGView.cpp
    JPEGView.rc
    JXLWrapper.cpp
    KeyMap.cpp
    LocalDensityCorr.cpp
    MainDlg.cpp
    ManageOpenWithDlg.cpp
    MultiMonitorSupport.cpp
    NavigationPanel.cpp
    NavigationPanelCtl.cpp
    NLS.cpp
    PaintMemDCMgr.cpp
    Panel.cpp
    PanelController.cpp
    PanelMgr.cpp
    ParameterDB.cpp
    PNGWrapper.cpp
    PrintDlg.cpp
    PrintImage.cpp
    ProcessingThreadPool.cpp
    PSDWrapper.cpp
    QOIWrapper.cpp
    RAWWrapper.cpp
    ReaderBMP.cpp
    ReaderTGA.cpp
    ResizeDlg.cpp
    ResizeFilter.cpp
    RotationPanel.cpp
    RotationPanelCtl.cpp
    SaveImage.cpp
    SettingsProvider.cpp
    stdafx.cpp
    TiltCorrectionPanel.cpp
    TiltCorrectionPanelCtl.cpp
    TJPEGWrapper.cpp
    Tooltip.cpp
    TransformPanel.cpp
    TransformPanelCtl.cpp
    UnsharpMaskPanel.cpp
    UnsharpMaskPanelCtl.cpp
    UserCommand.cpp
    WEBPWrapper.cpp
    WndButtonPanel.cpp
    WndButtonPanelCtl.cpp
    WorkThread.cpp
    XMMImage.cpp
    ZoomNavigator.cpp
    ZoomNavigatorCtl.cpp
    res/JPEGView.ico)

target_precompile_headers(JPEGView PRIVATE stdafx.h)

target_include_directories(JPEGView PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${JPEGVIEW_THIRDPARTY_INCLUDE_DIRS})

set_target_properties(JPEGView PROPERTIES
    OUTPUT_NAME "JPEGView"
    VS_DPI_AWARE "PerMonitor"
    VS_GLOBAL_CharacterSet "Unicode")

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_definitions(JPEGView PRIVATE
        WIN32
        _WINDOWS
        STRICT
        _CRT_SECURE_NO_DEPRECATE
        _SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS
        UNICODE
        _UNICODE)

    target_compile_options(JPEGView PRIVATE
        /utf-8
        /Zc:wchar_t
        /GR
        /EHsc)

    # AVX for ApplyFilterAVX on x64 builds.
    if(CMAKE_VS_PLATFORM_NAME STREQUAL "x64")
        set_source_files_properties(ApplyFilterAVX.cpp PROPERTIES
            COMPILE_OPTIONS "/arch:AVX")
    endif()

    # Static runtime to match the prebuilt third-party libs (/MT, /MTd).
    set(_rt "$<$<CONFIG:Debug>:MultiThreadedDebug>$<$<NOT:$<CONFIG:Debug>>:MultiThreaded>")
    set_property(TARGET JPEGView PROPERTY MSVC_RUNTIME_LIBRARY "${_rt}")

    target_compile_options(JPEGView PRIVATE
        $<$<CONFIG:Debug>:/Od;/ZI>
        $<$<CONFIG:Release>:/O2>)

    # Per-platform selection of prebuilt lib dirs and runtime DLL folders.
    if(CMAKE_VS_PLATFORM_NAME STREQUAL "x64")
        set(_tp_lib_dirs ${JPEGVIEW_THIRDPARTY_LIB_DIRS_X64})
        set(_dep_dirs
            libjxl/bin64 libheif/bin64 libavif/bin64 lcms2/bin64 libraw/bin64)
        set(_is_x64 TRUE)
    else()
        set(_tp_lib_dirs ${JPEGVIEW_THIRDPARTY_LIB_DIRS_X86})
        set(_dep_dirs
            libjxl/bin   libheif/bin   libavif/bin   lcms2/bin   libraw/bin)
        set(_is_x64 FALSE)
    endif()

    set(_common_libs
        Dbghelp
        gdiplus
        turbojpeg-static
        libpng16
        zlib
        jxl_dec
        jxl_threads
        heif
        libwebp
        libwebpdemux
        avif
        lcms2
        libraw)

    target_link_libraries(JPEGView PRIVATE
        WICLoader
        ${_common_libs})
    if(_is_x64)
        target_link_libraries(JPEGView PRIVATE
            legacy_stdio_definitions
            legacy_stdio_wide_specifiers)
    endif()

    target_link_directories(JPEGView PRIVATE ${_tp_lib_dirs})

    # Delay-load the runtime DLLs (mirrors the vcxproj DelayLoadDLLs).
    set(_delayloads
        "/DELAYLOAD:WICLoader.dll"
        "/DELAYLOAD:jxl_dec.dll"
        "/DELAYLOAD:jxl_threads.dll"
        "/DELAYLOAD:heif.dll"
        "/DELAYLOAD:avif.dll"
        "/DELAYLOAD:lcms2.dll"
        "/DELAYLOAD:libraw.dll")
    target_link_options(JPEGView PRIVATE ${_delayloads} "/ignore:4099")
    # The Debug static runtime drags in LIBCMT; ignore it as the old vcxproj did.
    target_link_options(JPEGView PRIVATE "$<$<CONFIG:Debug>:/NODEFAULTLIB:LIBCMT>")
    target_link_libraries(JPEGView PRIVATE delayimp)

    target_link_options(JPEGView PRIVATE /SUBSYSTEM:WINDOWS)
endif()

# ---------------------------------------------------------------------------
# Post-build: copy config files, third-party DLLs, and LICENSE into the
# runtime output directory.  This replaces JPEGView.vcxproj-postbuild-event.bat.
# ---------------------------------------------------------------------------
set(_out_dir "$<TARGET_FILE_DIR:JPEGView>")

# Config files (common across all builds).
file(GLOB _config_files
    "${CMAKE_CURRENT_SOURCE_DIR}/Config/*")
foreach(_f ${_config_files})
    add_custom_command(TARGET JPEGView POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_f}" "${_out_dir}"
        VERBATIM)
endforeach()

# LICENSE.txt lives at the repo root.
add_custom_command(TARGET JPEGView POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE.txt" "${_out_dir}"
    VERBATIM)

# Third-party runtime DLLs: bin (x86) or bin64 (x64), selected above.
foreach(_d ${_dep_dirs})
    add_custom_command(TARGET JPEGView POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/${_d}" "${_out_dir}"
        VERBATIM)
endforeach()
