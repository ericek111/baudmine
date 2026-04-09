#include "audio/AudioEngine.h"
#include "audio/FileSource.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace baudmine {

namespace {
constexpr float kLinearEpsilon = 1e-20f;  // threshold for log10 of linear power
constexpr float kLogGuard      = 1e-30f;  // guard against log10(0) in compressed scale
} // namespace

AudioEngine::AudioEngine() = default;

// ── Device enumeration ───────────────────────────────────────────────────────

void AudioEngine::enumerateDevices() {
    devices_ = MiniAudioSource::listInputDevices();
}

void AudioEngine::clearDeviceSelections() {
    std::memset(deviceSelected_, 0, sizeof(deviceSelected_));
}

// ── Source management ────────────────────────────────────────────────────────

void AudioEngine::closeAll() {
    if (audioSource_) audioSource_->close();
    audioSource_.reset();
    extraDevices_.clear();
}

void AudioEngine::openDevice(int deviceListIdx) {
    closeAll();

    int deviceIdx = -1;
    double sr = 48000.0;
    if (deviceListIdx >= 0 && deviceListIdx < static_cast<int>(devices_.size())) {
        deviceIdx = devices_[deviceListIdx].index;
        sr = devices_[deviceListIdx].defaultSampleRate;
    }

    int reqCh = 2;
    if (deviceListIdx >= 0 && deviceListIdx < static_cast<int>(devices_.size()))
        reqCh = std::min(devices_[deviceListIdx].maxInputChannels, kMaxChannels);
    if (reqCh < 1) reqCh = 1;

    auto src = std::make_unique<MiniAudioSource>(sr, reqCh, deviceIdx);
    if (src->open()) {
        audioSource_ = std::move(src);
        settings_.sampleRate = audioSource_->sampleRate();
        settings_.isIQ = false;
        settings_.numChannels = audioSource_->channels();
    } else {
        std::fprintf(stderr, "Failed to open audio device\n");
    }
}

void AudioEngine::openMultiDevice(const bool selected[], int maxDevs) {
    closeAll();

    std::vector<int> sel;
    for (int i = 0; i < maxDevs; ++i)
        if (selected[i]) sel.push_back(i);
    if (sel.empty()) return;

    // First selected device becomes the primary source.
    {
        int idx = sel[0];
        double sr = devices_[idx].defaultSampleRate;
        int reqCh = std::min(devices_[idx].maxInputChannels, kMaxChannels);
        if (reqCh < 1) reqCh = 1;
        auto src = std::make_unique<MiniAudioSource>(sr, reqCh, devices_[idx].index);
        if (src->open()) {
            audioSource_ = std::move(src);
            settings_.sampleRate = audioSource_->sampleRate();
            settings_.isIQ = false;
            settings_.numChannels = audioSource_->channels();
        } else {
            std::fprintf(stderr, "Failed to open primary device %s\n",
                         devices_[idx].name.c_str());
            return;
        }
    }

    // Remaining selected devices become extra sources.
    int totalCh = settings_.numChannels;
    for (size_t s = 1; s < sel.size() && totalCh < kMaxChannels; ++s) {
        int idx = sel[s];
        double sr = devices_[idx].defaultSampleRate;
        int reqCh = std::min(devices_[idx].maxInputChannels, kMaxChannels - totalCh);
        if (reqCh < 1) reqCh = 1;
        auto src = std::make_unique<MiniAudioSource>(sr, reqCh, devices_[idx].index);
        if (src->open()) {
            auto ed = std::make_unique<ExtraDevice>();
            ed->source = std::move(src);
            AnalyzerSettings es = settings_;
            es.sampleRate = ed->source->sampleRate();
            es.numChannels = ed->source->channels();
            es.isIQ = false;
            ed->analyzer.configure(es);
            totalCh += ed->source->channels();
            extraDevices_.push_back(std::move(ed));
        } else {
            std::fprintf(stderr, "Failed to open extra device %s\n",
                         devices_[idx].name.c_str());
        }
    }
}

void AudioEngine::openFile(const std::string& path, InputFormat format,
                            double sampleRate, bool loop) {
    closeAll();

    bool isIQ = (format != InputFormat::WAV);
    auto src = std::make_unique<FileSource>(path, format, sampleRate, loop);
    if (src->open()) {
        settings_.sampleRate = src->sampleRate();
        settings_.isIQ = isIQ;
        settings_.numChannels = isIQ ? 1 : src->channels();
        audioSource_ = std::move(src);
    } else {
        std::fprintf(stderr, "Failed to open file: %s\n", path.c_str());
    }
}

// ── Analyzer ─────────────────────────────────────────────────────────────────

