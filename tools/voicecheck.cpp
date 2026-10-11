// Runs the analysis and the renderer over a real recording, off line, and
// reports on what the synthesis cursor did. The synthetic voice the tests use
// is too regular to show how the renderer behaves on a singer.
//
//   pitchbox_voicecheck in.(wav|ogg|flac) outDir [seconds]
#include <juce_audio_formats/juce_audio_formats.h>

#include "../Source/model/Analysis.h"
#include "../Source/model/EditState.h"
#include "../Source/model/RegionRenderer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

using namespace pb;

namespace
{
std::vector<float> renderAll (const Analysis::Ptr& analysis, const std::vector<float>& src, const EditState& edits,
                              int kBlock = 1024, int64_t startAt = 0, bool twicePerBlock = false)
{
    const auto n = (int64_t) src.size();
    const auto pad = (int64_t) (0.5 * analysis->sampleRate);

    std::vector<float> padded ((size_t) (n + 2 * pad), 0.0f);
    std::copy (src.begin(), src.end(), padded.begin() + pad);

    RegionRenderer renderer;
    renderer.prepare (kBlock, analysis->sampleRate);
    renderer.setAnalysis (analysis);

    std::vector<float> out ((size_t) n, 0.0f), again ((size_t) kBlock, 0.0f);

    for (int64_t pos = startAt; pos < n; pos += kBlock)
    {
        const auto len = std::min<int64_t> (kBlock, n - pos);
        renderer.render (padded.data(), -pad, (int64_t) padded.size(), edits, pos, pos + len, out.data() + pos, pos);

        // The second channel of a stereo pair: the same block, asked for again.
        if (twicePerBlock)
        {
            renderer.render (padded.data(), -pad, (int64_t) padded.size(), edits, pos, pos + len, again.data(), pos);

            for (int64_t i = 0; i < len; ++i)
                if (again[(size_t) i] != out[(size_t) (pos + i)])
                    out[(size_t) (pos + i)] = 1.0e9f;   // make the disagreement impossible to miss
        }
    }

    return out;
}

/** Clicks per second: places where the waveform bends far more sharply than
    anything around it does. The bend is the second difference; a spot counts
    when it is eight times the typical bend of the surrounding 40 ms, and loud
    enough to matter. A voice has a few of its own, on consonants. */
double clicksPerSecond (const std::vector<float>& x, double rate)
{
    const auto n = (int64_t) x.size();
    const auto half = (int64_t) (0.02 * rate);

    std::vector<float> bend ((size_t) n, 0.0f);
    double peak = 0.0;

    for (int64_t i = 2; i < n; ++i)
    {
        bend[(size_t) i] = std::abs (x[(size_t) i] - 2.0f * x[(size_t) i - 1] + x[(size_t) i - 2]);
        peak = std::max (peak, (double) std::abs (x[(size_t) i]));
    }

    std::vector<double> sum ((size_t) n + 1, 0.0);

    for (int64_t i = 0; i < n; ++i)
        sum[(size_t) i + 1] = sum[(size_t) i] + (double) bend[(size_t) i];

    int64_t clicks = 0, lastClick = -100000;

    for (int64_t i = half; i < n - half; ++i)
    {
        const auto typical = (sum[(size_t) (i + half)] - sum[(size_t) (i - half)]) / (double) (2 * half);

        if ((double) bend[(size_t) i] > 8.0 * typical && (double) bend[(size_t) i] > 0.02 * peak
            && i - lastClick > half)
        {
            ++clicks;
            lastClick = i;
        }
    }

    return (double) clicks / ((double) n / rate);
}
/** Clicks in the quiet between phrases, where there is nothing to hide them:
    a sharp bend in a passage that was all but silent just before. Reported
    against the reference so the ones the recording already had can be told
    from the ones the rendering added, and with where each sits relative to
    the phase anchors. */
void reportQuietClicks (const char* label, const std::vector<float>& x, const std::vector<float>& reference,
                        const Analysis& analysis)
{
    const auto n = (int64_t) x.size();
    const auto rate = analysis.sampleRate;
    double peak = 0.0;

    for (const auto s : reference)
        peak = std::max (peak, (double) std::abs (s));

    auto bendAt = [] (const std::vector<float>& y, int64_t i)
    {
        return (double) std::abs (y[(size_t) i] - 2.0f * y[(size_t) i - 1] + y[(size_t) i - 2]);
    };

    int count = 0;

    for (int64_t i = 1300; i < n - 10; ++i)
    {
        const auto bend = bendAt (x, i);

        if (bend < 0.008 * peak)
            continue;

        double energy = 0.0;

        for (auto k = i - 1200; k < i - 300; ++k)
            energy += (double) x[(size_t) k] * x[(size_t) k];

        if (std::sqrt (energy / 900.0) > 0.012 * peak)
            continue;

        // Was it there in the recording?
        double original = 0.0;

        for (auto k = i - 4; k <= i + 4; ++k)
            original = std::max (original, bendAt (reference, k));

        if (original > 0.5 * bend)
        {
            i += 2000;
            continue;
        }

        int64_t nearestAnchor = 0, nearestDistance = 1 << 30;

        for (const auto a : analysis.anchors)
            if (std::abs (a - i) < std::abs (nearestDistance))
            {
                nearestDistance = i - a;
                nearestAnchor = a;
            }

        const auto mark = analysis.marks.nearest ((double) i);
        const auto& m = analysis.marks.marks[(size_t) mark];

        if (count < 12)
            std::printf ("      %s  %.4f s  bend %.4f  %+lld from anchor %.4f s  mark %s, spacing %lld\n",
                         label, (double) i / rate, bend, (long long) nearestDistance, (double) nearestAnchor / rate,
                         m.voiced ? "voiced" : "unvoiced",
                         (long long) (mark + 1 < (int) analysis.marks.marks.size()
                                          ? analysis.marks.marks[(size_t) mark + 1].position - m.position : 0));

        ++count;
        i += 2000;
    }

    std::printf ("  clicks added in quiet passages, %s: %d\n", label, count);
}
/** Sags where the voice stops and starts. At each end of a pause the level of
    the rendering is followed in short steps against the recording's; a place
    where it drops below 70% while the recording is not quiet is two
    out-of-step copies of the voice being faded through one another, which is
    heard as a crackle on the join. */
void reportEdgeSags (const char* label, const std::vector<float>& x, const std::vector<float>& reference,
                     const Analysis& analysis)
{
    const auto n = (int64_t) x.size();
    const auto rate = analysis.sampleRate;
    // Long enough to hold two cycles of a low voice, or the level being
    // followed would rise and fall with the cycle itself.
    const auto win = (int64_t) (0.012 * rate);
    double peak = 0.0;

    for (const auto s : reference)
        peak = std::max (peak, (double) std::abs (s));

    auto level = [win] (const std::vector<float>& y, int64_t from)
    {
        double sum = 0.0;

        for (auto i = from; i < from + win; ++i)
            sum += (double) y[(size_t) i] * y[(size_t) i];

        return std::sqrt (sum / (double) win);
    };

    int edges = 0, sagging = 0, shown = 0;
    double worst = 1.0;

    auto look = [&] (int64_t edge, const char* which)
    {
        if (edge < (int64_t) (0.05 * rate) || edge > n - (int64_t) (0.05 * rate))
            return;

        double lowest = 1.0;
        bool loud = false;

        for (auto at = edge - (int64_t) (0.02 * rate); at < edge + (int64_t) (0.02 * rate); at += win / 4)
        {
            const auto original = level (reference, at);

            if (original < 0.04 * peak)
                continue;

            loud = true;
            lowest = std::min (lowest, level (x, at) / original);
        }

        if (! loud)
            return;

        ++edges;
        worst = std::min (worst, lowest);

        if (lowest < 0.7)
        {
            ++sagging;

            if (shown++ < 6)
                std::printf ("      %s  %.4f s  %s  down to %.0f%%\n", label, (double) edge / rate, which, 100.0 * lowest);
        }
    };

    for (const auto& p : analysis.marks.pauses)
    {
        look (p.begin, "voice stops");
        look (p.end, "voice returns");
    }

    std::printf ("  sags at the ends of pauses, %s: %d of %d audible edges, worst %.0f%%\n",
                 label, sagging, edges, 100.0 * worst);
}
/** Sharp events in the rendering that the recording does not have anywhere
    near, wherever they fall: each with how far it is from the nearest end of a
    pause and from the nearest note boundary, to show what it goes with. */
std::vector<int64_t> sharpEvents (const std::vector<float>& x, double rate, double factor)
{
    const auto n = (int64_t) x.size();
    const auto half = (int64_t) (0.02 * rate);
    std::vector<double> sum ((size_t) n + 1, 0.0);
    std::vector<float> bend ((size_t) n, 0.0f);
    double peak = 0.0;

    for (int64_t i = 2; i < n; ++i)
    {
        bend[(size_t) i] = std::abs (x[(size_t) i] - 2.0f * x[(size_t) i - 1] + x[(size_t) i - 2]);
        peak = std::max (peak, (double) std::abs (x[(size_t) i]));
    }

    for (int64_t i = 0; i < n; ++i)
        sum[(size_t) i + 1] = sum[(size_t) i] + (double) bend[(size_t) i];

    std::vector<int64_t> events;
    int64_t last = -100000;

    for (int64_t i = half; i < n - half; ++i)
    {
        const auto typical = (sum[(size_t) (i + half)] - sum[(size_t) (i - half)]) / (double) (2 * half);

        if ((double) bend[(size_t) i] > factor * typical && (double) bend[(size_t) i] > 0.004 * peak && i - last > half / 2)
        {
            events.push_back (i);
            last = i;
        }
    }

    return events;
}

void reportAddedEvents (const char* label, const std::vector<float>& x, const std::vector<float>& reference,
                        const Analysis& analysis)
{
    const auto rate = analysis.sampleRate;
    const auto mine = sharpEvents (x, rate, 7.0);
    const auto theirs = sharpEvents (reference, rate, 4.5);
    const auto near = (int64_t) (0.03 * rate);

    int added = 0, atEdges = 0, atNotes = 0;

    for (const auto at : mine)
    {
        bool known = false;

        for (const auto other : theirs)
            known = known || std::abs (other - at) < near;

        if (known)
            continue;

        int64_t toEdge = 1 << 30, toNote = 1 << 30;
        const char* edgeKind = "";

        for (const auto& p : analysis.marks.pauses)
        {
            if (std::abs (at - p.begin) < std::abs (toEdge)) { toEdge = at - p.begin; edgeKind = "stop"; }
            if (std::abs (at - p.end) < std::abs (toEdge))   { toEdge = at - p.end;   edgeKind = "return"; }
        }

        for (const auto& note : analysis.notes)
        {
            if (std::abs (at - note.startSample) < std::abs (toNote)) toNote = at - note.startSample;
            if (std::abs (at - note.endSample) < std::abs (toNote))   toNote = at - note.endSample;
        }

        atEdges += std::abs (toEdge) < near ? 1 : 0;
        atNotes += std::abs (toNote) < near ? 1 : 0;

        if (added++ < 14)
            std::printf ("      %s  %.4f s  value %+.3f  %+.1f ms from a %s, %+.1f ms from a note boundary\n",
                         label, (double) at / rate, x[(size_t) at], 1000.0 * (double) toEdge / rate, edgeKind,
                         1000.0 * (double) toNote / rate);
    }

    std::printf ("  sharp events added, %s: %d (%d by a pause edge, %d by a note boundary)\n",
                 label, added, atEdges, atNotes);
}
double worstDifference (const std::vector<float>& a, const std::vector<float>& b, int64_t from)
{
    double worst = 0.0;

    for (auto i = (size_t) from; i < a.size(); ++i)
        worst = std::max (worst, (double) std::abs (a[i] - b[i]));

    return worst;
}

void writeWav (const juce::File& file, const std::vector<float>& x, double rate)
{
    file.deleteFile();

    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();

    if (stream == nullptr)
        return;

    juce::WavAudioFormat format;
    auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                      .withSampleRate (rate)
                                                      .withNumChannels (1)
                                                      .withBitsPerSample (24));

