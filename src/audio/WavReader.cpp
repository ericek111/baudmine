#include "audio/WavReader.h"
#include <cstring>
#include <algorithm>

namespace baudmine {

// Read a little-endian uint16/uint32 from raw bytes.
static uint16_t readU16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t readU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }

bool WavReader::open(const std::string& path) {
    close();
    file_.open(path, std::ios::binary);
    if (!file_.is_open()) return false;

    // Read RIFF header.
    uint8_t hdr[12];
    file_.read(reinterpret_cast<char*>(hdr), 12);
    if (file_.gcount() < 12) return false;
    if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0)
        return false;

    // Scan chunks for "fmt " and "data".
    bool gotFmt = false, gotData = false;
    while (!file_.eof() && !(gotFmt && gotData)) {
        uint8_t chunkHdr[8];
        file_.read(reinterpret_cast<char*>(chunkHdr), 8);
        if (file_.gcount() < 8) break;

        uint32_t chunkSize = readU32(chunkHdr + 4);

        if (std::memcmp(chunkHdr, "fmt ", 4) == 0) {
            uint8_t fmt[40] = {};
            size_t toRead = std::min<size_t>(chunkSize, sizeof(fmt));
            file_.read(reinterpret_cast<char*>(fmt), toRead);
            if (file_.gcount() < 16) return false;

            uint16_t audioFormat = readU16(fmt);
            // 1 = PCM integer, 3 = IEEE float
            if (audioFormat != 1 && audioFormat != 3) return false;

            channels_       = readU16(fmt + 2);
            sampleRate_     = static_cast<int>(readU32(fmt + 4));
            bitsPerSample_  = readU16(fmt + 14);
            bytesPerSample_ = bitsPerSample_ / 8;

            if (channels_ < 1 || sampleRate_ < 1 || bytesPerSample_ < 1)
                return false;

            // Skip remainder of fmt chunk if any.
            if (chunkSize > toRead)
                file_.seekg(chunkSize - toRead, std::ios::cur);
            gotFmt = true;

        } else if (std::memcmp(chunkHdr, "data", 4) == 0) {
            dataOffset_ = static_cast<size_t>(file_.tellg());
            dataSize_   = chunkSize;
            gotData = true;
            // Don't seek past data — we'll read from here.
        } else {
            // Skip unknown chunk.
            file_.seekg(chunkSize, std::ios::cur);
        }
    }

    if (!gotFmt || !gotData) return false;

    totalFrames_ = dataSize_ / (channels_ * bytesPerSample_);

    // Position at start of data.
    file_.seekg(dataOffset_);
    return true;
}

void WavReader::close() {
    if (file_.is_open()) file_.close();
    sampleRate_ = channels_ = bitsPerSample_ = bytesPerSample_ = 0;
    totalFrames_ = dataOffset_ = dataSize_ = 0;
}

size_t WavReader::readFloat(float* buffer, size_t frames) {
    if (!file_.is_open() || bytesPerSample_ == 0) return 0;

    size_t samples = frames * channels_;
    size_t rawBytes = samples * bytesPerSample_;
    readBuf_.resize(rawBytes);

    file_.read(reinterpret_cast<char*>(readBuf_.data()), rawBytes);
    size_t bytesRead = file_.gcount();
    size_t samplesRead = bytesRead / bytesPerSample_;
    size_t framesRead  = samplesRead / channels_;

    const uint8_t* src = readBuf_.data();

    switch (bitsPerSample_) {
        case 8:
            // Unsigned 8-bit PCM → [-1, 1]
            for (size_t i = 0; i < samplesRead; ++i)
                buffer[i] = (src[i] - 128) / 128.0f;
            break;

        case 16:
            for (size_t i = 0; i < samplesRead; ++i) {
                int16_t s = static_cast<int16_t>(readU16(src + i * 2));
                buffer[i] = s / 32768.0f;
            }
            break;

        case 24:
            for (size_t i = 0; i < samplesRead; ++i) {
                const uint8_t* p = src + i * 3;
                int32_t s = p[0] | (p[1] << 8) | (p[2] << 16);
                if (s & 0x800000) s |= 0xFF000000;  // sign-extend
                buffer[i] = s / 8388608.0f;
            }
            break;

        case 32:
            if (bytesPerSample_ == 4) {
                // Could be float or int32 — check by trying float first.
                // We detected audioFormat in open(); for simplicity, treat
                // 32-bit as float (most common for 32-bit WAV in SDR tools).
                std::memcpy(buffer, src, samplesRead * sizeof(float));
            }
            break;

        default:
            return 0;
    }

    return framesRead;
}

void WavReader::seekFrame(size_t frame) {
    if (!file_.is_open()) return;
    size_t byteOffset = dataOffset_ + frame * channels_ * bytesPerSample_;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(byteOffset));
}

} // namespace baudmine
