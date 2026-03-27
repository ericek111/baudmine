#define MINIAUDIO_IMPLEMENTATION
#include "audio/miniaudio.h"
#include "audio/MiniAudioSource.h"
#include <cstdio>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// Enumerate audio input devices via the Web MediaDevices API.
// Stores results in a JS-side array; returns the number of devices found.
EM_ASYNC_JS(int, baudmine_js_enumerate_devices, (), {
    if (!navigator.mediaDevices || !navigator.mediaDevices.enumerateDevices) {
        return 0;
    }
    // Request mic permission first so labels are populated.
    try {
        var stream = await navigator.mediaDevices.getUserMedia({audio: true});
        stream.getTracks().forEach(function(t) { t.stop(); });
    } catch (e) {
        console.warn("baudmine: mic permission denied, device labels may be empty");
    }
    try {
        var devices = await navigator.mediaDevices.enumerateDevices();
        window.baudmine_audioInputs = [];
        for (var i = 0; i < devices.length; ++i) {
            if (devices[i].kind === "audioinput") {
                window.baudmine_audioInputs.push({
                    deviceId: devices[i].deviceId,
                    label: devices[i].label || ("Microphone " + (window.baudmine_audioInputs.length + 1))
                });
            }
        }
        return window.baudmine_audioInputs.length;
    } catch (e) {
        console.error("baudmine: enumerateDevices failed:", e);
        return 0;
    }
});

// Get the label of the device at the given index.
EM_JS(void, baudmine_js_get_device_name, (int index, char* buf, int bufSize), {
    var inputs = window.baudmine_audioInputs || [];
    var name = (index >= 0 && index < inputs.length) ? inputs[index].label : "";
    stringToUTF8(name, buf, bufSize);
});

// Set the device ID that the next getUserMedia call should use.
EM_JS(void, baudmine_js_set_selected_device, (int index), {
    var inputs = window.baudmine_audioInputs || [];
    if (index >= 0 && index < inputs.length) {
        window.baudmine_selectedDeviceId = inputs[index].deviceId;
    } else {
        window.baudmine_selectedDeviceId = null;
    }
});

#endif // __EMSCRIPTEN__

namespace baudmine {

// ── Shared context (lazy-initialized) ────────────────────────────────────────

static ma_context* sharedContext() {
    static ma_context ctx;
    static bool init = false;
    if (!init) {
        if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) {
            std::fprintf(stderr, "miniaudio: failed to init context\n");
            return nullptr;
        }
        init = true;
    }
    return &ctx;
}

// ── Device enumeration ───────────────────────────────────────────────────────

std::vector<MiniAudioSource::DeviceInfo> MiniAudioSource::listInputDevices() {
    std::vector<DeviceInfo> result;

#ifdef __EMSCRIPTEN__
    // Use the Web MediaDevices API to enumerate microphones.
    int count = baudmine_js_enumerate_devices();
    for (int i = 0; i < count; ++i) {
        char nameBuf[256] = {};
        baudmine_js_get_device_name(i, nameBuf, sizeof(nameBuf));
        result.push_back({
            i,
            nameBuf,
            2,       // Web Audio typically provides stereo
            48000.0  // Web Audio default sample rate
        });
    }
#else
    ma_context* ctx = sharedContext();
    if (!ctx) return result;

    ma_device_info* captureDevices;
    ma_uint32 captureCount;
    if (ma_context_get_devices(ctx, nullptr, nullptr,
                               &captureDevices, &captureCount) != MA_SUCCESS) {
        return result;
    }

    for (ma_uint32 i = 0; i < captureCount; ++i) {
        // Query full device info for channel count and sample rate.
        ma_device_info fullInfo;
        if (ma_context_get_device_info(ctx, ma_device_type_capture,
                                        &captureDevices[i].id, &fullInfo) != MA_SUCCESS) {
            fullInfo = captureDevices[i];
        }

        int maxCh = 0;
        if (fullInfo.nativeDataFormatCount > 0) {
            for (ma_uint32 f = 0; f < fullInfo.nativeDataFormatCount; ++f) {
                int ch = static_cast<int>(fullInfo.nativeDataFormats[f].channels);
                if (ch > maxCh) maxCh = ch;
            }
        }
        // channels == 0 means "any" in miniaudio (e.g. Web Audio backend).
        // Default to 2 (stereo) so the UI can offer multi-channel mode.
        if (maxCh <= 0) maxCh = 2;

        double defaultSR = 48000.0;
        if (fullInfo.nativeDataFormatCount > 0 && fullInfo.nativeDataFormats[0].sampleRate > 0)
            defaultSR = static_cast<double>(fullInfo.nativeDataFormats[0].sampleRate);

        result.push_back({
            static_cast<int>(i),
            fullInfo.name,
            maxCh,
            defaultSR
        });
    }
#endif

    return result;
}

