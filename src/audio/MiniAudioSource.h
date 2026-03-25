#pragma once

#include "audio/AudioSource.h"
#include "core/RingBuffer.h"
#include <memory>
#include <string>
#include <vector>

// Forward-declare miniaudio types to avoid pulling the huge header into every TU.
struct ma_device;
struct ma_context;

namespace baudline {

class MiniAudioSource : public AudioSource {
public:
    // deviceIndex = -1 for default input device
    MiniAudioSource(double sampleRate = 48000.0, int channels = 1,
                    int deviceIndex = -1);
    ~MiniAudioSource() override;

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
    static void dataCallback(ma_device* device, void* output,
                             const void* input, unsigned int frameCount);

    double      sampleRate_;
    int         channels_;
    int         deviceIndex_;
    bool        opened_ = false;

    std::unique_ptr<ma_device>  device_;
    std::unique_ptr<RingBuffer<float>> ringBuf_;
};

} // namespace baudline