    if (writer == nullptr)
        return;

    const float* channels[] = { x.data() };
    writer->writeFromFloatArrays (channels, 1, (int) x.size());
}

/** How regular the pitch of a signal is from one cycle to the next.

    Finds the cycle length around each point by matching the signal against
    itself, and reports how much consecutive cycles differ. A voice moves
    smoothly, so neighbouring cycles are within a fraction of a percent of one
    another; a cycle cut short stands out as a jump. Returned as the share of
    cycles, in voiced audio, that differ from the one before by more than 4%. */
double irregularCycles (const std::vector<float>& x, const Analysis& analysis, double shiftRatio, double threshold = 0.04)
{
    const auto rate = analysis.sampleRate;
    int64_t jumps = 0, cycles = 0;

    for (const auto& frame : analysis.track.frames)
    {
        if (! frame.voiced || frame.f0Hz <= 0.0f)
            continue;

        // Walk a few cycles from this frame, measuring each against the next.
        const auto expected = rate / ((double) frame.f0Hz * shiftRatio);
        auto at = (double) frame.sampleIndex;
        double previous = 0.0;

        for (int k = 0; k < 4; ++k)
        {
            const auto lo = (int64_t) (0.8 * expected), hi = (int64_t) (1.25 * expected);
            const auto window = (int64_t) expected;
            const auto start = (int64_t) at;

            if (start < 0 || start + hi + window + 1 >= (int64_t) x.size())
                break;

            double best = -2.0;
            int64_t bestLag = lo;

            for (auto lag = lo; lag <= hi; ++lag)
            {
                double cross = 0.0, a = 0.0, b = 0.0;

                for (int64_t i = 0; i < window; ++i)
                {
                    const auto p = (double) x[(size_t) (start + i)], q = (double) x[(size_t) (start + lag + i)];
                    cross += p * q;
                    a += p * p;
                    b += q * q;
                }

                const auto score = cross / (std::sqrt (a * b) + 1.0e-12);

                if (score > best)
                {
                    best = score;
                    bestLag = lag;
                }
            }

            if (best < 0.8)
                break;

            if (previous > 0.0)
            {
                ++cycles;

                if (std::abs ((double) bestLag - previous) > threshold * previous)
                    ++jumps;
            }

            previous = (double) bestLag;
            at += (double) bestLag;
        }
    }

    return cycles > 0 ? (double) jumps / (double) cycles : 0.0;
}
}

