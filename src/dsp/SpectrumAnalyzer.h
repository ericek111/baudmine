#pragma once

#include "core/Types.h"
#include "dsp/FFTProcessor.h"
#include "dsp/WindowFunctions.h"
#include <complex>
#include <deque>
#include <vector>

namespace baudline {

class SpectrumAnalyzer {
public:
    SpectrumAnalyzer();

    void configure(const AnalyzerSettings& settings);
    const AnalyzerSettings& settings() const { return settings_; }

    void pushSamples(const float* data, size_t frames);

    bool hasNewSpectrum() const { return newSpectrumReady_; }

    // Number of physical spectra (1 for mono/IQ, numChannels for multi-ch).
    int numSpectra() const { return static_cast<int>(channelSpectra_.size()); }

    // Per-channel dB magnitude spectrum.
    const std::vector<float>& channelSpectrum(int ch) const { return channelSpectra_[ch]; }
    const std::vector<float>& currentSpectrum() const { return channelSpectra_[0]; }
    const std::vector<std::vector<float>>& allSpectra() const { return channelSpectra_; }

    // Per-channel complex spectrum (for math ops like phase, cross-correlation).
    const std::vector<std::complex<float>>& channelComplex(int ch) const {
        return channelComplex_[ch];
    }
    const std::vector<std::vector<std::complex<float>>>& allComplex() const {
        return channelComplex_;
    }

    int spectrumSize() const { return fft_.spectrumSize(); }

    std::pair<int, float> findPeak(int ch = 0) const;
    double binToFreq(int bin) const;
    void clearHistory();

    const std::deque<std::vector<float>>& waterfallHistory(int ch = 0) const {
        return channelWaterfalls_[ch];
    }

private:
    void processBlock();

    AnalyzerSettings    settings_;
    FFTProcessor        fft_;
    std::vector<float>  window_;
    float               windowGain_ = 1.0f;

    std::vector<float>  accumBuf_;
    size_t              accumPos_ = 0;
    size_t              hopSize_  = 0;

    // Per-channel averaging
    std::vector<std::vector<float>> avgAccum_;
    int                             avgCount_ = 0;

    // Per-channel output: magnitude (dB) and complex
    std::vector<std::vector<float>>                  channelSpectra_;
    std::vector<std::vector<std::complex<float>>>    channelComplex_;
    std::vector<std::deque<std::vector<float>>>      channelWaterfalls_;
    bool                                             newSpectrumReady_ = false;
};

} // namespace baudline
