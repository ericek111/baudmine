#pragma once
// Minimal WAV file reader — no external dependencies.
// Used for WASM builds where libsndfile is unavailable.

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace baudline {

class WavReader {
public:
    bool open(const std::string& path);
    void close();

    // Read interleaved float frames.  Returns number of frames read.
    size_t readFloat(float* buffer, size_t frames);

    // Seek to frame position.
    void seekFrame(size_t frame);

    int    sampleRate() const { return sampleRate_; }
    int    channels()   const { return channels_; }
    size_t totalFrames() const { return totalFrames_; }

private:
    std::ifstream file_;
    int      sampleRate_  = 0;
    int      channels_    = 0;
    int      bitsPerSample_ = 0;
    int      bytesPerSample_ = 0;
    size_t   totalFrames_ = 0;
    size_t   dataOffset_  = 0;    // byte offset of PCM data in file
    size_t   dataSize_    = 0;    // total bytes of PCM data

    std::vector<uint8_t> readBuf_;  // scratch for format conversion
};

} // namespace baudline