void AudioEngine::configure(const AnalyzerSettings& s) {
    settings_ = s;
    analyzer_.configure(settings_);

    for (auto& ed : extraDevices_) {
        AnalyzerSettings es = settings_;
        es.sampleRate = ed->source->sampleRate();
        es.numChannels = ed->source->channels();
        es.isIQ = false;
        ed->analyzer.configure(es);
    }
}

void AudioEngine::clearHistory() {
    analyzer_.clearHistory();
    for (auto& ed : extraDevices_)
        ed->analyzer.clearHistory();
}

int AudioEngine::processAudio() {
    if (!audioSource_) return 0;

    int channels = audioSource_->channels();
    size_t hopFrames = static_cast<size_t>(
        settings_.fftSize * (1.0f - settings_.overlap));
    if (hopFrames < 1) hopFrames = 1;
    audioBuf_.resize(hopFrames * channels);

    // Drain all available audio so the scroll rate is independent of the
    // display refresh rate (vsync).  Real-time sources self-limit via their
    // ring buffer; file sources are capped to wall-clock time so playback
    // runs at 1× speed regardless of frame rate.

    // For file sources, compute how many samples correspond to elapsed time.
    size_t fileSampleCap = SIZE_MAX;  // unlimited for real-time
    if (!audioSource_->isRealTime()) {
        using Clock = std::chrono::steady_clock;
        static Clock::time_point lastFileTime = Clock::now();
        auto now = Clock::now();
        double elapsed = std::chrono::duration<double>(now - lastFileTime).count();
        lastFileTime = now;
        // Clamp elapsed to avoid huge bursts after pauses or stalls.
        if (elapsed > 0.1) elapsed = 0.1;
        fileSampleCap = static_cast<size_t>(elapsed * settings_.sampleRate) + 1;
    }

    // Process primary source.
    int spectraThisFrame = 0;
    size_t samplesRead = 0;
    for (;;) {
        if (samplesRead >= fileSampleCap) break;
        size_t framesRead = audioSource_->read(audioBuf_.data(), hopFrames);
        if (framesRead == 0) break;
        samplesRead += framesRead;
        analyzer_.pushSamples(audioBuf_.data(), framesRead);
        if (analyzer_.hasNewSpectrum())
            ++spectraThisFrame;
    }

    // Process extra devices independently (always real-time).
    for (auto& ed : extraDevices_) {
        int edCh = ed->source->channels();
        const auto& edSettings = ed->analyzer.settings();
        size_t edHop = static_cast<size_t>(edSettings.fftSize * (1.0f - edSettings.overlap));
        if (edHop < 1) edHop = 1;
        ed->audioBuf.resize(edHop * edCh);

        for (;;) {
            size_t framesRead = ed->source->read(ed->audioBuf.data(), edHop);
            if (framesRead == 0) break;
            ed->analyzer.pushSamples(ed->audioBuf.data(), framesRead);
        }
    }

    return spectraThisFrame;
}

void AudioEngine::drainSources() {
    if (audioSource_ && audioSource_->isRealTime()) {
        int channels = audioSource_->channels();
        std::vector<float> drain(4096 * channels);
        while (audioSource_->read(drain.data(), 4096) > 0) {}
    }
    for (auto& ed : extraDevices_) {
        if (ed->source && ed->source->isRealTime()) {
            int ch = ed->source->channels();
            std::vector<float> drain(4096 * ch);
            while (ed->source->read(drain.data(), 4096) > 0) {}
        }
    }
}

// ── Unified channel view ─────────────────────────────────────────────────────

int AudioEngine::totalNumSpectra() const {
    int n = analyzer_.numSpectra();
    for (auto& ed : extraDevices_)
        n += ed->analyzer.numSpectra();
    return n;
}

const std::vector<float>& AudioEngine::getSpectrum(int globalCh) const {
    int n = analyzer_.numSpectra();
    if (globalCh < n) return analyzer_.channelSpectrum(globalCh);
    globalCh -= n;
    for (auto& ed : extraDevices_) {
        int en = ed->analyzer.numSpectra();
        if (globalCh < en) return ed->analyzer.channelSpectrum(globalCh);
        globalCh -= en;
    }
    return analyzer_.channelSpectrum(0);
}

const std::deque<std::vector<float>>& AudioEngine::getWaterfallHistory(int globalCh) const {
    int n = analyzer_.numSpectra();
    if (globalCh < n) return analyzer_.waterfallHistory(globalCh);
    globalCh -= n;
    for (auto& ed : extraDevices_) {
        int en = ed->analyzer.numSpectra();
        if (globalCh < en) return ed->analyzer.waterfallHistory(globalCh);
        globalCh -= en;
    }
    return analyzer_.waterfallHistory(0);
}

