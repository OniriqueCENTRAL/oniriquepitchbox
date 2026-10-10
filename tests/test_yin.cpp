// Numerical check of the YIN tracker against a synthetic voice-like signal.
#include "../Source/dsp/YinPitchTracker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace pb;

// A glottal-ish source: band-limited sawtooth shaped by three fixed formants.
static std::vector<float> makeVoice (double sr, double seconds, double f0,
                                     double vibratoCents, double vibratoHz)
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

        // Band-limited sawtooth by additive synthesis.
        double s = 0.0;
        const int maxH = (int) (0.45 * sr / f);
        for (int h = 1; h <= std::min (maxH, 60); ++h)
            s += std::sin (2.0 * M_PI * h * phase) / (double) h;

        x[(size_t) i] = (float) (0.3 * s);
    }

    // Three resonators to give it a spectral envelope (formants stay put).
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

static int runCase (double sr, double f0, double vibCents, double vibHz, const char* label)
{
    auto x = makeVoice (sr, 1.5, f0, vibCents, vibHz);

    YinPitchTracker::Params p;
    p.sampleRate = sr;
    YinPitchTracker tracker (p);

    auto track = tracker.process (x.data(), (int64_t) x.size());
    medianSmoothPitchTrack (track);

    std::vector<double> errors;
    int voiced = 0;

    for (const auto& f : track.frames)
    {
        if (! f.voiced)
            continue;

        ++voiced;
        const double t = (double) f.sampleIndex / sr;
        const double expected = f0 * std::pow (2.0, vibCents * std::sin (2.0 * M_PI * vibHz * t) / 1200.0);
        errors.push_back (std::abs (1200.0 * std::log2 (f.f0Hz / expected)));
    }

    if (errors.empty())
    {
        std::printf ("  %-28s FAIL  no voiced frames\n", label);
        return 1;
    }

    std::sort (errors.begin(), errors.end());
    const double median = errors[errors.size() / 2];
    const double p95    = errors[(size_t) (0.95 * (double) (errors.size() - 1))];
    const double voicedPct = 100.0 * (double) voiced / (double) track.frames.size();

    const bool ok = median < 5.0 && p95 < 25.0 && voicedPct > 90.0;
    std::printf ("  %-28s %s  median %5.2f cents, p95 %6.2f cents, voiced %.1f%%\n",
                 label, ok ? "ok  " : "FAIL", median, p95, voicedPct);
    return ok ? 0 : 1;
}

int main()
{
    std::printf ("YIN pitch tracker\n");
    int failures = 0;

    failures += runCase (48000.0,  98.0,  0.0, 0.0, "98 Hz steady (low male)");
    failures += runCase (48000.0, 220.0,  0.0, 0.0, "220 Hz steady");
    failures += runCase (48000.0, 220.0, 50.0, 5.5, "220 Hz + 50c vibrato");
    failures += runCase (48000.0, 440.0, 30.0, 6.0, "440 Hz + 30c vibrato");
    failures += runCase (44100.0, 311.1, 20.0, 5.0, "311 Hz at 44.1k");
    failures += runCase (48000.0, 880.0,  0.0, 0.0, "880 Hz steady (high)");

    std::printf (failures == 0 ? "ALL PASS\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
