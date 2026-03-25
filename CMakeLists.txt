cmake_minimum_required(VERSION 3.16)
project(baudline VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type defaults
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── ImGui via FetchContent (shared by native and WASM) ───────────────────────

include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.8
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

# ── Common sources ───────────────────────────────────────────────────────────

set(SOURCES
    src/main.cpp
    src/core/Config.cpp
    src/dsp/WindowFunctions.cpp
    src/dsp/FFTProcessor.cpp
    src/dsp/SpectrumAnalyzer.cpp
    src/audio/MiniAudioSource.cpp
    src/audio/FileSource.cpp
    src/ui/ColorMap.cpp
    src/ui/WaterfallDisplay.cpp
    src/ui/SpectrumDisplay.cpp
    src/ui/Cursors.cpp
    src/ui/Measurements.cpp
    src/ui/Application.cpp
)

if(EMSCRIPTEN)
    # ── WASM Build ───────────────────────────────────────────────────────────

    # Add WavReader (replaces libsndfile on WASM)
    list(APPEND SOURCES src/audio/WavReader.cpp)

    # Fetch FFTW3 source (don't build via its own CMake — it fails with Emscripten)
    FetchContent_Declare(
        fftw3
        URL      http://www.fftw.org/fftw-3.3.10.tar.gz
        URL_HASH MD5=8ccbf6a5ea78a16dbc3e1306e234cc5c
    )
    FetchContent_GetProperties(fftw3)
    if(NOT fftw3_POPULATED)
        FetchContent_Populate(fftw3)
    endif()

    # Build a minimal single-precision FFTW3 static lib
    file(GLOB FFTW_KERNEL_SRCS ${fftw3_SOURCE_DIR}/kernel/*.c)
    file(GLOB FFTW_DFT_SRCS    ${fftw3_SOURCE_DIR}/dft/*.c)
    file(GLOB FFTW_RDFT_SRCS   ${fftw3_SOURCE_DIR}/rdft/*.c
                                ${fftw3_SOURCE_DIR}/rdft/scalar/*.c
                                ${fftw3_SOURCE_DIR}/rdft/scalar/r2cf/*.c
                                ${fftw3_SOURCE_DIR}/rdft/scalar/r2cb/*.c
                                ${fftw3_SOURCE_DIR}/rdft/scalar/r2r/*.c)
    file(GLOB FFTW_DFT_SCALAR  ${fftw3_SOURCE_DIR}/dft/scalar/*.c
                                ${fftw3_SOURCE_DIR}/dft/scalar/codelets/*.c)
    file(GLOB FFTW_REODFT_SRCS ${fftw3_SOURCE_DIR}/reodft/*.c)
    file(GLOB FFTW_API_SRCS    ${fftw3_SOURCE_DIR}/api/*.c)

    # Generate config.h for FFTW (Emscripten/WASM compatible)
    file(WRITE ${fftw3_BINARY_DIR}/config.h
"#ifndef FFTW_CONFIG_H
#define FFTW_CONFIG_H
#define FFTW_SINGLE 1
#define SIZEOF_VOID_P 4
#define SIZEOF_SIZE_T 4
#define SIZEOF_PTRDIFF_T 4
#define SIZEOF_INT 4
#define SIZEOF_LONG 4
#define SIZEOF_LONG_LONG 8
#define SIZEOF_UNSIGNED_INT 4
#define SIZEOF_UNSIGNED_LONG 4
#define SIZEOF_UNSIGNED_LONG_LONG 8
#define HAVE_ABORT 1
#define HAVE_UNISTD_H 1
#define HAVE_STRING_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_MEMORY_H 1
#define HAVE_MATH_H 1
#define HAVE_ERRNO_H 1
#define HAVE_ISNAN 1
#define HAVE_UINTPTR_T 1
#define HAVE_SNPRINTF 1
#define HAVE_SYS_TIME_H 1
#define HAVE_GETTIMEOFDAY 1
#define FFTW_CC \"emcc\"
#define CODELET_OPTIM \"\"
#define PACKAGE \"fftw\"
#define PACKAGE_VERSION \"3.3.10\"
#define VERSION \"3.3.10\"
#endif
")

    add_library(fftw3f_wasm STATIC
        ${FFTW_KERNEL_SRCS}
        ${FFTW_DFT_SRCS}
        ${FFTW_DFT_SCALAR}
        ${FFTW_RDFT_SRCS}
        ${FFTW_REODFT_SRCS}
        ${FFTW_API_SRCS}
    )
    target_include_directories(fftw3f_wasm PUBLIC
        ${fftw3_SOURCE_DIR}/api
        ${fftw3_SOURCE_DIR}/kernel
        ${fftw3_SOURCE_DIR}/dft
        ${fftw3_SOURCE_DIR}/dft/scalar
        ${fftw3_SOURCE_DIR}/rdft
        ${fftw3_SOURCE_DIR}/rdft/scalar
        ${fftw3_SOURCE_DIR}/reodft
        ${fftw3_SOURCE_DIR}
        ${fftw3_BINARY_DIR}
    )
    target_compile_definitions(fftw3f_wasm PRIVATE FFTW_SINGLE=1)
    target_compile_options(fftw3f_wasm PRIVATE -w)  # suppress warnings from FFTW

    # ImGui: link with Emscripten SDL2 port
    target_compile_options(imgui PUBLIC -sUSE_SDL=2)
    target_link_options(imgui PUBLIC -sUSE_SDL=2)

    add_executable(baudline ${SOURCES})
    target_include_directories(baudline PRIVATE src)
    target_link_libraries(baudline PRIVATE imgui fftw3f_wasm)

    # Emscripten linker flags
    target_link_options(baudline PRIVATE
        -sUSE_SDL=2
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=67108864
        -sSTACK_SIZE=1048576
        -sASYNCIFY
        -sASYNCIFY_STACK_SIZE=65536
        -sEXPORTED_RUNTIME_METHODS=ccall
        --shell-file=${CMAKE_SOURCE_DIR}/web/shell.html
    )

    # Output baudline.html + .js + .wasm
    set_target_properties(baudline PROPERTIES
        SUFFIX ".html"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web"
    )

    set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "" FORCE)

else()
    # ── Native Build ─────────────────────────────────────────────────────────

    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")

    find_package(PkgConfig REQUIRED)
    find_package(OpenGL REQUIRED)

    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)
    pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)

    target_link_libraries(imgui PUBLIC PkgConfig::SDL2 OpenGL::GL)

    add_executable(baudline ${SOURCES})
    target_include_directories(baudline PRIVATE src)
    target_link_libraries(baudline PRIVATE
        imgui
        PkgConfig::SDL2
        PkgConfig::FFTW3F
        PkgConfig::SNDFILE
        OpenGL::GL
        pthread
    )

    # Link math library and dl on Unix (dl needed by miniaudio for backend loading)
    if(UNIX)
        target_link_libraries(baudline PRIVATE m dl)
    endif()

endif()