// ── Construction / destruction ───────────────────────────────────────────────

MiniAudioSource::MiniAudioSource(double sampleRate, int channels, int deviceIndex)
    : sampleRate_(sampleRate)
    , channels_(channels)
    , deviceIndex_(deviceIndex)
{
    size_t ringSize = static_cast<size_t>(sampleRate * channels * 2); // ~2 seconds
    ringBuf_ = std::make_unique<RingBuffer<float>>(ringSize);
}

MiniAudioSource::~MiniAudioSource() {
    close();
}

// ── Callback ─────────────────────────────────────────────────────────────────

void MiniAudioSource::dataCallback(ma_device* device, void* /*output*/,
                                    const void* input, unsigned int frameCount) {
    auto* self = static_cast<MiniAudioSource*>(device->pUserData);
    if (input) {
        const auto* in = static_cast<const float*>(input);
        self->ringBuf_->write(in, frameCount * self->channels_);
    }
}

// ── Open / close / read ──────────────────────────────────────────────────────

bool MiniAudioSource::open() {
    if (opened_) return true;

    ma_context* ctx = sharedContext();
    if (!ctx) return false;

    // Resolve device ID if a specific index was requested.
    ma_device_id* pDeviceID = nullptr;
    ma_device_id  deviceID{};

#ifdef __EMSCRIPTEN__
    // Tell the JS layer which microphone to use in the next getUserMedia call.
    baudmine_js_set_selected_device(deviceIndex_);
#else
    if (deviceIndex_ >= 0) {
        ma_device_info* captureDevices;
        ma_uint32 captureCount;
        if (ma_context_get_devices(ctx, nullptr, nullptr,
                                    &captureDevices, &captureCount) == MA_SUCCESS) {
            if (deviceIndex_ < static_cast<int>(captureCount)) {
                deviceID = captureDevices[deviceIndex_].id;
                pDeviceID = &deviceID;
            }
        }
    }
#endif

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.pDeviceID = pDeviceID;
    config.capture.format    = ma_format_f32;
    config.capture.channels  = static_cast<ma_uint32>(channels_);
    config.sampleRate        = static_cast<ma_uint32>(sampleRate_);
    config.dataCallback      = dataCallback;
    config.pUserData         = this;
    config.periodSizeInFrames = 512;

    device_ = std::make_unique<ma_device>();
    if (ma_device_init(ctx, &config, device_.get()) != MA_SUCCESS) {
        std::fprintf(stderr, "miniaudio: failed to init device\n");
        device_.reset();
        return false;
    }

    // Read back actual device parameters (the backend may give us fewer
    // channels than requested, e.g. Web Audio defaults to mono).
    channels_   = static_cast<int>(device_->capture.channels);
    sampleRate_ = static_cast<double>(device_->sampleRate);

    // Rebuild ring buffer to match actual channel count.
    size_t ringSize = static_cast<size_t>(sampleRate_ * channels_ * 2);
    ringBuf_ = std::make_unique<RingBuffer<float>>(ringSize);

    if (ma_device_start(device_.get()) != MA_SUCCESS) {
        std::fprintf(stderr, "miniaudio: failed to start device\n");
        ma_device_uninit(device_.get());
        device_.reset();
        return false;
    }

    opened_ = true;
    return true;
}

void MiniAudioSource::close() {
    if (device_) {
        ma_device_uninit(device_.get());
        device_.reset();
    }
    opened_ = false;
}

size_t MiniAudioSource::read(float* buffer, size_t frames) {
    return ringBuf_->read(buffer, frames * channels_) / channels_;
}

} // namespace baudmine
