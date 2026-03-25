#include "dsp/SpectrumAnalyzer.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace baudline {

SpectrumAnalyzer::SpectrumAnalyzer() {
    // Force sizeChanged=true on first configure by setting fftSize to 0.
    settings_.fftSize = 0;
    configure(AnalyzerSettings{});
}

void SpectrumAnalyzer::configure(const AnalyzerSettings& settings) {
    bool sizeChanged = settings.fftSize     != settings_.fftSize ||
                       settings.isIQ        != settings_.isIQ    ||
                       settings.numChannels != settings_.numChannels;

    settings_ = settings;

    fft_.configure(settings_.fftSize, settings_.isIQ);
    WindowFunctions::generate(settings_.window, settings_.fftSize, window_,
                              settings_.kaiserBeta);
    windowGain_ = WindowFunctions::coherentGain(window_);

    int inCh = settings_.inputChannels();
    hopSize_ = static_cast<size_t>(settings_.fftSize * (1.0f - settings_.overlap));
    if (hopSize_ < 1) hopSize_ = 1;

    if (sizeChanged) {
        accumBuf_.assign(settings_.fftSize * inCh, 0.0f);
        accumPos_ = 0;

        int nSpec = settings_.isIQ ? 1 : settings_.numChannels;
        int specSz = fft_.spectrumSize();

        channelSpectra_.assign(nSpec, std::vector<float>(specSz, -200.0f));
        channelWaterfalls_.assign(nSpec, {});

        avgAccum_.assign(nSpec, std::vector<float>(specSz, 0.0f));
        avgCount_ = 0;

        newSpectrumReady_ = false;
    }
}

void SpectrumAnalyzer::pushSamples(const float* data, size_t frames) {
    int inCh = settings_.inputChannels();
    size_t totalSamples = frames * inCh;
    size_t bufLen = static_cast<size_t>(settings_.fftSize) * inCh;
    const float* ptr = data;
    size_t remaining = totalSamples;

    newSpectrumReady_ = false;

    while (remaining > 0) {
        size_t space = bufLen - accumPos_;
        size_t toCopy = std::min(remaining, space);
        std::memcpy(accumBuf_.data() + accumPos_, ptr, toCopy * sizeof(float));
        accumPos_ += toCopy;
        ptr += toCopy;
        remaining -= toCopy;

        if (accumPos_ >= bufLen) {
            processBlock();

            // Shift by hopSize for overlap
            size_t hopSamples = hopSize_ * inCh;
            size_t keep = bufLen - hopSamples;
            std::memmove(accumBuf_.data(), accumBuf_.data() + hopSamples,
                         keep * sizeof(float));
            accumPos_ = keep;
        }
    }
}

void SpectrumAnalyzer::processBlock() {
    int N    = settings_.fftSize;
    int inCh = settings_.inputChannels();
    int nSpec = static_cast<int>(channelSpectra_.size());
    int specSz = fft_.spectrumSize();

    // Compute per-channel spectra.
    std::vector<std::vector<float>> tempDBs(nSpec);

    if (settings_.isIQ) {
        // I/Q: treat the 2 interleaved channels as one complex signal.
        std::vector<float> windowed(N * 2);
        for (int i = 0; i < N; ++i) {
            windowed[2 * i]     = accumBuf_[2 * i]     * window_[i];
            windowed[2 * i + 1] = accumBuf_[2 * i + 1] * window_[i];
        }
        fft_.processComplex(windowed.data(), tempDBs[0]);
    } else {
        // Real: deinterleave and FFT each channel independently.
        std::vector<float> chanBuf(N);
        for (int ch = 0; ch < nSpec; ++ch) {
            // Deinterleave channel `ch` from the accumulation buffer.
            for (int i = 0; i < N; ++i)
                chanBuf[i] = accumBuf_[i * inCh + ch];
            WindowFunctions::apply(window_, chanBuf.data(), N);
            fft_.processReal(chanBuf.data(), tempDBs[ch]);
        }
    }

    // Correct for window gain.
    float correction = -20.0f * std::log10(windowGain_ > 0 ? windowGain_ : 1.0f);
    for (auto& db : tempDBs)
        for (float& v : db)
            v += correction;

    // Averaging.
    if (settings_.averaging > 1) {
        if (static_cast<int>(avgAccum_[0].size()) != specSz) {
            for (auto& a : avgAccum_) a.assign(specSz, 0.0f);
            avgCount_ = 0;
        }
        for (int ch = 0; ch < nSpec; ++ch)
            for (int i = 0; i < specSz; ++i)
                avgAccum_[ch][i] += tempDBs[ch][i];
        avgCount_++;

        if (avgCount_ >= settings_.averaging) {
            for (int ch = 0; ch < nSpec; ++ch)
                for (int i = 0; i < specSz; ++i)
                    tempDBs[ch][i] = avgAccum_[ch][i] / avgCount_;
            for (auto& a : avgAccum_) a.assign(specSz, 0.0f);
            avgCount_ = 0;
        } else {
            return;
        }
    }

    // Store results.
    for (int ch = 0; ch < nSpec; ++ch) {
        channelSpectra_[ch] = tempDBs[ch];
        channelWaterfalls_[ch].push_back(tempDBs[ch]);
        if (channelWaterfalls_[ch].size() > kWaterfallHistory)
            channelWaterfalls_[ch].pop_front();
    }
    newSpectrumReady_ = true;
}

std::pair<int, float> SpectrumAnalyzer::findPeak(int ch) const {
    if (ch < 0 || ch >= static_cast<int>(channelSpectra_.size()) ||
        channelSpectra_[ch].empty())
        return {0, -200.0f};
    const auto& spec = channelSpectra_[ch];
    auto it = std::max_element(spec.begin(), spec.end());
    int idx = static_cast<int>(std::distance(spec.begin(), it));
    return {idx, *it};
}

double SpectrumAnalyzer::binToFreq(int bin) const {
    double sr = settings_.sampleRate;
    int N = settings_.fftSize;

    if (settings_.isIQ) {
        return -sr / 2.0 + (static_cast<double>(bin) / N) * sr;
    } else {
        return (static_cast<double>(bin) / N) * sr;
    }
}

void SpectrumAnalyzer::clearHistory() {
    for (auto& w : channelWaterfalls_) w.clear();
    newSpectrumReady_ = false;
}

} // namespace baudline
