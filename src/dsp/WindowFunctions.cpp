#include "dsp/WindowFunctions.h"
#include <cmath>
#include <numeric>

namespace baudmine {

static constexpr double kPi = 3.14159265358979323846;

void WindowFunctions::generate(WindowType type, int size, std::vector<float>& out,
                               float kaiserBeta) {
    out.resize(size);
    switch (type) {
        case WindowType::Rectangular:   rectangular(size, out);              break;
        case WindowType::Hann:          hann(size, out);                     break;
        case WindowType::Hamming:       hamming(size, out);                  break;
        case WindowType::Blackman:      blackman(size, out);                 break;
        case WindowType::BlackmanHarris: blackmanHarris(size, out);          break;
        case WindowType::Kaiser:        kaiser(size, out, kaiserBeta);       break;
        case WindowType::FlatTop:       flatTop(size, out);                  break;
        default:                        rectangular(size, out);              break;
    }
}

void WindowFunctions::apply(const std::vector<float>& window, float* data, int size) {
    for (int i = 0; i < size; ++i)
        data[i] *= window[i];
}

float WindowFunctions::coherentGain(const std::vector<float>& window) {
    if (window.empty()) return 1.0f;
    double sum = 0.0;
    for (float w : window) sum += w;
    return static_cast<float>(sum / window.size());
}

// ── Window implementations ───────────────────────────────────────────────────

void WindowFunctions::rectangular(int N, std::vector<float>& w) {
    for (int i = 0; i < N; ++i)
        w[i] = 1.0f;
}

void WindowFunctions::hann(int N, std::vector<float>& w) {
    for (int i = 0; i < N; ++i)
        w[i] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi * i / (N - 1))));
}

void WindowFunctions::hamming(int N, std::vector<float>& w) {
    for (int i = 0; i < N; ++i)
        w[i] = static_cast<float>(0.54 - 0.46 * std::cos(2.0 * kPi * i / (N - 1)));
}

void WindowFunctions::blackman(int N, std::vector<float>& w) {
    for (int i = 0; i < N; ++i) {
        double x = 2.0 * kPi * i / (N - 1);
        w[i] = static_cast<float>(0.42 - 0.5 * std::cos(x) + 0.08 * std::cos(2.0 * x));
    }
}

void WindowFunctions::blackmanHarris(int N, std::vector<float>& w) {
    constexpr double a0 = 0.35875, a1 = 0.48829, a2 = 0.14128, a3 = 0.01168;
    for (int i = 0; i < N; ++i) {
        double x = 2.0 * kPi * i / (N - 1);
        w[i] = static_cast<float>(a0 - a1 * std::cos(x)
                                     + a2 * std::cos(2.0 * x)
                                     - a3 * std::cos(3.0 * x));
    }
}

void WindowFunctions::kaiser(int N, std::vector<float>& w, float beta) {
    double denom = besselI0(beta);
    for (int i = 0; i < N; ++i) {
        double t = 2.0 * i / (N - 1) - 1.0;
        w[i] = static_cast<float>(besselI0(beta * std::sqrt(1.0 - t * t)) / denom);
    }
}

void WindowFunctions::flatTop(int N, std::vector<float>& w) {
    constexpr double a0 = 0.21557895, a1 = 0.41663158, a2 = 0.277263158;
    constexpr double a3 = 0.083578947, a4 = 0.006947368;
    for (int i = 0; i < N; ++i) {
        double x = 2.0 * kPi * i / (N - 1);
        w[i] = static_cast<float>(a0 - a1 * std::cos(x) + a2 * std::cos(2.0 * x)
                                     - a3 * std::cos(3.0 * x) + a4 * std::cos(4.0 * x));
    }
}

// Modified Bessel function of the first kind, order 0.
double WindowFunctions::besselI0(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 30; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

} // namespace baudmine
