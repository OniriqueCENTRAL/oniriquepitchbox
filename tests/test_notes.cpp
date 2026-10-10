// End-to-end: synthesise a sung phrase, analyse it, correct it, render it,
// then re-analyse the render and check it landed where the editor said it would.
#include "TestSignal.h"

#include "../Source/dsp/PitchMarks.h"
#include "../Source/dsp/Psola.h"
#include "../Source/dsp/Scale.h"
#include "../Source/dsp/YinPitchTracker.h"
#include "../Source/model/Notes.h"

#include <cstdio>
#include <vector>

using namespace pb;

namespace
{
int failures = 0;

void check (bool ok, const char* name, const char* detail)
{
    std::printf ("  %-42s %s   %s\n", name, ok ? "ok  " : "FAIL", detail);
    if (! ok)
        ++failures;
}

constexpr double kSr = 48000.0;

/** A phrase of separate notes, each with vibrato, each a little out of tune. */
std::vector<float> makePhrase (const std::vector<double>& midiNotes,
                               const std::vector<double>& detuneCents,
                               double noteSeconds,
                               double gapSeconds)
{
    std::vector<float> out;

    for (size_t i = 0; i < midiNotes.size(); ++i)
    {
        const double hz = midiToHz (midiNotes[i] + detuneCents[i] / 100.0);
        auto note = pbtest::makeVoice (kSr, noteSeconds, hz, 28.0, 5.2);

        // Fade the edges so note boundaries are clean transients, not clicks.
        const int fade = (int) (0.012 * kSr);
        for (int j = 0; j < fade; ++j)
        {
            const float g = (float) j / (float) fade;
            note[(size_t) j] *= g;
            note[note.size() - 1 - (size_t) j] *= g;
        }

        out.insert (out.end(), note.begin(), note.end());
        out.insert (out.end(), (size_t) (gapSeconds * kSr), 0.0f);
    }

    return out;
}

PitchTrack analyse (const std::vector<float>& x)
{
    YinPitchTracker::Params p;
    p.sampleRate = kSr;
    p.windowSize = 2048;
    p.hopSize    = 256;
    YinPitchTracker tracker (p);
    auto track = tracker.process (x.data(), (int64_t) x.size());
    medianSmoothPitchTrack (track);
    return track;
}

/** Median pitch of the middle of a note in the rendered signal. */
double measuredCents (const PitchTrack& track, int64_t from, int64_t to)
{
    std::vector<double> cents;

    for (const auto& f : track.frames)
        if (f.voiced && f.sampleIndex >= from && f.sampleIndex < to)
            cents.push_back (hzToCents (f.f0Hz));

    if (cents.empty())
        return 0.0;

    std::sort (cents.begin(), cents.end());
    return cents[cents.size() / 2];
}

/** Peak-to-peak pitch excursion inside a note — the vibrato depth. */
double vibratoDepthCents (const PitchTrack& track, int64_t from, int64_t to)
{
    double lo = 1e9, hi = -1e9;

    for (const auto& f : track.frames)
    {
        if (f.voiced && f.sampleIndex >= from && f.sampleIndex < to)
        {
            const auto c = hzToCents (f.f0Hz);
            lo = std::min (lo, c);
            hi = std::max (hi, c);
        }
    }

    return hi > lo ? hi - lo : 0.0;
}

std::vector<float> render (const std::vector<float>& src,
                           const PitchTrack& track,
                           const std::vector<Note>& notes,
                           const CorrectionSettings& settings)
{
    const auto n = (int64_t) src.size();
    const auto periods = buildPeriodCurve (track, n);
    const auto marks   = detectPitchMarks (src.data(), n, track, periods);

    std::vector<float> ratio ((size_t) n, 1.0f);
    buildPitchRatioCurve (track, notes, settings, 0, n, ratio.data());

    std::vector<float> out ((size_t) n, 0.0f);
    psolaRender (src.data(), n, marks, ratio.data(), settings.formantRatio, out.data());
    return out;
}
}

