#pragma once

#include "core/Types.h"
#include "audio/AudioSource.h"
#include "audio/MiniAudioSource.h"
#include "dsp/SpectrumAnalyzer.h"

#include <complex>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace baudmine {

class AudioEngine {
public:
    AudioEngine();

    // ── Device enumeration ──
    void enumerateDevices();
    const std::vector<MiniAudioSource::DeviceInfo>& devices() const { return devices_; }

    // ── Source management ──
    void openDevice(int deviceListIdx);
    void openMultiDevice(const bool selected[], int maxDevs);
    void openFile(const std::string& path, InputFormat format, double sampleRate, bool loop);
    void closeAll();

    bool hasSource() const { return audioSource_ != nullptr; }
    AudioSource* source() { return audioSource_.get(); }

    // ── Analyzer ──
    AnalyzerSettings&       settings()       { return settings_; }
    const AnalyzerSettings& settings() const { return settings_; }
    const SpectrumAnalyzer& primaryAnalyzer() const { return analyzer_; }

    void configure(const AnalyzerSettings& s);
    int  processAudio();   // returns number of new spectra from primary analyzer
    void clearHistory();
    void drainSources();   // flush stale audio from all real-time sources

    // ── Unified channel view across all analyzers ──
    int totalNumSpectra() const;
    const std::vector<float>& getSpectrum(int globalCh) const;
    const std::deque<std::vector<float>>& getWaterfallHistory(int globalCh) const;
    const std::vector<std::complex<float>>& getComplex(int globalCh) const;
    const char* getDeviceName(int globalCh) const;
    int spectrumSize() const { return analyzer_.spectrumSize(); }
    double binToFreq(int bin) const { return analyzer_.binToFreq(bin); }

    // ── Math channels ──
    std::vector<MathChannel>&       mathChannels()       { return mathChannels_; }
    const std::vector<MathChannel>& mathChannels() const { return mathChannels_; }
    const std::vector<std::vector<float>>& mathSpectra() const { return mathSpectra_; }
    const std::deque<std::vector<float>>& mathWaterfallHistory(int mi) const;
    void computeMathChannels();

    // ── Overrun tracking ──
    // Counts spectra lost because more were produced in a single frame
    // than the waterfall history deque can hold.
    int  overrunCount() const { return overrunCount_; }
    void resetOverrunCount()  { overrunCount_ = 0; }

    // ── Device selection state (for config persistence) ──
    int  deviceIdx()       const { return deviceIdx_; }
    void setDeviceIdx(int i)     { deviceIdx_ = i; }
    bool multiDeviceMode() const { return multiDeviceMode_; }
    void setMultiDeviceMode(bool m) { multiDeviceMode_ = m; }
    bool deviceSelected(int i) const { return (i >= 0 && i < kMaxChannels) ? deviceSelected_[i] : false; }
    void setDeviceSelected(int i, bool v) { if (i >= 0 && i < kMaxChannels) deviceSelected_[i] = v; }
    void clearDeviceSelections();

private:
    struct ExtraDevice {
        std::unique_ptr<AudioSource> source;
        SpectrumAnalyzer             analyzer;
        std::vector<float>           audioBuf;
    };

    // Sources
    std::unique_ptr<AudioSource>              audioSource_;
    std::vector<float>                        audioBuf_;
    std::vector<std::unique_ptr<ExtraDevice>> extraDevices_;

    // DSP
    SpectrumAnalyzer analyzer_;
    AnalyzerSettings settings_;

    // Math
    std::vector<MathChannel>                    mathChannels_;
    std::vector<std::vector<float>>             mathSpectra_;
    std::vector<std::deque<std::vector<float>>> mathWaterfalls_;

    // Overrun
    int overrunCount_ = 0;

    // Device state
    std::vector<MiniAudioSource::DeviceInfo> devices_;
    int  deviceIdx_  = 0;
    bool multiDeviceMode_ = false;
    bool deviceSelected_[kMaxChannels] = {};
};

} // namespace baudmine