const std::vector<std::complex<float>>& AudioEngine::getComplex(int globalCh) const {
    int n = analyzer_.numSpectra();
    if (globalCh < n) return analyzer_.channelComplex(globalCh);
    globalCh -= n;
    for (auto& ed : extraDevices_) {
        int en = ed->analyzer.numSpectra();
        if (globalCh < en) return ed->analyzer.channelComplex(globalCh);
        globalCh -= en;
    }
    return analyzer_.channelComplex(0);
}

const char* AudioEngine::getDeviceName(int globalCh) const {
    int n = analyzer_.numSpectra();
    if (globalCh < n) {
        if (deviceIdx_ >= 0 && deviceIdx_ < static_cast<int>(devices_.size()))
            return devices_[deviceIdx_].name.c_str();
        for (int i = 0; i < static_cast<int>(devices_.size()); ++i)
            if (deviceSelected_[i]) return devices_[i].name.c_str();
        return "Audio Device";
    }
    globalCh -= n;
    int devSel = 0;
    for (int i = 0; i < static_cast<int>(devices_.size()) && i < kMaxChannels; ++i) {
        if (!deviceSelected_[i]) continue;
        ++devSel;
        if (devSel <= 1) continue;
        int edIdx = devSel - 2;
        if (edIdx < static_cast<int>(extraDevices_.size())) {
            int en = extraDevices_[edIdx]->analyzer.numSpectra();
            if (globalCh < en) return devices_[i].name.c_str();
            globalCh -= en;
        }
    }
    return "Audio Device";
}

// ── Math channels ────────────────────────────────────────────────────────────

void AudioEngine::computeMathChannels() {
    int nPhys = totalNumSpectra();
    int specSz = analyzer_.spectrumSize();
    mathSpectra_.resize(mathChannels_.size());

    for (size_t mi = 0; mi < mathChannels_.size(); ++mi) {
        const auto& mc = mathChannels_[mi];
        auto& out = mathSpectra_[mi];
        out.resize(specSz);

        if (!mc.enabled) {
            std::fill(out.begin(), out.end(), kNoSignalDB);
            continue;
        }

        int sx = std::clamp(mc.sourceX, 0, nPhys - 1);
        int sy = std::clamp(mc.sourceY, 0, nPhys - 1);
        const auto& xDB = getSpectrum(sx);
        const auto& yDB = getSpectrum(sy);
        const auto& xC  = getComplex(sx);
        const auto& yC  = getComplex(sy);

        for (int i = 0; i < specSz; ++i) {
            float val = kNoSignalDB;
            switch (mc.op) {
                case MathOp::Negate:   val = -xDB[i]; break;
                case MathOp::Absolute: val = std::abs(xDB[i]); break;
                case MathOp::Square:   val = 2.0f * xDB[i]; break;
                case MathOp::Cube:     val = 3.0f * xDB[i]; break;
                case MathOp::Sqrt:     val = 0.5f * xDB[i]; break;
                case MathOp::Log: {
                    float lin = std::pow(10.0f, xDB[i] / 10.0f);
                    val = 10.0f * std::log10(lin + kLogGuard);
                    break;
                }
                case MathOp::Add: {
                    float lx = std::pow(10.0f, xDB[i] / 10.0f);
                    float ly = std::pow(10.0f, yDB[i] / 10.0f);
                    float s = lx + ly;
                    val = (s > kLinearEpsilon) ? 10.0f * std::log10(s) : kNoSignalDB;
                    break;
                }
                case MathOp::Subtract: {
                    float lx = std::pow(10.0f, xDB[i] / 10.0f);
                    float ly = std::pow(10.0f, yDB[i] / 10.0f);
                    float d = std::abs(lx - ly);
                    val = (d > kLinearEpsilon) ? 10.0f * std::log10(d) : kNoSignalDB;
                    break;
                }
                case MathOp::Multiply:
                    val = xDB[i] + yDB[i];
                    break;
                case MathOp::Phase: {
                    if (i < static_cast<int>(xC.size()) &&
                        i < static_cast<int>(yC.size())) {
                        auto cross = xC[i] * std::conj(yC[i]);
                        val = std::atan2(cross.imag(), cross.real())
                              * (180.0f / kPi);
                    }
                    break;
                }
                case MathOp::CrossCorr: {
                    if (i < static_cast<int>(xC.size()) &&
                        i < static_cast<int>(yC.size())) {
                        auto cross = xC[i] * std::conj(yC[i]);
                        float mag2 = std::norm(cross);
                        val = (mag2 > kLinearEpsilon) ? 10.0f * std::log10(mag2) : kNoSignalDB;
                    }
                    break;
                }
                default: break;
            }
            out[i] = val;
        }
    }
}

} // namespace baudmine
