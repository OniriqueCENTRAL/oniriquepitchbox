// Onirique PitchBox — minimal radix-2 FFT (no external dependency)
#pragma once

#include <complex>
#include <vector>

namespace pb
{

/** Iterative in-place radix-2 complex FFT. Size is a power of two. */
class Fft
{
public:
    explicit Fft (int sizePowerOfTwo);

    int size() const noexcept { return n; }

    /** In-place forward transform. data.size() must equal size(). */
    void forward (std::vector<std::complex<float>>& data) const;

    /** In-place inverse transform, scaled by 1/N. */
    void inverse (std::vector<std::complex<float>>& data) const;

private:
    void transform (std::vector<std::complex<float>>& data, bool invert) const;

    int n = 0;
    std::vector<int> bitReverse;
    std::vector<std::complex<float>> twiddles; // forward twiddles, one block per stage
};

/** Smallest power of two >= v. */
int nextPowerOfTwo (int v) noexcept;

} // namespace pb
