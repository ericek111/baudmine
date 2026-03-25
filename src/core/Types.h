#pragma once

#include <cstddef>
#include <cstdint>
#include <complex>
#include <string>
#include <vector>

namespace baudline {

// ── FFT configuration ────────────────────────────────────────────────────────

constexpr int kMinFFTSize = 256;
constexpr int kMaxFFTSize = 65536;
constexpr int kDefaultFFTSize = 4096;
constexpr int kWaterfallHistory = 2048;

// ── Enumerations ─────────────────────────────────────────────────────────────

enum class WindowType {
    Rectangular,
    Hann,
    Hamming,
    Blackman,
    BlackmanHarris,
    Kaiser,
    FlatTop,
    Count
};

inline const char* windowName(WindowType w) {
    switch (w) {
        case WindowType::Rectangular:   return "Rectangular";
        case WindowType::Hann:          return "Hann";
        case WindowType::Hamming:       return "Hamming";
        case WindowType::Blackman:      return "Blackman";
        case WindowType::BlackmanHarris: return "Blackman-Harris";
        case WindowType::Kaiser:        return "Kaiser";
        case WindowType::FlatTop:       return "Flat Top";
        default:                        return "Unknown";
    }
}

enum class FreqScale {
    Linear,
    Logarithmic
};

enum class ColorMapType {
    Magma,
    Viridis,
    Inferno,
    Plasma,
    Grayscale,
    Count
};

inline const char* colorMapName(ColorMapType c) {
    switch (c) {
        case ColorMapType::Magma:     return "Magma";
        case ColorMapType::Viridis:   return "Viridis";
        case ColorMapType::Inferno:   return "Inferno";
        case ColorMapType::Plasma:    return "Plasma";
        case ColorMapType::Grayscale: return "Grayscale";
        default:                      return "Unknown";
    }
}

enum class InputFormat {
    Float32IQ,
    Int16IQ,
    Uint8IQ,
    WAV,
    PortAudio
};

inline const char* inputFormatName(InputFormat f) {
    switch (f) {
        case InputFormat::Float32IQ: return "Float32 I/Q";
        case InputFormat::Int16IQ:   return "Int16 I/Q";
        case InputFormat::Uint8IQ:   return "Uint8 I/Q";
        case InputFormat::WAV:       return "WAV File";
        case InputFormat::PortAudio: return "PortAudio";
        default:                     return "Unknown";
    }
}

// ── Spectrum data ────────────────────────────────────────────────────────────

struct SpectrumLine {
    std::vector<float> magnitudeDB;  // power in dB, length = fftSize/2 (real) or fftSize (IQ)
    double             centerFreq;   // Hz (0 for real signals)
    double             bandwidth;    // Hz (= sampleRate for IQ, sampleRate/2 for real)
};

constexpr int kMaxChannels = 8;

struct AnalyzerSettings {
    int        fftSize     = kDefaultFFTSize;
    float      overlap     = 0.5f;          // 0.0 – 0.875
    WindowType window      = WindowType::BlackmanHarris;
    float      kaiserBeta  = 9.0f;
    bool       isIQ        = false;         // true → complex input (2-ch interleaved)
    int        numChannels = 1;             // real channels (ignored when isIQ)
    double     sampleRate  = 48000.0;
    int        averaging   = 1;             // number of spectra to average (1 = none)

    // Effective input channel count (for buffer sizing / deinterleaving).
    int inputChannels() const { return isIQ ? 2 : numChannels; }
};

// ── Color ────────────────────────────────────────────────────────────────────

struct Color3 {
    uint8_t r, g, b;
};

} // namespace baudline
