// Simulates the ARA playback renderer: every block is rendered cold, from a
// windowed read of the source, exactly as the audio thread will do it. The
// result must be identical to an offline render of the whole take, otherwise a
// bounce would not match what the user heard.
#include "TestSignal.h"

#include "../Source/dsp/PitchMarks.h"
#include "../Source/dsp/Psola.h"
#include "../Source/dsp/Scale.h"
#include "../Source/dsp/YinPitchTracker.h"
#include "../Source/model/Notes.h"

#include <chrono>
#include <cstdio>
#include <vector>

using namespace pb;

namespace
{
int failures = 0;

void check (bool ok, const char* name, const char* detail)
{
    std::printf ("  %-44s %s   %s\n", name, ok ? "ok  " : "FAIL", detail);
    if (! ok)
        ++failures;
}

constexpr double kSr = 48000.0;
}

int main()
{
    std::printf ("Real-time block renderer\n");

    // ~8 seconds of singing, long enough that an O(take) renderer would show up.
    std::vector<float> src;
    const std::vector<double> midiNotes { 57, 60, 62, 64, 62, 60, 65, 64, 62, 60, 59, 57 };

    for (size_t i = 0; i < midiNotes.size(); ++i)
    {
        const double detune = ((double) ((i * 37) % 71) - 35.0);
        auto note = pbtest::makeVoice (kSr, 0.55, midiToHz (midiNotes[i] + detune / 100.0), 30.0, 5.4);
        const int fade = (int) (0.012 * kSr);

        for (int j = 0; j < fade; ++j)
        {
            const float g = (float) j / (float) fade;
            note[(size_t) j] *= g;
            note[note.size() - 1 - (size_t) j] *= g;
        }

        src.insert (src.end(), note.begin(), note.end());
        src.insert (src.end(), (size_t) (0.1 * kSr), 0.0f);
    }

    const auto n = (int64_t) src.size();
    std::printf ("  take is %.1f s at %.0f Hz\n", (double) n / kSr, kSr);

    YinPitchTracker::Params p;
    p.sampleRate = kSr;
    YinPitchTracker tracker (p);
    auto track = tracker.process (src.data(), n);
    medianSmoothPitchTrack (track);

    auto notes = detectNotes (track);

    CorrectionSettings settings;
    settings.scale.root = 9;                     // A minor
    settings.scale.type = ScaleType::naturalMinor;
    settings.globalAmount = 1.0f;
    settings.formantRatio = 1.0f;
    applyScaleToNotes (notes, settings, {});

    const auto periods = buildPeriodCurve (track, n);
    const auto marks   = detectPitchMarks (src.data(), n, track, periods);

    std::vector<int64_t> noteStarts;
    for (const auto& note : notes)
        noteStarts.push_back (note.startSample);

    const auto anchors = buildPhaseAnchors (marks, noteStarts, kSr);

    {
        char d[160];
        std::snprintf (d, sizeof d, "%zu anchors over %zu marks", anchors.size(), marks.marks.size());
        check (! anchors.empty(), "phase anchors were placed", d);
    }

    // Reference: one offline render of the whole take.
    std::vector<float> ratioFull ((size_t) n, 1.0f);
    buildPitchRatioCurve (track, notes, settings, 0, n, ratioFull.data());

    std::vector<float> reference ((size_t) n, 0.0f);
    psolaRender (src.data(), n, marks, ratioFull.data(), settings.formantRatio,
                 reference.data(), &anchors);

    const auto pad = sourcePaddingFor (marks, settings.formantRatio);
    {
        char d[160];
        std::snprintf (d, sizeof d, "%lld samples (%.0f ms) of source context per block",
                       (long long) pad, 1000.0 * (double) pad / kSr);
        check (pad < (int64_t) (0.4 * kSr), "source padding stays bounded", d);
    }

    // Now the audio thread's view: block by block, nothing retained between
    // blocks except pre-allocated scratch.
    for (int64_t blockSize : { (int64_t) 64, (int64_t) 512, (int64_t) 2048 })
    {
        std::vector<float> out ((size_t) n, 0.0f);

        PsolaScratch scratch;
        scratch.prepare (blockSize);
        const auto accCapacity = scratch.acc.capacity();

        std::vector<float> window ((size_t) (blockSize + 2 * pad), 0.0f);
        std::vector<float> ratio ((size_t) blockSize, 1.0f);
        std::vector<float> block ((size_t) blockSize, 0.0f);

        const auto t0 = std::chrono::steady_clock::now();

        for (int64_t pos = 0; pos < n; pos += blockSize)
        {
            const auto len = std::min (blockSize, n - pos);

            // The host asks for [pos, pos+len). Read the source window around it,
            // the way ARAAudioSourceReader would.
            const auto winStart = pos - pad;
            const auto winLen   = len + 2 * pad;

            for (int64_t i = 0; i < winLen; ++i)
            {
                const auto s = winStart + i;
                window[(size_t) i] = (s >= 0 && s < n) ? src[(size_t) s] : 0.0f;
            }

            // The ratio curve has to start at the block's phase anchor, since
            // that is where the synthesis cursor starts consulting it.
            const auto anchor = psolaWalkStart (marks, anchors, pos);
            const auto ratioLen = pos + len + pad - anchor;
            ratio.resize ((size_t) ratioLen);
            buildPitchRatioCurve (track, notes, settings, anchor, ratioLen, ratio.data());

            PsolaRequest r;
            r.src          = window.data();
            r.srcOffset    = winStart;
            r.srcLength    = winLen;
            r.marks        = &marks;
            r.anchors      = &anchors;
            r.ratio        = ratio.data();
            r.ratioOffset  = anchor;
            r.ratioLength  = ratioLen;
            r.formantRatio = settings.formantRatio;
            r.firstSample  = pos;
            r.lastSample   = pos + len;
            r.dst          = block.data();
            r.dstOffset    = pos;

            psolaRender (r, scratch);

            for (int64_t i = 0; i < len; ++i)
                out[(size_t) (pos + i)] = block[(size_t) i];
        }

        const auto t1 = std::chrono::steady_clock::now();
        const auto seconds = std::chrono::duration<double> (t1 - t0).count();
        const auto realtimeFactor = ((double) n / kSr) / seconds;

        const auto snr = pbtest::snrDb (reference, out, 0, n);

        char name[80], d[200];
        std::snprintf (name, sizeof name, "block size %lld matches offline render", (long long) blockSize);
        std::snprintf (d, sizeof d, "%.1f dB, %.0fx real time", snr, realtimeFactor);
        check (snr > 120.0, name, d);

        std::snprintf (name, sizeof name, "block size %lld allocated nothing", (long long) blockSize);
        std::snprintf (d, sizeof d, "scratch capacity unchanged at %zu", scratch.acc.capacity());
        check (scratch.acc.capacity() == accCapacity, name, d);
    }

    // Playback starting mid-phrase must sound the same as playback from the top:
    // the host does this every time the user drops the playhead somewhere.
    {
        const int64_t blockSize = 512;
        const int64_t startAt = (n / 2 / blockSize) * blockSize;

        PsolaScratch scratch;
        scratch.prepare (blockSize);
        std::vector<float> window ((size_t) (blockSize + 2 * pad), 0.0f);
        std::vector<float> ratio ((size_t) blockSize, 1.0f);
        std::vector<float> block ((size_t) blockSize, 0.0f);
        std::vector<float> out ((size_t) n, 0.0f);

        for (int64_t pos = startAt; pos < n; pos += blockSize)
        {
            const auto len = std::min (blockSize, n - pos);
            const auto winStart = pos - pad;
            const auto winLen = len + 2 * pad;

            for (int64_t i = 0; i < winLen; ++i)
            {
                const auto s = winStart + i;
                window[(size_t) i] = (s >= 0 && s < n) ? src[(size_t) s] : 0.0f;
            }

            const auto anchor = psolaWalkStart (marks, anchors, pos);
            const auto ratioLen = pos + len + pad - anchor;
            ratio.resize ((size_t) ratioLen);
            buildPitchRatioCurve (track, notes, settings, anchor, ratioLen, ratio.data());

            PsolaRequest r;
            r.src = window.data();           r.srcOffset = winStart;   r.srcLength = winLen;
            r.marks = &marks;                r.anchors = &anchors;
            r.ratio = ratio.data();   r.ratioOffset = anchor;   r.ratioLength = ratioLen;
            r.formantRatio = settings.formantRatio;
            r.firstSample = pos;             r.lastSample = pos + len;
            r.dst = block.data();            r.dstOffset = pos;

            psolaRender (r, scratch);

            for (int64_t i = 0; i < len; ++i)
                out[(size_t) (pos + i)] = block[(size_t) i];
        }

        const auto snr = pbtest::snrDb (reference, out, startAt, n);
        char d[160];
        std::snprintf (d, sizeof d, "%.1f dB from sample %lld onwards", snr, (long long) startAt);
        check (snr > 120.0, "starting playback mid-take is identical", d);
    }

    // An edit must not force a re-analysis: moving a note only rewrites the
    // ratio curve, which is what makes the change audible on the next block.
    {
        notes[4].targetCents = notes[4].centsDetected + 200.0f;
        notes[4].correctAmount = 1.0f;

        const int64_t blockSize = 512;
        const auto probe = notes[4].startSample + (int64_t) (0.25 * kSr);
        const auto pos = (probe / blockSize) * blockSize;

        PsolaScratch scratch;
        scratch.prepare (blockSize);
        std::vector<float> window ((size_t) (blockSize + 2 * pad), 0.0f);
        std::vector<float> ratio ((size_t) blockSize, 1.0f);
        std::vector<float> block ((size_t) blockSize, 0.0f);

        const auto winStart = pos - pad;
        const auto winLen = blockSize + 2 * pad;

        for (int64_t i = 0; i < winLen; ++i)
        {
            const auto s = winStart + i;
            window[(size_t) i] = (s >= 0 && s < n) ? src[(size_t) s] : 0.0f;
        }

        const auto anchor = psolaWalkStart (marks, anchors, pos);
        const auto ratioLen = pos + blockSize + pad - anchor;
        ratio.resize ((size_t) ratioLen);
        buildPitchRatioCurve (track, notes, settings, anchor, ratioLen, ratio.data());

        PsolaRequest r;
        r.src = window.data();      r.srcOffset = winStart;  r.srcLength = winLen;
        r.marks = &marks;           r.anchors = &anchors;
        r.ratio = ratio.data();  r.ratioOffset = anchor;  r.ratioLength = ratioLen;
        r.formantRatio = settings.formantRatio;
        r.firstSample = pos;        r.lastSample = pos + blockSize;
        r.dst = block.data();       r.dstOffset = pos;

        psolaRender (r, scratch);

        // The block should now differ from the reference by about two semitones.
        double meanRatio = 0.0;

        for (int64_t i = 0; i < blockSize; ++i)
            meanRatio += (double) ratio[(size_t) (pos - anchor + i)];

        meanRatio /= (double) blockSize;
        const auto cents = 1200.0 * std::log2 (meanRatio);

        char d[160];
        std::snprintf (d, sizeof d, "block now asks for %+.0f cents without re-analysis", cents);
        check (cents > 150.0 && cents < 260.0, "an edit changes the very next block", d);
    }

    std::printf (failures == 0 ? "ALL PASS\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
