#pragma once

#include "core/Types.h"
#include <fftw3.h>
#include <complex>
#include <vector>

namespace baudmine {

// Wraps FFTW for real->complex and complex->complex transforms.
// Produces magnitude output in dB and optionally retains the complex spectrum.
class FFTProcessor {
public:
    FFTProcessor();
    ~FFTProcessor();

    FFTProcessor(const FFTProcessor&) = delete;
    FFTProcessor& operator=(const FFTProcessor&) = delete;

    void configure(int fftSize, bool complexInput);

    int  fftSize()      const { return fftSize_; }
    bool isComplex()    const { return complexInput_; }
    int  outputBins()   const { return complexInput_ ? fftSize_ : fftSize_ / 2 + 1; }
    int  spectrumSize() const { return complexInput_ ? fftSize_ : fftSize_ / 2 + 1; }

    // Process and produce both dB magnitude and complex spectrum.
    void processReal(const float* input,
                     std::vector<float>& outputDB,
                     std::vector<std::complex<float>>& outputCplx);

    void processComplex(const float* inputIQ,
                        std::vector<float>& outputDB,
                        std::vector<std::complex<float>>& outputCplx);

    // Convenience: dB-only (no complex output).
    void processReal(const float* input, std::vector<float>& outputDB);
    void processComplex(const float* inputIQ, std::vector<float>& outputDB);

private:
    int  fftSize_      = 0;
    bool complexInput_ = false;

    float*         realIn_   = nullptr;
    fftwf_complex* realOut_  = nullptr;
    fftwf_plan     realPlan_ = nullptr;

    fftwf_complex* cplxIn_   = nullptr;
    fftwf_complex* cplxOut_  = nullptr;
    fftwf_plan     cplxPlan_ = nullptr;

    void destroyPlans();

    // Scratch buffer for convenience overloads (avoids per-call allocation).
    std::vector<std::complex<float>> scratchCplx_;
};

} // namespace baudmine
