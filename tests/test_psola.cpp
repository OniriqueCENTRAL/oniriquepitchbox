// Checks the PSOLA renderer: transparency at unity, accuracy under shift,
// formant preservation, and range/full-render agreement.
#include "../Source/dsp/Psola.h"
#include "../Source/dsp/YinPitchTracker.h"
#include "TestSignal.h"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace pb;
using namespace pbtest;

namespace
{

struct Analysed
{
    std::vector<float> audio;
    PitchTrack         track;
    std::vector<float> periodCurve;
    PitchMarkSet       marks;
};

Analysed analyse (double sr, double seconds, double f0, double vibCents = 0.0, double vibHz = 0.0)
{
    Analysed a;
    a.audio = makeVoice (sr, seconds, f0, vibCents, vibHz);

    YinPitchTracker::Params p;
    p.sampleRate = sr;
    YinPitchTracker tracker (p);

    a.track = tracker.process (a.audio.data(), (int64_t) a.audio.size());
    medianSmoothPitchTrack (a.track);
    a.periodCurve = buildPeriodCurve (a.track, (int64_t) a.audio.size());
    a.marks = detectPitchMarks (a.audio.data(), (int64_t) a.audio.size(), a.track, a.periodCurve);
    return a;
}

double measureF0 (const std::vector<float>& x, double sr)
{
    YinPitchTracker::Params p;
    p.sampleRate = sr;
    YinPitchTracker tracker (p);

    auto track = tracker.process (x.data(), (int64_t) x.size());
    medianSmoothPitchTrack (track);

    std::vector<float> f;
    for (const auto& fr : track.frames)
        if (fr.voiced)
            f.push_back (fr.f0Hz);

    if (f.empty())
        return 0.0;

    std::sort (f.begin(), f.end());
    return f[f.size() / 2];
}

int failures = 0;

void check (bool ok, const char* label, const char* detail)
{
    std::printf ("  %-34s %s  %s\n", label, ok ? "ok  " : "FAIL", detail);
    if (! ok)
        ++failures;
}

} // namespace

int main()
{
    const double sr = 48000.0;
    std::printf ("PSOLA renderer\n");

    // ---- 1. Unity ratio must be transparent.
    {
        auto a = analyse (sr, 1.2, 196.0);
        std::vector<float> ratio (a.audio.size(), 1.0f);
        std::vector<float> out (a.audio.size(), 0.0f);

        psolaRender (a.audio.data(), (int64_t) a.audio.size(), a.marks,
                     ratio.data(), 1.0f, out.data());

        const int64_t n = (int64_t) a.audio.size();
        const double snr = snrDb (a.audio, out, n / 5, n * 4 / 5);

        char buf[128];
        std::snprintf (buf, sizeof buf, "SNR %.1f dB vs source", snr);
        check (snr > 30.0, "unity ratio is transparent", buf);
    }

    // ---- 2. Shifts must land on the requested interval.
    for (double cents : { -700.0, -300.0, -100.0, 100.0, 300.0, 700.0 })
    {
        auto a = analyse (sr, 1.2, 196.0);
        const float r = (float) std::pow (2.0, cents / 1200.0);

        std::vector<float> ratio (a.audio.size(), r);
        std::vector<float> out (a.audio.size(), 0.0f);

        psolaRender (a.audio.data(), (int64_t) a.audio.size(), a.marks,
                     ratio.data(), 1.0f, out.data());

        const double measured = measureF0 (out, sr);
        const double expected = 196.0 * r;
        const double errCents = 1200.0 * std::log2 (measured / expected);

        char label[64], buf[128];
        std::snprintf (label, sizeof label, "shift %+.0f cents", cents);
        std::snprintf (buf, sizeof buf, "got %.2f Hz, want %.2f Hz (%+.2f cents)",
                       measured, expected, errCents);
        check (std::abs (errCents) < 12.0, label, buf);
    }

    // ---- 3. Formants must stay put while the pitch moves.
    {
        auto a = analyse (sr, 1.5, 196.0);
        const float r = (float) std::pow (2.0, 700.0 / 1200.0); // up a fifth

        std::vector<float> ratio (a.audio.size(), r);
        std::vector<float> out (a.audio.size(), 0.0f);

        psolaRender (a.audio.data(), (int64_t) a.audio.size(), a.marks,
                     ratio.data(), 1.0f, out.data());

        const double before = spectralCentroid (a.audio, sr);
        const double after  = spectralCentroid (out, sr);
        const double drift  = after / before;

        char buf[160];
        std::snprintf (buf, sizeof buf,
                       "centroid %.0f -> %.0f Hz (x%.3f; resampling would give x%.3f)",
                       before, after, drift, (double) r);
        check (drift > 0.88 && drift < 1.14, "formants preserved under shift", buf);
    }

    // ---- 4. A partial re-render must match the full render sample for sample.
    {
        auto a = analyse (sr, 1.5, 196.0, 40.0, 5.0);
        const int64_t n = (int64_t) a.audio.size();

        std::vector<float> ratio ((size_t) n);
        for (int64_t i = 0; i < n; ++i)
            ratio[(size_t) i] = (float) std::pow (2.0, (i < n / 2 ? -150.0 : 220.0) / 1200.0);

        std::vector<float> full ((size_t) n, 0.0f), partial ((size_t) n, 0.0f);

        psolaRender (a.audio.data(), n, a.marks, ratio.data(), 1.0f, full.data());

        const int64_t from = n / 3, to = 2 * n / 3;
        std::copy (a.audio.begin(), a.audio.end(), partial.begin());
        psolaRenderRange (a.audio.data(), n, a.marks, ratio.data(), 1.0f, from, to, partial.data());

        const double snr = snrDb (full, partial, from, to);

        char buf[128];
        std::snprintf (buf, sizeof buf, "agreement %.1f dB over the re-rendered span", snr);
        check (snr > 60.0, "partial render matches full render", buf);
    }

    // ---- 5. Formant control must actually move the envelope.
    {
        auto a = analyse (sr, 1.5, 196.0);
        std::vector<float> ratio (a.audio.size(), 1.0f);
        std::vector<float> up (a.audio.size(), 0.0f);

        const float fr = (float) std::pow (2.0, 3.0 / 12.0); // +3 semitones of envelope
        psolaRender (a.audio.data(), (int64_t) a.audio.size(), a.marks,
                     ratio.data(), fr, up.data());

        const double before = spectralCentroid (a.audio, sr);
        const double after  = spectralCentroid (up, sr);
        const double moved  = after / before;

        char buf[128];
        std::snprintf (buf, sizeof buf, "centroid x%.3f for a x%.3f request", moved, (double) fr);
        check (moved > 1.05, "formant shift moves the envelope", buf);
    }

    std::printf (failures == 0 ? "ALL PASS\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
