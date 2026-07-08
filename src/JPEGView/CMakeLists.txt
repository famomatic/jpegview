# JPEGView.exe - main application
add_executable(JPEGView WIN32
    AboutDlg.cpp
    ApplyFilterHighway.cpp
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
    GpuDevice.cpp
    GpuImageProcessor.cpp
    GpuRenderTarget.cpp
    GpuShaders.cpp
    GpuTextureHelpers.cpp
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
    ImageProcessor.cpp
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
    ProcessingPresets.cpp
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
    TIFFWrapper.cpp
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
    SVGWrapper.cpp
    DDSWrapper.cpp
    JP2Wrapper.cpp
    EXRWrapper.cpp
    HDRWrapper.cpp
    JXRWrapper.cpp
    ThumbnailCache.cpp
    CrtDbgStub.cpp
    res/JPEGView.ico)

target_precompile_headers(JPEGView PRIVATE stdafx.h)

# WTL is a vendored header-only submodule; image codec deps come from vcpkg.
# QOI is a header-only submodule kept under extras/third_party/qoi.
target_include_directories(JPEGView PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${WTL_INCLUDE_DIR}"
    "${CMAKE_SOURCE_DIR}/extras/third_party/qoi"
    "${CMAKE_SOURCE_DIR}/extras/third_party/nanosvg/src"
    "${CMAKE_SOURCE_DIR}/extras/third_party/nanosvg/src")

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
        UNICODE
        _UNICODE)

    target_compile_options(JPEGView PRIVATE
        /utf-8
        /Zc:wchar_t
        /Zc:__cplusplus
        /permissive-
        /Zc:preprocessor
        /GR
        /EHsc)

    # AVX for ApplyFilterAVX on x64 builds.
    # google/highway runtime-dispatches SSE2/AVX2 from a single translation
    # unit (ApplyFilterHighway.cpp), so the per-file /arch:AVX split that the
    # old AVX kernel needed is gone.

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
        hwy::hwy
        Dbghelp
        gdiplus
        d3d11
        dxgi
        d3dcompiler
        d2d1)

    # JXR (jxrlib) uses a FindJXR module rather than a config target.
    target_include_directories(JPEGView PRIVATE ${JXR_INCLUDE_DIRS})
    target_link_libraries(JPEGView PRIVATE ${JXR_LIBRARIES})

    # OpenEXR 3.x headers live under include/OpenEXR and use angle-bracket
    # includes (e.g. <IlmThreadConfig.h>) that expect that dir on the path.
    target_include_directories(JPEGView PRIVATE
        "${_vcpkg_installed}/${VCPKG_TARGET_TRIPLET}/include/OpenEXR"
        "${_vcpkg_installed}/${VCPKG_TARGET_TRIPLET}/include/Imath")

    # legacy_stdio_definitions provides __iob_func (used by dcraw_mod.cpp's
    # fprintf to stderr) on VS2022 for both x86 and x64; the old __iob_func
    # shim in stdafx.cpp was removed.
    target_link_libraries(JPEGView PRIVATE
        legacy_stdio_definitions
        legacy_stdio_wide_specifiers)

    target_link_options(JPEGView PRIVATE /SUBSYSTEM:WINDOWS)
    # -----------------------------------------------------------------------
    # add_gpu_test: builds a standalone console exe for a GPU compute-pass
    # test. All tests share the same source set (GpuDevice.cpp + the shader/
    # texture helpers) and the same MSVC/link settings, so this keeps the
    # per-test boilerplate to one line. Register the test with CTest unless
    # REGISTER_TEST is OFF (used for diagnostic tools that have no pass/fail).
    # -----------------------------------------------------------------------
    function(add_gpu_test name)
        cmake_parse_arguments(ARG "REGISTER_TEST" "" "SOURCES;LIBS" ${ARGN})
        set(_srcs ${ARG_SOURCES})
        add_executable(${name} ${_srcs})
        set_source_files_properties(${_srcs} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
        target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        set_target_properties(${name} PROPERTIES OUTPUT_NAME "${name}" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests")
        set_property(TARGET ${name} PROPERTY MSVC_RUNTIME_LIBRARY
            "$<$<CONFIG:Debug>:MultiThreadedDebug>$<$<NOT:$<CONFIG:Debug>>:MultiThreaded>")
        target_compile_options(${name} PRIVATE
            /utf-8 /Zc:wchar_t /Zc:__cplusplus /permissive- /Zc:preprocessor /EHsc)
        target_link_libraries(${name} PRIVATE ${ARG_LIBS})
        target_link_options(${name} PRIVATE /SUBSYSTEM:CONSOLE)
        if(ARG_REGISTER_TEST)
            add_test(NAME ${name} COMMAND ${name} SKIP_RETURN_CODE 77)
        endif()
    endfunction()

    set(_gpu_common GpuDevice.cpp GpuShaders.cpp GpuTextureHelpers.cpp)
    set(_gpu_libs d3d11 dxgi d3dcompiler)

    # Verify the Apply3ChannelLUT32bpp compute pass against a CPU reference.
    add_gpu_test(GpuLutTest SOURCES GpuLutTest.cpp ${_gpu_common} LIBS ${_gpu_libs} REGISTER_TEST)
    # Verify the ApplyLDC32bpp (non-saturation) compute pass against a CPU reference.
    add_gpu_test(GpuLdcTest SOURCES GpuLdcTest.cpp ${_gpu_common} LIBS ${_gpu_libs} REGISTER_TEST)
    # Verify the LDC saturation compute pass against a CPU reference.
    add_gpu_test(GpuLdcSatTest SOURCES GpuLdcSatTest.cpp ${_gpu_common} LIBS ${_gpu_libs} REGISTER_TEST)
    # Verify the ResampleX compute pass kernel plumbing.
    add_gpu_test(GpuResampleTest SOURCES GpuResampleTest.cpp ${_gpu_common} LIBS ${_gpu_libs} REGISTER_TEST)
    # Verify the UnsharpMask compute pass against a CPU reference.
    add_gpu_test(GpuUnsharpTest SOURCES GpuUnsharpTest.cpp ${_gpu_common} LIBS ${_gpu_libs} REGISTER_TEST)
    # Verify the separable Gauss compute shader (X+Y passes) against a
    # transposed-output CPU reference.
    add_gpu_test(GpuGaussTest SOURCES GpuGaussTest.cpp ${_gpu_common} LIBS ${_gpu_libs} REGISTER_TEST)
    # Verify D3D11 device creation without launching the full WTL UI.
    add_gpu_test(GpuDeviceTest SOURCES GpuDeviceTest.cpp GpuDevice.cpp LIBS d3d11 dxgi REGISTER_TEST)
    # ShaderDiag prints D3DCompile errors for debugging. Dev-only, not a test.
    add_gpu_test(ShaderDiag SOURCES ShaderDiag.cpp GpuDevice.cpp GpuShaders.cpp LIBS ${_gpu_libs})

    # CPU algorithm unit tests (no D3D dependency - run on any CI runner).
    add_executable(CpuImageProcTest CpuImageProcTest.cpp)
    set_source_files_properties(CpuImageProcTest.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(CpuImageProcTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    set_target_properties(CpuImageProcTest PROPERTIES
        OUTPUT_NAME "CpuImageProcTest"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests")
    set_property(TARGET CpuImageProcTest PROPERTY MSVC_RUNTIME_LIBRARY
        "$<$<CONFIG:Debug>:MultiThreadedDebug>$<$<NOT:$<CONFIG:Debug>>:MultiThreaded>")
    target_compile_options(CpuImageProcTest PRIVATE
        /utf-8 /Zc:wchar_t /Zc:__cplusplus /permissive- /Zc:preprocessor /EHsc)
    target_link_options(CpuImageProcTest PRIVATE /SUBSYSTEM:CONSOLE)
    add_test(NAME CpuImageProcTest COMMAND CpuImageProcTest)
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

