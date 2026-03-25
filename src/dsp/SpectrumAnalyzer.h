#pragma once

#include "core/Types.h"
#include "dsp/FFTProcessor.h"
#include "dsp/WindowFunctions.h"
#include <deque>
#include <vector>

namespace baudline {

// Manages the DSP pipeline: accumulation with overlap, windowing, FFT,
// averaging, and waterfall history.
//
// Supports three modes:
//   - Mono real   (numChannels=1, isIQ=false): 1 real FFT  → 1 spectrum
//   - Multi-ch real (numChannels>1, isIQ=false): N real FFTs → N spectra
//   - I/Q complex (isIQ=true):                 1 complex FFT → 1 spectrum
class SpectrumAnalyzer {
public:
    SpectrumAnalyzer();

    void configure(const AnalyzerSettings& settings);
    const AnalyzerSettings& settings() const { return settings_; }

    // Feed raw interleaved audio samples.
    // `frames` = number of sample frames (1 frame = inputChannels() samples).
    void pushSamples(const float* data, size_t frames);

    // Returns true if a new spectrum line is available since last call.
    bool hasNewSpectrum() const { return newSpectrumReady_; }

    // Number of independent spectra (1 for mono/IQ, numChannels for multi-ch).
    int numSpectra() const { return static_cast<int>(channelSpectra_.size()); }

    // Per-channel spectra (dB magnitudes).
    const std::vector<float>& channelSpectrum(int ch) const { return channelSpectra_[ch]; }

    // Convenience: first channel spectrum (backward compat / primary).
    const std::vector<float>& currentSpectrum() const { return channelSpectra_[0]; }

    // All channel spectra.
    const std::vector<std::vector<float>>& allSpectra() const { return channelSpectra_; }

    // Number of output bins (per channel).
    int spectrumSize() const { return fft_.spectrumSize(); }

    // Peak detection on a given channel.
    std::pair<int, float> findPeak(int ch = 0) const;

    // Get frequency for a given bin index.
    double binToFreq(int bin) const;

    void clearHistory();

    // Waterfall history for a given channel (most recent = back).
    const std::deque<std::vector<float>>& waterfallHistory(int ch = 0) const {
        return channelWaterfalls_[ch];
    }

private:
    void processBlock();

    AnalyzerSettings    settings_;
    FFTProcessor        fft_;
    std::vector<float>  window_;
    float               windowGain_ = 1.0f;

    // Accumulation buffer (interleaved, length = fftSize * inputChannels)
    std::vector<float>  accumBuf_;
    size_t              accumPos_ = 0;
    size_t              hopSize_  = 0;

    // Per-channel averaging
    std::vector<std::vector<float>> avgAccum_;
    int                             avgCount_ = 0;

    // Per-channel output
    std::vector<std::vector<float>>                channelSpectra_;
    std::vector<std::deque<std::vector<float>>>     channelWaterfalls_;
    bool                                            newSpectrumReady_ = false;
};

} // namespace baudline
