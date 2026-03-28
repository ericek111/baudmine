#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <complex>
#include <string>
#include <vector>

namespace baudmine {

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

// ── Formatting helpers ───────────────────────────────────────────────────────

// Format frequency into buf with fixed-width numeric field per unit range.
inline int fmtFreq(char* buf, size_t sz, double freq) {
    if (std::abs(freq) >= 1e6)
        return std::snprintf(buf, sz, "% 10.6f MHz", freq / 1e6);
    else if (std::abs(freq) >= 1e3)
        return std::snprintf(buf, sz, "% 7.3f kHz", freq / 1e3);
    else
        return std::snprintf(buf, sz, "% 7.1f Hz", freq);
}

// Format "label: freq dB" into buf for sidebar display (fixed-width freq + dB).
// If label is null or empty, omits the "label: " prefix.
inline int fmtFreqDB(char* buf, size_t sz, const char* label, double freq, float dB) {
    int off = 0;
    if (label && label[0])
        off = std::snprintf(buf, sz, "%s: ", label);
    if (std::abs(freq) >= 1e6)
        off += std::snprintf(buf + off, sz - off, "% 10.6f MHz %6.1f dB", freq / 1e6, dB);
    else if (std::abs(freq) >= 1e3)
        off += std::snprintf(buf + off, sz - off, "% 7.3f kHz %6.1f dB", freq / 1e3, dB);
    else
        off += std::snprintf(buf + off, sz - off, " % 7.1f Hz %6.1f dB", freq, dB);
    return off;
}

// Format "label: freq, time" into buf for sidebar/overlay display (fixed-width).
inline int fmtFreqTime(char* buf, size_t sz, const char* label, double freq, float seconds) {
    int off = 0;
    if (label && label[0])
        off = std::snprintf(buf, sz, "%s: ", label);
    if (std::abs(freq) >= 1e6)
        off += std::snprintf(buf + off, sz - off, "% 10.6f MHz %7.2f s", freq / 1e6, seconds);
    else if (std::abs(freq) >= 1e3)
        off += std::snprintf(buf + off, sz - off, "% 7.3f kHz %7.2f s", freq / 1e3, seconds);
    else
        off += std::snprintf(buf + off, sz - off, " % 7.1f Hz %7.2f s", freq, seconds);
    return off;
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

    // Effective input channel count (for buffer sizing / deinterleaving).
    int inputChannels() const { return isIQ ? 2 : numChannels; }
};

// ── Color ────────────────────────────────────────────────────────────────────

struct Color3 {
    uint8_t r, g, b;
};

// ── Channel math operations ──────────────────────────────────────────────────

enum class MathOp {
    // Unary (on channel X)
    Negate,       // -x  (negate dB)
    Absolute,     // |x| (absolute value of dB)
    Square,       // x^2 in linear  → 2*x_dB
    Cube,         // x^3 in linear  → 3*x_dB
    Sqrt,         // sqrt in linear → 0.5*x_dB
    Log,          // 10*log10(10^(x_dB/10) + 1)  — compressed scale
    // Binary (on channels X and Y)
    Add,          // linear(x) + linear(y)  → dB
    Subtract,     // |linear(x) - linear(y)| → dB
    Multiply,     // x_dB + y_dB  (multiply in linear = add in dB)
    Phase,        // angle(X_cplx * conj(Y_cplx)) in degrees
    CrossCorr,    // |X_cplx * conj(Y_cplx)| → dB
    Count
};

inline bool mathOpIsBinary(MathOp op) {
    return op >= MathOp::Add;
}

inline const char* mathOpName(MathOp op) {
    switch (op) {
        case MathOp::Negate:    return "-x";
        case MathOp::Absolute:  return "|x|";
        case MathOp::Square:    return "x^2";
        case MathOp::Cube:      return "x^3";
        case MathOp::Sqrt:      return "sqrt(x)";
        case MathOp::Log:       return "log(x)";
        case MathOp::Add:       return "x + y";
        case MathOp::Subtract:  return "x - y";
        case MathOp::Multiply:  return "x * y";
        case MathOp::Phase:     return "phase(x,y)";
        case MathOp::CrossCorr: return "xcorr(x,y)";
        default:                return "?";
    }
}

struct MathChannel {
    MathOp op        = MathOp::Subtract;
    int    sourceX   = 0;
    int    sourceY   = 1;
    float  color[4]  = {1.0f, 1.0f, 1.0f, 1.0f};
    bool   enabled   = true;
    bool   waterfall = false;
};

} // namespace baudmine
