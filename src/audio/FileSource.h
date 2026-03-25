#pragma once

#include "audio/AudioSource.h"
#include "core/Types.h"
#ifndef __EMSCRIPTEN__
#include <sndfile.h>
#else
#include "audio/WavReader.h"
#endif
#include <fstream>
#include <string>
#include <vector>

namespace baudline {

// Reads WAV files and raw I/Q files (float32, int16, uint8).
// Native builds use libsndfile; WASM builds use a built-in WAV reader.
class FileSource : public AudioSource {
public:
    // For WAV files: format is auto-detected, sampleRate/channels from file header.
    // For raw I/Q: user must specify format and sampleRate.
    FileSource(const std::string& path, InputFormat format = InputFormat::WAV,
               double sampleRate = 48000.0, bool loop = false);
    ~FileSource() override;

    bool   open()  override;
    void   close() override;
    size_t read(float* buffer, size_t frames) override;

    double sampleRate() const override { return sampleRate_; }
    int    channels()   const override { return channels_; }
    bool   isRealTime() const override { return false; }
    bool   isEOF()      const override { return eof_; }

    // Seek to a position (seconds).
    void seek(double seconds);

    // File duration in seconds (-1 if unknown, e.g. raw files without known size).
    double duration() const;

private:
    size_t readWAV(float* buffer, size_t frames);
    size_t readRawFloat32(float* buffer, size_t frames);
    size_t readRawInt16(float* buffer, size_t frames);
    size_t readRawUint8(float* buffer, size_t frames);

    std::string   path_;
    InputFormat   format_;
    double        sampleRate_;
    int           channels_ = 2; // I/Q default
    bool          loop_;
    bool          eof_ = false;

#ifndef __EMSCRIPTEN__
    // WAV via libsndfile (native)
    SNDFILE*      sndFile_  = nullptr;
    SF_INFO       sfInfo_{};
#else
    // WAV via built-in reader (WASM)
    WavReader     wavReader_;
#endif

    // Raw I/Q files
    std::ifstream rawFile_;
    size_t        rawFileSize_ = 0;
};

} // namespace baudline
