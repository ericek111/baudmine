#include "audio/PortAudioSource.h"
#include <cstdio>
#include <cstring>

namespace baudmine {

static bool sPaInitialized = false;

static void ensurePaInit() {
    if (!sPaInitialized) {
        Pa_Initialize();
        sPaInitialized = true;
    }
}

PortAudioSource::PortAudioSource(double sampleRate, int channels,
                                 int deviceIndex, int framesPerBuffer)
    : sampleRate_(sampleRate)
    , channels_(channels)
    , deviceIndex_(deviceIndex)
    , framesPerBuffer_(framesPerBuffer)
{
    ensurePaInit();
    size_t ringSize = static_cast<size_t>(sampleRate * channels * 2); // ~2 seconds
    ringBuf_ = std::make_unique<RingBuffer<float>>(ringSize);
}

PortAudioSource::~PortAudioSource() {
    close();
}

bool PortAudioSource::open() {
    if (opened_) return true;

    PaStreamParameters params{};
    if (deviceIndex_ < 0) {
        params.device = Pa_GetDefaultInputDevice();
    } else {
        params.device = deviceIndex_;
    }
    if (params.device == paNoDevice) {
        std::fprintf(stderr, "PortAudio: no input device available\n");
        return false;
    }

    const PaDeviceInfo* info = Pa_GetDeviceInfo(params.device);
    if (!info) return false;

    params.channelCount = channels_;
    params.sampleFormat = paFloat32;
    params.suggestedLatency = info->defaultLowInputLatency;
    params.hostApiSpecificStreamInfo = nullptr;

    PaError err = Pa_OpenStream(&stream_, &params, nullptr,
                                sampleRate_, framesPerBuffer_,
                                paClipOff, paCallback, this);
    if (err != paNoError) {
        std::fprintf(stderr, "PortAudio open error: %s\n", Pa_GetErrorText(err));
        return false;
    }

    err = Pa_StartStream(stream_);
    if (err != paNoError) {
        std::fprintf(stderr, "PortAudio start error: %s\n", Pa_GetErrorText(err));
        Pa_CloseStream(stream_);
        stream_ = nullptr;
        return false;
    }

    opened_ = true;
    return true;
}

void PortAudioSource::close() {
    if (stream_) {
        Pa_StopStream(stream_);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
    }
    opened_ = false;
}

size_t PortAudioSource::read(float* buffer, size_t frames) {
    return ringBuf_->read(buffer, frames * channels_) / channels_;
}

int PortAudioSource::paCallback(const void* input, void* /*output*/,
                                unsigned long frameCount,
                                const PaStreamCallbackTimeInfo* /*timeInfo*/,
                                PaStreamCallbackFlags /*statusFlags*/,
                                void* userData) {
    auto* self = static_cast<PortAudioSource*>(userData);
    if (input) {
        const auto* in = static_cast<const float*>(input);
        self->ringBuf_->write(in, frameCount * self->channels_);
    }
    return paContinue;
}

std::vector<PortAudioSource::DeviceInfo> PortAudioSource::listInputDevices() {
    ensurePaInit();
    std::vector<DeviceInfo> devices;
    int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info && info->maxInputChannels > 0) {
            devices.push_back({i, info->name, info->maxInputChannels,
                               info->defaultSampleRate});
        }
    }
    return devices;
}

} // namespace baudmine
