// Shared synthetic voice generator for the DSP tests.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include "../Source/dsp/Fft.h"

namespace pbtest
{

/** Band-limited sawtooth through three fixed resonators: the fundamental moves,
    the formants do not, which is exactly what a pitch shifter must preserve. */
inline std::vector<float> makeVoice (double sr, double seconds, double f0,
                                     double vibratoCents = 0.0, double vibratoHz = 0.0)
{
    const int64_t n = (int64_t) (sr * seconds);
    std::vector<float> x ((size_t) n, 0.0f);

    double phase = 0.0;
    for (int64_t i = 0; i < n; ++i)
    {
        const double t = (double) i / sr;
        const double cents = vibratoCents * std::sin (2.0 * M_PI * vibratoHz * t);
        const double f = f0 * std::pow (2.0, cents / 1200.0);

        phase += f / sr;
        if (phase >= 1.0)
            phase -= 1.0;

        double s = 0.0;
        const int maxH = (int) (0.45 * sr / f);
        for (int h = 1; h <= std::min (maxH, 60); ++h)
            s += std::sin (2.0 * M_PI * h * phase) / (double) h;

        x[(size_t) i] = (float) (0.3 * s);
    }

    const double formants[3] = { 700.0, 1220.0, 2600.0 };
    for (double fc : formants)
    {
        const double r = 0.97;
        const double theta = 2.0 * M_PI * fc / sr;
        const double a1 = -2.0 * r * std::cos (theta);
        const double a2 = r * r;
        double z1 = 0.0, z2 = 0.0;

        for (int64_t i = 0; i < n; ++i)
        {
            const double in = x[(size_t) i];
            const double out = in - a1 * z1 - a2 * z2;
            z2 = z1;
            z1 = out;
            x[(size_t) i] = (float) (0.3 * out);
        }
    }

    return x;
}

/** Signal-to-noise ratio in dB of `test` measured against `ref`. */
inline double snrDb (const std::vector<float>& ref, const std::vector<float>& test,
                     int64_t from, int64_t to)
{
    double sig = 0.0, err = 0.0;
    for (int64_t i = from; i < to; ++i)
    {
        sig += (double) ref[(size_t) i] * ref[(size_t) i];
        const double d = (double) test[(size_t) i] - ref[(size_t) i];
        err += d * d;
    }
    if (err <= 0.0)
        return 200.0;
    return 10.0 * std::log10 (sig / err);
}

/** Magnitude-spectrum centroid in Hz over the middle of the signal.
    Pitch shifting with formant preservation leaves it nearly unchanged;
    resampling scales it by the shift ratio. */
inline double spectralCentroid (const std::vector<float>& x, double sr)
{
    const int N = 16384;
    const int64_t start = std::max<int64_t> (0, (int64_t) x.size() / 2 - N / 2);

    std::vector<std::complex<float>> buf ((size_t) N, { 0.0f, 0.0f });
    for (int i = 0; i < N && start + i < (int64_t) x.size(); ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * M_PI * i / (double) N);
        buf[(size_t) i] = std::complex<float> ((float) (w * x[(size_t) (start + i)]), 0.0f);
    }

    pb::Fft fft (N);
    fft.forward (buf);

    double num = 0.0, den = 0.0;
    for (int k = 1; k < N / 2; ++k)
    {
        const double mag = std::abs (buf[(size_t) k]);
        const double f = (double) k * sr / (double) N;
        if (f < 80.0 || f > 6000.0)
            continue;
        num += f * mag;
        den += mag;
    }

    return den > 0.0 ? num / den : 0.0;
}

} // namespace pbtest
