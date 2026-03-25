cmake_minimum_required(VERSION 3.16)
project(baudline VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type defaults
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fsanitize=address,undefined")

# ── Dependencies ──────────────────────────────────────────────────────────────

find_package(PkgConfig REQUIRED)
find_package(OpenGL REQUIRED)

pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)
pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)

# ── ImGui via FetchContent ────────────────────────────────────────────────────

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
target_link_libraries(imgui PUBLIC PkgConfig::SDL2 OpenGL::GL)

# ── Application ───────────────────────────────────────────────────────────────

set(SOURCES
    src/main.cpp
    src/core/Config.cpp
    src/dsp/WindowFunctions.cpp
    src/dsp/FFTProcessor.cpp
    src/dsp/SpectrumAnalyzer.cpp
    src/audio/PortAudioSource.cpp
    src/audio/FileSource.cpp
    src/ui/ColorMap.cpp
    src/ui/WaterfallDisplay.cpp
    src/ui/SpectrumDisplay.cpp
    src/ui/Cursors.cpp
    src/ui/Application.cpp
)

add_executable(baudline ${SOURCES})
target_include_directories(baudline PRIVATE src)
target_link_libraries(baudline PRIVATE
    imgui
    PkgConfig::SDL2
    PkgConfig::FFTW3F
    PkgConfig::PORTAUDIO
    PkgConfig::SNDFILE
    OpenGL::GL
    pthread
)

# Link math library on Unix
if(UNIX)
    target_link_libraries(baudline PRIVATE m)
endif()
