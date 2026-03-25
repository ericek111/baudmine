#pragma once

#include "core/Types.h"
#include <fftw3.h>
#include <vector>
#include <complex>

namespace baudline {

// Wraps FFTW for real→complex and complex→complex transforms.
// Produces magnitude output in dB.
class FFTProcessor {
public:
    FFTProcessor();
    ~FFTProcessor();

    FFTProcessor(const FFTProcessor&) = delete;
    FFTProcessor& operator=(const FFTProcessor&) = delete;

    // Reconfigure for a new FFT size and mode.  Rebuilds FFTW plans.
    void configure(int fftSize, bool complexInput);

    int  fftSize()      const { return fftSize_; }
    bool isComplex()    const { return complexInput_; }
    int  outputBins()   const { return complexInput_ ? fftSize_ : fftSize_ / 2 + 1; }
    int  spectrumSize() const { return complexInput_ ? fftSize_ : fftSize_ / 2 + 1; }

    // Process windowed real samples → magnitude dB spectrum.
    // `input` must have fftSize_ elements.
    // `outputDB` will be resized to spectrumSize().
    void processReal(const float* input, std::vector<float>& outputDB);

    // Process windowed I/Q samples → magnitude dB spectrum.
    // `inputIQ` is interleaved [I0,Q0,I1,Q1,...], fftSize_*2 floats.
    // `outputDB` will be resized to spectrumSize().
    // Output is FFT-shifted so DC is in the center.
    void processComplex(const float* inputIQ, std::vector<float>& outputDB);

private:
    int  fftSize_      = 0;
    bool complexInput_ = false;

    // Real FFT
    float*         realIn_   = nullptr;
    fftwf_complex* realOut_  = nullptr;
    fftwf_plan     realPlan_ = nullptr;

    // Complex FFT
    fftwf_complex* cplxIn_   = nullptr;
    fftwf_complex* cplxOut_  = nullptr;
    fftwf_plan     cplxPlan_ = nullptr;

    void destroyPlans();
};

} // namespace baudline
