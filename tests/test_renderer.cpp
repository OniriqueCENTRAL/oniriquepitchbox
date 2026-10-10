// Exercises RegionRenderer through the exact interface the ARA playback
// renderer will use, including the correction-curve cache.
#include "TestSignal.h"

#include "../Source/model/Analysis.h"
#include "../Source/model/EditState.h"
#include "../Source/model/RegionRenderer.h"

#include <chrono>
#include <cstdio>

using namespace pb;

namespace
{
int failures = 0;

void check (bool ok, const char* name, const char* detail)
{
    std::printf ("  %-46s %s   %s\n", name, ok ? "ok  " : "FAIL", detail);
    if (! ok)
        ++failures;
}

constexpr double kSr = 48000.0;

std::vector<float> makeTake()
{
    std::vector<float> src;
    const std::vector<double> midi { 57, 60, 62, 64, 62, 60, 65, 64, 62, 60, 59, 57 };

    for (size_t i = 0; i < midi.size(); ++i)
    {
        const double detune = ((double) ((i * 37) % 71) - 35.0);
        auto note = pbtest::makeVoice (kSr, 0.55, midiToHz (midi[i] + detune / 100.0), 30.0, 5.4);
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

    return src;
}

/** Drives the renderer the way a host does: block by block, reading a window
    of the source around each block. */
std::vector<float> renderInBlocks (RegionRenderer& renderer,
                                   const std::vector<float>& src,
                                   const EditState& edits,
                                   int64_t blockSize,
                                   int64_t startAt = 0)
{
    const auto n = (int64_t) src.size();
    std::vector<float> out ((size_t) n, 0.0f);

    const auto pad = renderer.paddingSamples (edits.settings.formantRatio);
    std::vector<float> window ((size_t) (blockSize + 2 * pad), 0.0f);
    std::vector<float> block ((size_t) blockSize, 0.0f);

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

        renderer.render (window.data(), winStart, winLen, edits,
                         pos, pos + len, block.data(), pos);

        for (int64_t i = 0; i < len; ++i)
            out[(size_t) (pos + i)] = block[(size_t) i];
    }

    return out;
}
}

