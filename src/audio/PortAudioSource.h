#pragma once

#include "audio/AudioSource.h"
#include "core/RingBuffer.h"
#include <portaudio.h>
#include <memory>
#include <string>

namespace baudmine {

class PortAudioSource : public AudioSource {
public:
    // deviceIndex = -1 for default input device
    PortAudioSource(double sampleRate = 48000.0, int channels = 1,
                    int deviceIndex = -1, int framesPerBuffer = 512);
    ~PortAudioSource() override;

    bool   open()  override;
    void   close() override;
    size_t read(float* buffer, size_t frames) override;

    double sampleRate() const override { return sampleRate_; }
    int    channels()   const override { return channels_; }
    bool   isRealTime() const override { return true; }
    bool   isEOF()      const override { return false; }

    // List available input devices (for UI enumeration).
    struct DeviceInfo {
        int         index;
        std::string name;
        int         maxInputChannels;
        double      defaultSampleRate;
    };
    static std::vector<DeviceInfo> listInputDevices();

private:
    static int paCallback(const void* input, void* output,
                          unsigned long frameCount,
                          const PaStreamCallbackTimeInfo* timeInfo,
                          PaStreamCallbackFlags statusFlags,
                          void* userData);

    double      sampleRate_;
    int         channels_;
    int         deviceIndex_;
    int         framesPerBuffer_;
    PaStream*   stream_ = nullptr;
    bool        opened_ = false;

    // Ring buffer large enough for ~1 second of audio
    std::unique_ptr<RingBuffer<float>> ringBuf_;
};

} // namespace baudmine
