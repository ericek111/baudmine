# Baudmine Spectrum Analyzer

Real-time audio spectrum analyzer and waterfall display built with C++17, ImGui, SDL2, and OpenGL. Runs natively on Linux and Windows, and in the browser via WebAssembly.

Beware, this thing is **entirely LLM-generated**! I've barely looked at the code! Anthropic had a promo for Claude, so I utilized it to make this clone of [**baudline**](https://www.baudline.com/what_is_baudline.html), which is a very powerful (and proprietary) tool, but sadly it's been seemingly abandoned for years, not that it would need anything. My hope was to put together something similar, using a more modern stack. It is not (yet) a fully-featured replacement, but it's good enough for me.

## Web preview

You can find the Web version here, delivered in 870 kB: https://ericek111.github.io/baudmine/

(If you allow access to "all microphones", you can switch between the input devices in the File menu.)

## Features

### Signal Processing
- FFT sizes from 256 to 65,536 points with configurable overlap of bins for precise speed control of the waterfall.
- Window functions: Hann, Hamming, Blackman, Blackman-Harris, Kaiser (configurable beta), Flat Top, Rectangular

### Input Sources
- Live audio from system input devices
- Multi-device mode: use up to 8 devices simultaneously, each mapped to a channel
- File playback: WAV, Float32/Int16/Uint8 I/Q formats

### Visualization
- Spectrum plot
- Scrolling waterfall display that can be zoomed and panned
- Linear and logarithmic frequency scales
- Additive color blending for multi-channel display
- Peak hold with adjustable decay rate

### Analysis
- Dual cursors with frequency and magnitude readout
- Automatic snap-to-peak
- Peak detection with configurable threshold, separation, and frequency range (it shows the center frequency of the peak-detected FFT bin)
- Peak trace on the waterfall
- Delta measurements between cursors

### Math Channels
- Unary: Negate, Absolute, Square, Cube, Square Root, Log
- Binary: Add, Subtract, Multiply, Phase, Cross-correlation
- Custom colors per math channel

### Interface
- DPI-aware UI with auto-detection and manual scaling (100-300%)
- Touch gesture support in the waterfall (pan and pinch-to-zoom)
- The most important settings persist across sessions

## Building

### Linux

```bash
# Install dependencies (Debian/Ubuntu)
sudo apt install build-essential cmake pkg-config libsdl2-dev libfftw3-dev libsndfile1-dev libgl-dev

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Windows (MSVC + vcpkg)

```powershell
vcpkg install sdl2 fftw3 "libsndfile[core]" --triplet x64-windows

cmake -B build -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release
```
