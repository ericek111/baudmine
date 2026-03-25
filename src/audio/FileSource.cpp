#include "audio/FileSource.h"
#include <algorithm>
#include <cstring>

namespace baudline {

FileSource::FileSource(const std::string& path, InputFormat format,
                       double sampleRate, bool loop)
    : path_(path), format_(format), sampleRate_(sampleRate), loop_(loop)
{
    if (format_ == InputFormat::WAV) {
        channels_ = 0; // determined on open
    } else {
        channels_ = 2; // I/Q is always 2 channels
    }
}

FileSource::~FileSource() {
    close();
}

bool FileSource::open() {
    close();
    eof_ = false;

    if (format_ == InputFormat::WAV) {
#ifndef __EMSCRIPTEN__
        std::memset(&sfInfo_, 0, sizeof(sfInfo_));
        sndFile_ = sf_open(path_.c_str(), SFM_READ, &sfInfo_);
        if (!sndFile_) return false;
        sampleRate_ = sfInfo_.samplerate;
        channels_   = sfInfo_.channels;
#else
        if (!wavReader_.open(path_)) return false;
        sampleRate_ = wavReader_.sampleRate();
        channels_   = wavReader_.channels();
#endif
        return true;
    }

    // Raw I/Q file
    rawFile_.open(path_, std::ios::binary);
    if (!rawFile_.is_open()) return false;

    rawFile_.seekg(0, std::ios::end);
    rawFileSize_ = rawFile_.tellg();
    rawFile_.seekg(0, std::ios::beg);
    channels_ = 2;
    return true;
}

void FileSource::close() {
#ifndef __EMSCRIPTEN__
    if (sndFile_) {
        sf_close(sndFile_);
        sndFile_ = nullptr;
    }
#else
    wavReader_.close();
#endif
    if (rawFile_.is_open()) {
        rawFile_.close();
    }
}

size_t FileSource::read(float* buffer, size_t frames) {
    if (eof_) return 0;
    size_t got = 0;

    switch (format_) {
        case InputFormat::WAV:        got = readWAV(buffer, frames);        break;
        case InputFormat::Float32IQ:  got = readRawFloat32(buffer, frames); break;
        case InputFormat::Int16IQ:    got = readRawInt16(buffer, frames);   break;
        case InputFormat::Uint8IQ:    got = readRawUint8(buffer, frames);   break;
        default: break;
    }

    if (got < frames) {
        if (loop_) {
            seek(0.0);
            eof_ = false;
            // Fill remainder
            size_t extra = read(buffer + got * channels_, frames - got);
            got += extra;
        } else {
            eof_ = true;
        }
    }
    return got;
}

void FileSource::seek(double seconds) {
    eof_ = false;
    if (format_ == InputFormat::WAV) {
#ifndef __EMSCRIPTEN__
        if (sndFile_)
            sf_seek(sndFile_, static_cast<sf_count_t>(seconds * sampleRate_), SEEK_SET);
#else
        wavReader_.seekFrame(static_cast<size_t>(seconds * sampleRate_));
#endif
    } else if (rawFile_.is_open()) {
        size_t bytesPerFrame = 0;
        switch (format_) {
            case InputFormat::Float32IQ: bytesPerFrame = 2 * sizeof(float);    break;
            case InputFormat::Int16IQ:   bytesPerFrame = 2 * sizeof(int16_t);  break;
            case InputFormat::Uint8IQ:   bytesPerFrame = 2 * sizeof(uint8_t);  break;
            default: break;
        }
        auto pos = static_cast<std::streamoff>(seconds * sampleRate_ * bytesPerFrame);
        rawFile_.clear();
        rawFile_.seekg(pos);
    }
}

double FileSource::duration() const {
    if (format_ == InputFormat::WAV) {
#ifndef __EMSCRIPTEN__
        if (sfInfo_.samplerate > 0)
            return static_cast<double>(sfInfo_.frames) / sfInfo_.samplerate;
#else
        if (wavReader_.sampleRate() > 0 && wavReader_.totalFrames() > 0)
            return static_cast<double>(wavReader_.totalFrames()) / wavReader_.sampleRate();
#endif
    }
    if (rawFileSize_ > 0) {
        size_t bytesPerFrame = 0;
        switch (format_) {
            case InputFormat::Float32IQ: bytesPerFrame = 2 * sizeof(float);    break;
            case InputFormat::Int16IQ:   bytesPerFrame = 2 * sizeof(int16_t);  break;
            case InputFormat::Uint8IQ:   bytesPerFrame = 2 * sizeof(uint8_t);  break;
            default: return -1.0;
        }
        size_t totalFrames = rawFileSize_ / bytesPerFrame;
        return static_cast<double>(totalFrames) / sampleRate_;
    }
    return -1.0;
}

// ── Format-specific readers ──────────────────────────────────────────────────

size_t FileSource::readWAV(float* buffer, size_t frames) {
#ifndef __EMSCRIPTEN__
    if (!sndFile_) return 0;
    sf_count_t got = sf_readf_float(sndFile_, buffer, frames);
    return (got > 0) ? static_cast<size_t>(got) : 0;
#else
    return wavReader_.readFloat(buffer, frames);
#endif
}

size_t FileSource::readRawFloat32(float* buffer, size_t frames) {
    size_t samples = frames * 2;
    rawFile_.read(reinterpret_cast<char*>(buffer), samples * sizeof(float));
    size_t bytesRead = rawFile_.gcount();
    return bytesRead / (2 * sizeof(float));
}

size_t FileSource::readRawInt16(float* buffer, size_t frames) {
    size_t samples = frames * 2;
    std::vector<int16_t> tmp(samples);
    rawFile_.read(reinterpret_cast<char*>(tmp.data()), samples * sizeof(int16_t));
    size_t bytesRead = rawFile_.gcount();
    size_t samplesRead = bytesRead / sizeof(int16_t);
    for (size_t i = 0; i < samplesRead; ++i)
        buffer[i] = tmp[i] / 32768.0f;
    return samplesRead / 2;
}

size_t FileSource::readRawUint8(float* buffer, size_t frames) {
    size_t samples = frames * 2;
    std::vector<uint8_t> tmp(samples);
    rawFile_.read(reinterpret_cast<char*>(tmp.data()), samples * sizeof(uint8_t));
    size_t bytesRead = rawFile_.gcount();
    size_t samplesRead = bytesRead / sizeof(uint8_t);
    // RTL-SDR style: center at 127.5, scale to [-1, 1]
    for (size_t i = 0; i < samplesRead; ++i)
        buffer[i] = (tmp[i] - 127.5f) / 127.5f;
    return samplesRead / 2;
}

} // namespace baudline
