#include "Fft.h"

#include <cassert>
#include <cmath>

namespace pb
{

int nextPowerOfTwo (int v) noexcept
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

Fft::Fft (int sizePowerOfTwo)
    : n (sizePowerOfTwo)
{
    assert (n > 0 && (n & (n - 1)) == 0);

    bitReverse.resize ((size_t) n);
    int bits = 0;
    while ((1 << bits) < n)
        ++bits;

    for (int i = 0; i < n; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b))
                r |= 1 << (bits - 1 - b);
        bitReverse[(size_t) i] = r;
    }

    // Twiddles laid out stage by stage: for len = 2,4,8,... store len/2 factors.
    twiddles.reserve ((size_t) n);
    for (int len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * M_PI / (double) len;
        for (int k = 0; k < len / 2; ++k)
            twiddles.push_back (std::complex<float> ((float) std::cos (ang * k),
                                                     (float) std::sin (ang * k)));
    }
}

void Fft::transform (std::vector<std::complex<float>>& data, bool invert) const
{
    assert ((int) data.size() == n);

    for (int i = 0; i < n; ++i)
    {
        const int j = bitReverse[(size_t) i];
        if (i < j)
            std::swap (data[(size_t) i], data[(size_t) j]);
    }

    // Real arithmetic rather than std::complex's operator*, which outside fast
    // math checks every product for infinities and runs several times slower.
    auto* d = reinterpret_cast<float*> (data.data());
    const auto* tw = reinterpret_cast<const float*> (twiddles.data());
    const float sign = invert ? -1.0f : 1.0f;
    size_t twBase = 0;

    for (int len = 2; len <= n; len <<= 1)
    {
        const int half = len / 2;

        for (int start = 0; start < n; start += len)
        {
            for (int k = 0; k < half; ++k)
            {
                const float wr = tw[2 * (twBase + (size_t) k)];
                const float wi = sign * tw[2 * (twBase + (size_t) k) + 1];

                float* a = d + 2 * (start + k);
                float* b = d + 2 * (start + k + half);

                const float vr = b[0] * wr - b[1] * wi;
                const float vi = b[0] * wi + b[1] * wr;
                const float ur = a[0], ui = a[1];

                a[0] = ur + vr;  a[1] = ui + vi;
                b[0] = ur - vr;  b[1] = ui - vi;
            }
        }

        twBase += (size_t) half;
    }

    if (invert)
    {
        const float scale = 1.0f / (float) n;
        for (auto& c : data)
            c *= scale;
    }
}

void Fft::forward (std::vector<std::complex<float>>& data) const { transform (data, false); }
void Fft::inverse (std::vector<std::complex<float>>& data) const { transform (data, true); }

} // namespace pb
