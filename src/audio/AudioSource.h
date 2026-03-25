#pragma once

#include <cstddef>

namespace baudmine {

// Abstract audio source.  All sources deliver interleaved float samples.
// For I/Q data channels()==2: [I0, Q0, I1, Q1, ...].
// For mono real data channels()==1.
class AudioSource {
public:
    virtual ~AudioSource() = default;

    virtual bool   open()  = 0;
    virtual void   close() = 0;

    // Read up to `frames` frames into `buffer` (buffer size >= frames * channels()).
    // Returns number of frames actually read.
    virtual size_t read(float* buffer, size_t frames) = 0;

    virtual double sampleRate() const = 0;
    virtual int    channels()   const = 0;  // 1 = real, 2 = I/Q
    virtual bool   isRealTime() const = 0;
    virtual bool   isEOF()      const = 0;
};

} // namespace baudmine