int main()
{
    std::printf ("Note model end to end\n");

    // C major-ish phrase, every note sung flat or sharp by a different amount.
    const std::vector<double> midiNotes   { 60, 62, 64, 65, 67 };
    const std::vector<double> detune      { -38, +27, -45, +18, -31 };
    const double noteSeconds = 0.55, gapSeconds = 0.12;

    auto src = makePhrase (midiNotes, detune, noteSeconds, gapSeconds);
    auto track = analyse (src);
    auto notes = detectNotes (track);

    {
        char d[160];
        std::snprintf (d, sizeof d, "found %zu notes, expected %zu", notes.size(), midiNotes.size());
        check (notes.size() == midiNotes.size(), "segmentation finds every note", d);
    }

    if (notes.size() != midiNotes.size())
    {
        std::printf ("\nsegmentation wrong, later checks would be meaningless\n");
        return 1;
    }

    // Each detected note should sit where it was actually sung.
    {
        double worst = 0.0;

        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto want = midiNotes[i] * 100.0 + detune[i];
            worst = std::max (worst, std::abs (notes[i].centsDetected - want));
        }

        char d[160];
        std::snprintf (d, sizeof d, "worst note centre off by %.1f cents", worst);
        check (worst < 12.0, "detected pitch matches what was sung", d);
    }

    // Key detection should find C major from the histogram.
    {
        const auto hist = pitchClassHistogram (notes);
        const auto est  = estimateScale (hist);
        char d[160];
        std::snprintf (d, sizeof d, "%s (confidence %.2f)", est.scale.describe().c_str(), est.confidence);
        check (est.scale.root == 0 && (est.scale.type == ScaleType::major
                                       || est.scale.type == ScaleType::majorPentatonic),
               "key detection finds C major", d);
    }

    // Full correction to the chromatic grid: every note should land in tune.
    {
        CorrectionSettings s;
        s.scale.root = 0;
        s.scale.type = ScaleType::major;
        s.globalAmount = 1.0f;
        applyScaleToNotes (notes, s, {});

        auto out = render (src, track, notes, s);
        auto outTrack = analyse (out);

        double worst = 0.0;

        for (size_t i = 0; i < notes.size(); ++i)
        {
            // Measure the sustained middle, away from the correction ramps.
            const auto from = notes[i].startSample + (int64_t) (0.15 * kSr);
            const auto to   = notes[i].endSample   - (int64_t) (0.10 * kSr);
            const auto got  = measuredCents (outTrack, from, to);
            worst = std::max (worst, std::abs (got - midiNotes[i] * 100.0));
        }

        char d[160];
        std::snprintf (d, sizeof d, "worst note %.1f cents from the grid (was up to 45)", worst);
        check (worst < 12.0, "full correction puts notes in tune", d);
    }

    // Vibrato must survive correction — this is what separates a musical
    // correction from a robot one.
    {
        CorrectionSettings s;
        s.scale.root = 0;
        s.scale.type = ScaleType::major;
        s.globalAmount = 1.0f;
        applyScaleToNotes (notes, s, {});

        auto out = render (src, track, notes, s);
        auto outTrack = analyse (out);

        const auto from = notes[2].startSample + (int64_t) (0.15 * kSr);
        const auto to   = notes[2].endSample   - (int64_t) (0.10 * kSr);
        const auto before = vibratoDepthCents (track, from, to);
        const auto after  = vibratoDepthCents (outTrack, from, to);

        char d[160];
        std::snprintf (d, sizeof d, "depth %.0f -> %.0f cents", before, after);
        check (after > before * 0.7, "vibrato survives full correction", d);
    }

    // vibratoScale = 0 should flatten the note to a straight line.
    {
        CorrectionSettings s;
        s.globalAmount = 0.0f;

        for (auto& n : notes)
        {
            n.targetCents   = n.centsDetected;
            n.correctAmount = 0.0f;
            n.vibratoScale  = 0.0f;
        }

        auto out = render (src, track, notes, s);
        auto outTrack = analyse (out);

        const auto from = notes[2].startSample + (int64_t) (0.15 * kSr);
        const auto to   = notes[2].endSample   - (int64_t) (0.10 * kSr);
        const auto before = vibratoDepthCents (track, from, to);
        const auto after  = vibratoDepthCents (outTrack, from, to);

        char d[160];
        std::snprintf (d, sizeof d, "depth %.0f -> %.0f cents", before, after);
        check (after < before * 0.45, "vibratoScale 0 straightens the note", d);

        for (auto& n : notes)
            n.vibratoScale = 1.0f;
    }

    // Correction off must be bit-transparent through the whole chain.
    {
        CorrectionSettings s;
        s.enabled = false;

        auto out = render (src, track, notes, s);
        const auto snr = pbtest::snrDb (src, out, (int64_t) (0.1 * kSr), (int64_t) src.size() - (int64_t) (0.1 * kSr));

        char d[160];
        std::snprintf (d, sizeof d, "SNR %.1f dB vs source", snr);
        check (snr > 55.0, "correction disabled is transparent", d);
    }

    // The property ARA depends on: rendering a block in isolation must equal
    // rendering the whole take and slicing out that block.
    {
        CorrectionSettings s;
        s.scale.root = 0;
        s.scale.type = ScaleType::major;
        s.globalAmount = 1.0f;
        applyScaleToNotes (notes, s, {});

        const auto n = (int64_t) src.size();
        const auto periods = buildPeriodCurve (track, n);
        const auto marks   = detectPitchMarks (src.data(), n, track, periods);

        std::vector<float> ratio ((size_t) n, 1.0f);
        buildPitchRatioCurve (track, notes, s, 0, n, ratio.data());

        std::vector<float> full ((size_t) n, 0.0f);
        psolaRender (src.data(), n, marks, ratio.data(), s.formantRatio, full.data());

        // Now the same thing in 512-sample host blocks, each rendered cold.
        std::vector<float> blocked ((size_t) n, 0.0f);
        const int64_t block = 512;

        for (int64_t pos = 0; pos < n; pos += block)
        {
            const auto len = std::min (block, n - pos);

            std::vector<float> blockRatio ((size_t) len, 1.0f);
            buildPitchRatioCurve (track, notes, s, pos, len, blockRatio.data());

            // A real renderer asks for just this span; psolaRenderRange reads
            // whatever source context it needs around it.
            std::vector<float> tmp ((size_t) n, 0.0f);
            std::vector<float> fullRatio ((size_t) n, 1.0f);
            buildPitchRatioCurve (track, notes, s, 0, n, fullRatio.data());
            psolaRenderRange (src.data(), n, marks, fullRatio.data(), s.formantRatio,
                              pos, pos + len, tmp.data());

            for (int64_t i = 0; i < len; ++i)
                blocked[(size_t) (pos + i)] = tmp[(size_t) (pos + i)];

            // The per-block ratio curve must agree with the global one.
            for (int64_t i = 0; i < len; ++i)
                if (std::abs (blockRatio[(size_t) i] - fullRatio[(size_t) (pos + i)]) > 1.0e-6f)
                {
                    std::printf ("    ratio curve mismatch at %lld\n", (long long) (pos + i));
                    ++failures;
                    return 1;
                }
        }

        const auto snr = pbtest::snrDb (full, blocked, 0, n);
        char d[160];
        std::snprintf (d, sizeof d, "agreement %.1f dB over the whole take", snr);
        check (snr > 120.0, "block rendering matches full rendering", d);
    }

    std::printf (failures == 0 ? "ALL PASS\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
