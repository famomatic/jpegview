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
    CrtDbgStub.cpp
    res/JPEGView.ico)

target_precompile_headers(JPEGView PRIVATE stdafx.h)

# WTL is a vendored header-only submodule; image codec deps come from vcpkg.
# QOI is a header-only submodule kept under extras/third_party/qoi.
target_include_directories(JPEGView PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${WTL_INCLUDE_DIR}"
    "${CMAKE_SOURCE_DIR}/extras/third_party/qoi")

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

    # Static runtime (/MT, /MTd) to match the vcpkg static-* triplet.
    set(_rt "$<$<CONFIG:Debug>:MultiThreadedDebug>$<$<NOT:$<CONFIG:Debug>>:MultiThreaded>")
    set_property(TARGET JPEGView PROPERTY MSVC_RUNTIME_LIBRARY "${_rt}")

    target_compile_options(JPEGView PRIVATE
        $<$<CONFIG:Debug>:/Od;/ZI>
        $<$<CONFIG:Release>:/O2>)

    # Link the vcpkg-provided static codec libraries plus Windows system libs.
    target_link_libraries(JPEGView PRIVATE
        WICLoader
        ${JPEGVIEW_DEP_TARGETS}
        Dbghelp
        gdiplus)

    if(CMAKE_VS_PLATFORM_NAME STREQUAL "x64")
        target_link_libraries(JPEGView PRIVATE
            legacy_stdio_definitions
            legacy_stdio_wide_specifiers)
    endif()

    target_link_options(JPEGView PRIVATE /SUBSYSTEM:WINDOWS)
endif()

# ---------------------------------------------------------------------------
# Post-build: copy config files and LICENSE next to JPEGView.exe.
# With static vcpkg linkage the codec libs are linked in, so no DLL copies.
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
