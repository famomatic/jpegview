# ---------------------------------------------------------------------------
# Test targets for the GPU compute passes and CPU image-processing math.
#
# These are development/CI tests registered with CTest, gated by the
# JPEGVIEW_BUILD_TESTS option (set from the Debug CMake presets and by CI).
# The test sources live here in tests/, but they compile the GPU backend
# sources (GpuDevice.cpp / GpuShaders.cpp / GpuTextureHelpers.cpp) from
# src/JPEGView, so that directory is on the include path. When the option is
# off, no test targets exist and Release builds never emit *Test.exe.
# ---------------------------------------------------------------------------

set(_gpu_src_dir "${CMAKE_SOURCE_DIR}/src/JPEGView")

# add_gpu_test: builds a standalone console exe for a GPU compute-pass test.
# All GPU tests share the same source set (GpuDevice.cpp + the shader/texture
# helpers) and the same MSVC/link settings, so this keeps the per-test
# boilerplate to one line. Register the test with CTest unless REGISTER_TEST is
# OFF (used for diagnostic tools that have no pass/fail).
function(add_gpu_test name)
    cmake_parse_arguments(ARG "REGISTER_TEST" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    set_source_files_properties(${ARG_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(${name} PRIVATE "${_gpu_src_dir}")
    set_target_properties(${name} PROPERTIES
        OUTPUT_NAME "${name}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests")
    set_property(TARGET ${name} PROPERTY MSVC_RUNTIME_LIBRARY
        "$<$<CONFIG:Debug>:MultiThreadedDebug>$<$<NOT:$<CONFIG:Debug>>:MultiThreaded>")
    target_compile_options(${name} PRIVATE
        /utf-8 /Zc:wchar_t /Zc:__cplusplus /permissive- /Zc:preprocessor /EHsc)
    target_link_libraries(${name} PRIVATE ${ARG_LIBS})
    target_link_options(${name} PRIVATE /SUBSYSTEM:CONSOLE)
    if(ARG_REGISTER_TEST)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endfunction()

set(_gpu_common
    "${_gpu_src_dir}/GpuDevice.cpp"
    "${_gpu_src_dir}/GpuShaders.cpp"
    "${_gpu_src_dir}/GpuTextureHelpers.cpp")
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
add_gpu_test(GpuDeviceTest SOURCES GpuDeviceTest.cpp "${_gpu_src_dir}/GpuDevice.cpp" LIBS d3d11 dxgi REGISTER_TEST)

# Verify the highway HQ resampling stays correct for every dispatched SIMD
# lane width (forces SSE2/SSE4 via hwy::SetSupportedTargetsForTest) and the
# scalar SampleDown_HQ fallback. Catches lane-vs-16-pixel-granularity
# mismatches that only reproduce on non-AVX2 hardware.
add_gpu_test(HighwayLaneTest SOURCES HighwayLaneTest.cpp
    "${_gpu_src_dir}/BasicProcessing.cpp"
    "${_gpu_src_dir}/ApplyFilterHighway.cpp"
    "${_gpu_src_dir}/XMMImage.cpp"
    "${_gpu_src_dir}/ResizeFilter.cpp"
    "${_gpu_src_dir}/ProcessingThreadPool.cpp"
    "${_gpu_src_dir}/WorkThread.cpp"
    LIBS hwy::hwy REGISTER_TEST)
target_include_directories(HighwayLaneTest PRIVATE "${WTL_INCLUDE_DIR}")
target_compile_definitions(HighwayLaneTest PRIVATE
    WIN32 _WINDOWS STRICT UNICODE _UNICODE _CRT_SECURE_NO_DEPRECATE)

# ShaderDiag prints D3DCompile errors for debugging. Dev-only, not a test.
# Off by default so it never appears in any output tree; enable explicitly
# during development with -DJPEGVIEW_BUILD_SHADER_DIAG=ON.
option(JPEGVIEW_BUILD_SHADER_DIAG "Build the ShaderDiag dev tool (Debug-only)" OFF)
if(JPEGVIEW_BUILD_SHADER_DIAG)
    add_gpu_test(ShaderDiag SOURCES ShaderDiag.cpp "${_gpu_src_dir}/GpuDevice.cpp" "${_gpu_src_dir}/GpuShaders.cpp" LIBS ${_gpu_libs})
endif()

# CPU algorithm unit tests (no D3D dependency - run on any CI runner).
add_executable(CpuImageProcTest CpuImageProcTest.cpp)
set_source_files_properties(CpuImageProcTest.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
set_target_properties(CpuImageProcTest PROPERTIES
    OUTPUT_NAME "CpuImageProcTest"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tests")
set_property(TARGET CpuImageProcTest PROPERTY MSVC_RUNTIME_LIBRARY
    "$<$<CONFIG:Debug>:MultiThreadedDebug>$<$<NOT:$<CONFIG:Debug>>:MultiThreaded>")
    target_compile_options(CpuImageProcTest PRIVATE
        /utf-8 /Zc:wchar_t /Zc:__cplusplus /permissive- /Zc:preprocessor /EHsc)
    target_link_options(CpuImageProcTest PRIVATE /SUBSYSTEM:CONSOLE)
    add_test(NAME CpuImageProcTest COMMAND CpuImageProcTest)
