#pragma once

#include "core/Types.h"
#include <vector>

namespace baudmine {

class WindowFunctions {
public:
    // Fill `out` with the window coefficients for the given type and size.
    static void generate(WindowType type, int size, std::vector<float>& out,
                         float kaiserBeta = 9.0f);

    // Apply window in-place: data[i] *= window[i].
    static void apply(const std::vector<float>& window, float* data, int size);

    // Coherent gain of the window (sum / N), used for amplitude correction.
    static float coherentGain(const std::vector<float>& window);

private:
    static void rectangular(int N, std::vector<float>& w);
    static void hann(int N, std::vector<float>& w);
    static void hamming(int N, std::vector<float>& w);
    static void blackman(int N, std::vector<float>& w);
    static void blackmanHarris(int N, std::vector<float>& w);
    static void kaiser(int N, std::vector<float>& w, float beta);
    static void flatTop(int N, std::vector<float>& w);

    static double besselI0(double x);
};

} // namespace baudmine
