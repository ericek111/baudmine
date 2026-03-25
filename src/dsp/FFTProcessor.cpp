#include "dsp/FFTProcessor.h"
#include <cmath>
#include <algorithm>

namespace baudline {

FFTProcessor::FFTProcessor() = default;

FFTProcessor::~FFTProcessor() {
    destroyPlans();
}

void FFTProcessor::destroyPlans() {
    if (realPlan_) { fftwf_destroy_plan(realPlan_); realPlan_ = nullptr; }
    if (realIn_)   { fftwf_free(realIn_);           realIn_   = nullptr; }
    if (realOut_)  { fftwf_free(realOut_);           realOut_  = nullptr; }
    if (cplxPlan_) { fftwf_destroy_plan(cplxPlan_); cplxPlan_ = nullptr; }
    if (cplxIn_)   { fftwf_free(cplxIn_);           cplxIn_   = nullptr; }
    if (cplxOut_)  { fftwf_free(cplxOut_);           cplxOut_  = nullptr; }
}

void FFTProcessor::configure(int fftSize, bool complexInput) {
    if (fftSize == fftSize_ && complexInput == complexInput_) return;

    destroyPlans();
    fftSize_      = fftSize;
    complexInput_ = complexInput;

    if (complexInput_) {
        cplxIn_   = fftwf_alloc_complex(fftSize_);
        cplxOut_  = fftwf_alloc_complex(fftSize_);
        cplxPlan_ = fftwf_plan_dft_1d(fftSize_, cplxIn_, cplxOut_,
                                       FFTW_FORWARD, FFTW_ESTIMATE);
    } else {
        realIn_   = fftwf_alloc_real(fftSize_);
        realOut_  = fftwf_alloc_complex(fftSize_ / 2 + 1);
        realPlan_ = fftwf_plan_dft_r2c_1d(fftSize_, realIn_, realOut_, FFTW_ESTIMATE);
    }
}

void FFTProcessor::processReal(const float* input,
                               std::vector<float>& outputDB,
                               std::vector<std::complex<float>>& outputCplx) {
    const int N = fftSize_;
    const int bins = N / 2 + 1;
    outputDB.resize(bins);
    outputCplx.resize(bins);

    std::copy(input, input + N, realIn_);
    fftwf_execute(realPlan_);

    const float scale = 1.0f / N;
    for (int i = 0; i < bins; ++i) {
        float re = realOut_[i][0] * scale;
        float im = realOut_[i][1] * scale;
        outputCplx[i] = {re, im};
        float mag2 = re * re + im * im;
        outputDB[i] = (mag2 > 1e-20f) ? 10.0f * std::log10(mag2) : -200.0f;
    }
}

void FFTProcessor::processComplex(const float* inputIQ,
                                  std::vector<float>& outputDB,
                                  std::vector<std::complex<float>>& outputCplx) {
    const int N = fftSize_;
    outputDB.resize(N);
    outputCplx.resize(N);

    for (int i = 0; i < N; ++i) {
        cplxIn_[i][0] = inputIQ[2 * i];
        cplxIn_[i][1] = inputIQ[2 * i + 1];
    }

    fftwf_execute(cplxPlan_);

    const float scale = 1.0f / N;
    const int half = N / 2;
    for (int i = 0; i < N; ++i) {
        int src = (i + half) % N;
        float re = cplxOut_[src][0] * scale;
        float im = cplxOut_[src][1] * scale;
        outputCplx[i] = {re, im};
        float mag2 = re * re + im * im;
        outputDB[i] = (mag2 > 1e-20f) ? 10.0f * std::log10(mag2) : -200.0f;
    }
}

// Convenience overloads (no complex output).
void FFTProcessor::processReal(const float* input, std::vector<float>& outputDB) {
    std::vector<std::complex<float>> dummy;
    processReal(input, outputDB, dummy);
}

void FFTProcessor::processComplex(const float* inputIQ, std::vector<float>& outputDB) {
    std::vector<std::complex<float>> dummy;
    processComplex(inputIQ, outputDB, dummy);
}

} // namespace baudline
