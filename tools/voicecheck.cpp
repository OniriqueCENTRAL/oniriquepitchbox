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
double irregularCycles (const std::vector<float>& x, const Analysis& analysis, double shiftRatio)
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

                if (std::abs ((double) bestLag - previous) > 0.04 * previous)
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

    outDir.createDirectory();
    writeWav (outDir.getChildFile ("1 original.wav"), mono, rate);

    std::printf ("  irregular cycles, original           : %5.2f %%\n", 100.0 * irregularCycles (mono, *analysis, 1.0));

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

        // The same audio whatever the host does: other block sizes, each block
        // asked for twice, and playback started part of the way through.
        const auto middle = (int64_t) (0.37 * n);

        std::printf ("  block invariance, worst difference   : 64: %g   7 twice: %g   from the middle: %g\n",
                     worstDifference (out, renderAll (analysis, mono, edits, 64), 0),
                     worstDifference (out, renderAll (analysis, mono, edits, 7 * 31, 0, true), 0),
                     worstDifference (out, renderAll (analysis, mono, edits, 300, middle), middle));
        std::printf ("  irregular cycles, up a semitone      : %5.2f %%   (rendered at %.0fx real time)\n",
                     100.0 * irregularCycles (out, *analysis, std::pow (2.0, 1.0 / 12.0)),
                     ((double) n / rate) / took);
    }

    // And what the AMOUNT knob does at full: every note onto the nearest semitone.
    {
        EditState edits;
        edits.settings.globalAmount = 1.0f;
        edits.adoptAnalysis (*analysis);

        const auto out = renderAll (analysis, mono, edits);
        writeWav (outDir.getChildFile ("3 amount at full.wav"), out, rate);
        std::printf ("  irregular cycles, amount at full     : %5.2f %%\n",
                     100.0 * irregularCycles (out, *analysis, 1.0));
    }

    return 0;
}