int main()
{
    std::printf ("RegionRenderer\n");

    const auto src = makeTake();
    const auto n = (int64_t) src.size();

    AnalysisParams ap;
    ap.sampleRate = kSr;

    const auto t0 = std::chrono::steady_clock::now();
    auto analysis = runAnalysis (src.data(), n, ap);
    const auto analysisSeconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();

    {
        char d[200];
        std::snprintf (d, sizeof d, "%zu notes, key %s, %.2fs for %.1fs of audio (%.0fx)",
                       analysis ? analysis->notes.size() : 0,
                       analysis ? analysis->key.scale.describe().c_str() : "-",
                       analysisSeconds, (double) n / kSr,
                       ((double) n / kSr) / analysisSeconds);
        check (analysis != nullptr && ! analysis->notes.empty(), "analysis completes", d);
    }

    if (analysis == nullptr)
        return 1;

    EditState edits;
    edits.settings.scale.root = 9;
    edits.settings.scale.type = ScaleType::naturalMinor;
    edits.settings.globalAmount = 1.0f;
    edits.adoptAnalysis (*analysis);

    RegionRenderer renderer;
    renderer.prepare (2048, kSr);
    renderer.setAnalysis (analysis);

    // Reference: one offline render of the whole take through the same code.
    std::vector<float> ratio ((size_t) n, 1.0f);
    buildPitchRatioCurve (analysis->track, edits.notes, edits.settings, 0, n, ratio.data());
    std::vector<float> reference ((size_t) n, 0.0f);
    psolaRender (src.data(), n, analysis->marks, ratio.data(),
                 edits.settings.formantRatio, reference.data(), &analysis->anchors);

    for (int64_t blockSize : { (int64_t) 64, (int64_t) 256, (int64_t) 512, (int64_t) 2048 })
    {
        RegionRenderer r;
        r.prepare ((int) blockSize, kSr);
        r.setAnalysis (analysis);

        const auto t1 = std::chrono::steady_clock::now();
        const auto out = renderInBlocks (r, src, edits, blockSize);
        const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - t1).count();

        const auto snr = pbtest::snrDb (reference, out, 0, n);

        char name[80], d[200];
        std::snprintf (name, sizeof name, "block %4lld matches the offline render", (long long) blockSize);
        std::snprintf (d, sizeof d, "%.1f dB, %.0fx real time", snr, ((double) n / kSr) / seconds);
        check (snr > 120.0, name, d);
    }

    // Dropping the playhead anywhere must give the same audio.
    {
        const int64_t blockSize = 512;
        RegionRenderer r;
        r.prepare ((int) blockSize, kSr);
        r.setAnalysis (analysis);

        bool allMatch = true;
        double worst = 200.0;

        for (int64_t startBlock : { (int64_t) 37, (int64_t) 180, (int64_t) 400 })
        {
            const auto startAt = startBlock * blockSize;

            if (startAt >= n)
                continue;

            const auto out = renderInBlocks (r, src, edits, blockSize, startAt);
            const auto snr = pbtest::snrDb (reference, out, startAt, n);
            worst = std::min (worst, snr);

            if (snr <= 120.0)
                allMatch = false;
        }

        char d[160];
        std::snprintf (d, sizeof d, "worst %.1f dB across three entry points", worst);
        check (allMatch, "starting playback anywhere is identical", d);
    }

    // An edit has to be audible on the next block, with no re-analysis.
    {
        const int64_t blockSize = 512;
        RegionRenderer r;
        r.prepare ((int) blockSize, kSr);
        r.setAnalysis (analysis);

        const auto before = renderInBlocks (r, src, edits, blockSize);

        auto edited = edits;
        edited.notes[5].targetCents = edited.notes[5].centsDetected + 300.0f;
        edited.notes[5].correctAmount = 1.0f;
        edited.markTouched (5);

        const auto analysisPtrBefore = r.getAnalysis();
        const auto after = renderInBlocks (r, src, edited, blockSize);

        // The edited note changed; a note far away did not.
        const auto editedFrom = edited.notes[5].startSample + (int64_t) (0.2 * kSr);
        const auto editedTo   = edited.notes[5].endSample - (int64_t) (0.1 * kSr);
        const auto otherFrom  = edited.notes[1].startSample + (int64_t) (0.2 * kSr);
        const auto otherTo    = edited.notes[1].endSample - (int64_t) (0.1 * kSr);

        const auto changed = pbtest::snrDb (before, after, editedFrom, editedTo);
        const auto untouched = pbtest::snrDb (before, after, otherFrom, otherTo);

        char d[200];
        std::snprintf (d, sizeof d, "edited note %.0f dB, other note %.0f dB, analysis reused: %s",
                       changed, untouched, analysisPtrBefore == r.getAnalysis() ? "yes" : "no");
        check (changed < 20.0 && untouched > 120.0 && analysisPtrBefore == r.getAnalysis(),
               "an edit changes only that note, no re-analysis", d);
    }

    // Correction off must return the take untouched.
    {
        auto off = edits;
        off.settings.enabled = false;
        off.bump();

        RegionRenderer r;
        r.prepare (512, kSr);
        r.setAnalysis (analysis);

        const auto out = renderInBlocks (r, src, off, 512);
        const auto snr = pbtest::snrDb (src, out, 0, n);

        char d[160];
        std::snprintf (d, sizeof d, "SNR %.1f dB vs the original take", snr);
        check (snr > 120.0, "correction off is bit transparent", d);
    }

    // Edits must survive a save/load round trip, and a re-analysis.
    {
        auto edited = edits;
        edited.notes[3].targetCents = edited.notes[3].centsDetected - 150.0f;
        edited.notes[3].correctAmount = 0.8f;
        edited.notes[3].vibratoScale = 0.4f;
        edited.markTouched (3);

        const auto text = edited.toString();
        auto restored = EditState::fromString (text);
        restored.adoptAnalysis (*analysis);

        const auto& a = edited.notes[3];
        const auto& b = restored.notes[3];

        const bool same = std::abs (a.targetCents - b.targetCents) < 0.5f
                          && std::abs (a.correctAmount - b.correctAmount) < 1.0e-3f
                          && std::abs (a.vibratoScale - b.vibratoScale) < 1.0e-3f;

        char d[200];
        std::snprintf (d, sizeof d, "%zu bytes, note 3 target %.1f -> %.1f cents",
                       text.size(), a.targetCents, b.targetCents);
        check (same, "edits survive save, load and re-analysis", d);
    }

    std::printf (failures == 0 ? "ALL PASS\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