int main (int argc, char** argv)
{
    if (argc < 3)
        return 2;

    const auto input = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (argv[1]));
    const auto outDir = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (argv[2]));
    const auto seconds = argc > 3 ? juce::String (argv[3]).getDoubleValue() : 20.0;

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (input));

    if (reader == nullptr)
    {
        std::printf ("cannot read %s\n", input.getFullPathName().toRawUTF8());
        return 1;
    }

    const auto rate = reader->sampleRate;
    const auto n = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (seconds * rate));

    juce::AudioBuffer<float> buffer ((int) reader->numChannels, n);
    reader->read (&buffer, 0, n, 0, true, true);

    std::vector<float> mono ((size_t) n, 0.0f);

    for (int c = 0; c < buffer.getNumChannels(); ++c)
        for (int i = 0; i < n; ++i)
            mono[(size_t) i] += buffer.getSample (c, i) / (float) buffer.getNumChannels();

    AnalysisParams params;
    params.sampleRate = rate;

    const auto analysis = runAnalysis (mono.data(), n, params);

    if (analysis == nullptr)
    {
        std::printf ("analysis failed\n");
        return 1;
    }

    int64_t voicedFrames = 0;

    for (const auto& f : analysis->track.frames)
        voicedFrames += f.voiced ? 1 : 0;

    const auto voicedSeconds = (double) voicedFrames * analysis->track.hopSize / rate;

    std::printf ("%s\n", input.getFileName().toRawUTF8());
    std::printf ("  %.1f s at %.0f Hz, %.1f s voiced, %zu notes, %zu marks, %zu anchors (%.1f per voiced second)\n",
                 (double) n / rate, rate, voicedSeconds, analysis->notes.size(),
                 analysis->marks.marks.size(), analysis->anchors.size(),
                 (double) analysis->anchors.size() / std::max (0.001, voicedSeconds));

    std::printf ("  clicks per second, original          : %5.2f\n", clicksPerSecond (mono, rate));

    outDir.createDirectory();
    writeWav (outDir.getChildFile ("1 original.wav"), mono, rate);

    std::printf ("  irregular cycles, original           : %5.2f %%   broken (off by a fifth or more): %5.3f %%\n",
                 100.0 * irregularCycles (mono, *analysis, 1.0), 100.0 * irregularCycles (mono, *analysis, 1.0, 0.2));

    // Every note up a semitone: the plainest thing a pitch editor does.
    {
        EditState edits;
        edits.adoptAnalysis (*analysis);

        for (size_t i = 0; i < edits.notes.size(); ++i)
        {
            edits.notes[i].targetCents = edits.notes[i].centsDetected + 100.0f;
            edits.notes[i].correctAmount = 1.0f;
            edits.touched[i] = true;
        }

        edits.settings.transitionSeconds = 0.0;
        edits.bump();

        const auto started = std::chrono::steady_clock::now();
        const auto out = renderAll (analysis, mono, edits);
        const auto took = std::chrono::duration<double> (std::chrono::steady_clock::now() - started).count();

        writeWav (outDir.getChildFile ("2 up a semitone.wav"), out, rate);
        std::printf ("  clicks per second, up a semitone     : %5.2f\n", clicksPerSecond (out, rate));
        reportQuietClicks ("semitone", out, mono, *analysis);
        reportEdgeSags ("semitone", out, mono, *analysis);
        reportAddedEvents ("semitone", out, mono, *analysis);

        // The same audio whatever the host does: other block sizes, each block
        // asked for twice, and playback started part of the way through.
        const auto middle = (int64_t) (0.37 * n);

        std::printf ("  block invariance, worst difference   : 64: %g   7 twice: %g   from the middle: %g\n",
                     worstDifference (out, renderAll (analysis, mono, edits, 64), 0),
                     worstDifference (out, renderAll (analysis, mono, edits, 7 * 31, 0, true), 0),
                     worstDifference (out, renderAll (analysis, mono, edits, 300, middle), middle));
        std::printf ("  irregular cycles, up a semitone      : %5.2f %%   broken: %5.3f %%   (rendered at %.0fx real time)\n",
                     100.0 * irregularCycles (out, *analysis, std::pow (2.0, 1.0 / 12.0)),
                     100.0 * irregularCycles (out, *analysis, std::pow (2.0, 1.0 / 12.0), 0.2),
                     ((double) n / rate) / took);
    }

    // Notes moved by hand, each somewhere different from its neighbours: the
    // joins between them then have a long way to travel.
    {
        EditState edits;
        edits.adoptAnalysis (*analysis);

        static const float moves[] = { 200.0f, -100.0f, 0.0f, 300.0f, -200.0f };

        for (size_t i = 0; i < edits.notes.size(); ++i)
        {
            edits.notes[i].targetCents = edits.notes[i].centsDetected + moves[i % 5];
            edits.notes[i].correctAmount = 1.0f;
            edits.touched[i] = true;
        }

        edits.bump();

        const auto out = renderAll (analysis, mono, edits);
        writeWav (outDir.getChildFile ("4 notes moved apart.wav"), out, rate);
        reportQuietClicks ("moved", out, mono, *analysis);
        reportEdgeSags ("moved", out, mono, *analysis);
        reportAddedEvents ("moved", out, mono, *analysis);
    }
    // A group of notes turned down, nothing else touched. Within a phrase the
    // level should then sit 12 dB down throughout; anywhere it jumps back up
    // between two of the notes is a burst at full volume.
    {
        EditState edits;
        edits.adoptAnalysis (*analysis);

        for (size_t i = 0; i < edits.notes.size(); ++i)
        {
            edits.notes[i].gainDb = -12.0f;
            edits.touched[i] = true;
        }

        edits.bump();

        const auto out = renderAll (analysis, mono, edits);
        writeWav (outDir.getChildFile ("5 notes turned down.wav"), out, rate);

        const auto win = (int64_t) (0.012 * rate);
        const auto legato = (int64_t) (0.1 * rate);
        double peak = 0.0;

        for (const auto s : mono)
            peak = std::max (peak, (double) std::abs (s));

        auto level = [win] (const std::vector<float>& y, int64_t from)
        {
            double sum = 0.0;

            for (auto i = from; i < from + win; ++i)
                sum += (double) y[(size_t) i] * y[(size_t) i];

            return std::sqrt (sum / (double) win);
        };

        int joins = 0, bursts = 0;
        double loudest = 0.0;

        for (size_t i = 0; i + 1 < edits.notes.size(); ++i)
        {
            const auto& a = edits.notes[i];
            const auto& b = edits.notes[i + 1];

            if (b.startSample - a.endSample > legato)
                continue;

            ++joins;
            double worst = 0.0;

            for (auto at = a.endSample - win; at < b.startSample; at += win / 4)
            {
                if (at < 0 || at + win >= (int64_t) out.size())
                    continue;

                const auto original = level (mono, at);

                if (original > 0.03 * peak)
                    worst = std::max (worst, level (out, at) / original);
            }

            loudest = std::max (loudest, worst);
            bursts += worst > 0.5 ? 1 : 0;
        }

        std::printf ("  notes turned down 12 dB: %d of %d joins burst back above -6 dB, loudest %.1f dB\n",
                     bursts, joins, 20.0 * std::log10 (loudest + 1.0e-9));
    }
    // And what the AMOUNT knob does at full: every note onto the nearest semitone.
    {
        EditState edits;
        edits.settings.globalAmount = 1.0f;
        edits.adoptAnalysis (*analysis);

        const auto out = renderAll (analysis, mono, edits);
        writeWav (outDir.getChildFile ("3 amount at full.wav"), out, rate);
        std::printf ("  clicks per second, amount at full    : %5.2f\n", clicksPerSecond (out, rate));
        reportQuietClicks ("amount", out, mono, *analysis);
        reportEdgeSags ("amount", out, mono, *analysis);
        reportAddedEvents ("amount", out, mono, *analysis);
        std::printf ("  irregular cycles, amount at full     : %5.2f %%   broken: %5.3f %%\n",
                     100.0 * irregularCycles (out, *analysis, 1.0), 100.0 * irregularCycles (out, *analysis, 1.0, 0.2));
    }

    return 0;
}
