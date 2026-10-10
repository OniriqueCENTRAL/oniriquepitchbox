// Drives the transfer session the way a host without ARA does: arm, play the
// passage past it, stop, then play it back in blocks of uneven size. The one
// test here that needs JUCE, because the session lives on its timer and fifo.
#include "TestSignal.h"

#include "../Source/transfer/TransferSession.h"

#include <cstdio>

using namespace pb;
using onirique::TransferSession;

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
constexpr int kMaxBlock = 512;
constexpr int64_t kSongStart = 123456;

std::vector<float> makeTake()
{
    std::vector<float> src;
    const std::vector<double> midi { 57, 60, 62, 64, 62, 60 };

    for (size_t i = 0; i < midi.size(); ++i)
    {
        const double detune = ((double) ((i * 37) % 71) - 35.0);
        auto note = pbtest::makeVoice (kSr, 0.5, midiToHz (midi[i] + detune / 100.0), 30.0, 5.4);
        const int fade = (int) (0.012 * kSr);

        for (int j = 0; j < fade; ++j)
        {
            const float g = (float) j / (float) fade;
            note[(size_t) j] *= g;
            note[note.size() - 1 - (size_t) j] *= g;
        }

        src.insert (src.end(), note.begin(), note.end());
        src.insert (src.end(), (size_t) (0.08 * kSr), 0.0f);
    }

    return src;
}

void pump (int milliseconds)
{
    juce::MessageManager::getInstance()->runDispatchLoopUntil (milliseconds);
}

juce::Optional<juce::AudioPlayHead::PositionInfo> positionAt (int64_t time, bool playing)
{
    juce::AudioPlayHead::PositionInfo info;
    info.setIsPlaying (playing);
    info.setTimeInSamples (time);
    return info;
}

/** Block sizes the way FL Studio hands them out: never the same twice. */
int nextBlockSize (int index)
{
    static const int sizes[] = { 512, 64, 441, 128, 7, 512, 300, 256, 1, 480 };
    return sizes[index % 10];
}

/** Plays [from, to) of the song through the session. `input` is what the host
    puts in the buffer; the result is what comes out, left channel then right. */
std::array<std::vector<float>, 2> play (TransferSession& session, int64_t from, int64_t to, float input)
{
    std::array<std::vector<float>, 2> out;
    juce::AudioBuffer<float> buffer (2, kMaxBlock);

    int index = 0;

    for (auto pos = from; pos < to;)
    {
        const auto count = (int) std::min<int64_t> (nextBlockSize (index++), to - pos);
        juce::AudioBuffer<float> block (buffer.getArrayOfWritePointers(), 2, count);

        for (int c = 0; c < 2; ++c)
            juce::FloatVectorOperations::fill (block.getWritePointer (c), input, count);

        session.process (block, positionAt (pos, true));

        for (size_t c = 0; c < 2; ++c)
            out[c].insert (out[c].end(), block.getReadPointer ((int) c), block.getReadPointer ((int) c) + count);

        pos += count;
    }

    return out;
}

/** Arms a transfer and plays `left`/`right` past the session from `start`. */
void transfer (TransferSession& session, const std::vector<float>& left,
               const std::vector<float>& right, int64_t start)
{
    session.toggleTransfer();

    juce::AudioBuffer<float> buffer (2, kMaxBlock);
    const auto n = (int64_t) left.size();
    int index = 0;

    // Stopped transport first: armed must wait for play.
    {
        juce::AudioBuffer<float> block (buffer.getArrayOfWritePointers(), 2, 256);
        block.clear();
        session.process (block, positionAt (start, false));
    }

    for (int64_t pos = 0; pos < n;)
    {
        const auto count = (int) std::min<int64_t> (nextBlockSize (index++), n - pos);
        juce::AudioBuffer<float> block (buffer.getArrayOfWritePointers(), 2, count);

        block.copyFrom (0, 0, left.data() + pos, count);
        block.copyFrom (1, 0, right.data() + pos, count);

        session.process (block, positionAt (start + pos, true));
        pos += count;

        // The test runs far faster than real time; give the timer its turn
        // before the fifo fills.
        if (index % 60 == 0)
            pump (60);
    }

    {
        juce::AudioBuffer<float> block (buffer.getArrayOfWritePointers(), 2, 256);
        block.clear();
        session.process (block, positionAt (start + n, false));
    }
}

bool waitForAnalysis (TransferSession& session)
{
    for (int i = 0; i < 600; ++i)
    {
        pump (50);

        if (session.getState() == TransferSession::State::idle && ! session.isAnalysing())
            return session.getAnalysis() != nullptr;
    }

    return false;
}

double maxDifference (const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size())
        return 1.0e9;

    double worst = 0.0;

    for (size_t i = 0; i < a.size(); ++i)
        worst = std::max (worst, (double) std::abs (a[i] - b[i]));

    return worst;
}
}

int main()
{
    const juce::ScopedJuceInitialiser_GUI messageLoop;

    std::printf ("TransferSession\n");

    const auto left = makeTake();
    auto right = left;

    for (auto& s : right)
        s *= 0.5f;   // a different right channel, so a channel mix-up shows

    const auto n = (int64_t) left.size();
    char d[200];

    juce::MemoryBlock saved;
    std::array<std::vector<float>, 2> corrected;

    {
        TransferSession session;
        session.prepare (kSr, kMaxBlock, 2);

        check (session.getState() == TransferSession::State::idle, "starts idle", "");

        transfer (session, left, right, kSongStart);
        const auto analysed = waitForAnalysis (session);
        auto analysis = session.getAnalysis();

        std::snprintf (d, sizeof d, "%zu notes from %.2fs", analysis ? analysis->notes.size() : 0,
                       session.getRecordedSeconds());
        check (analysed && ! analysis->notes.empty(), "transfer records and analyses", d);

        if (! analysed)
            return 1;

        std::snprintf (d, sizeof d, "%lld of %lld samples", (long long) analysis->numSamples, (long long) n);
        check (analysis->numSamples == n, "whole passage captured", d);

        // With nothing to correct, the take comes back exactly — and it is the
        // take that plays, not the track: the input here is a constant.
        const auto margin = (int64_t) 4000;
        const auto plain = play (session, kSongStart - margin, kSongStart + n + margin, 0.25f);

        const std::vector<float> inside (plain[0].begin() + margin, plain[0].begin() + margin + n);
        const std::vector<float> insideRight (plain[1].begin() + margin, plain[1].begin() + margin + n);

        std::snprintf (d, sizeof d, "max error L %.2g R %.2g", maxDifference (inside, left),
                       maxDifference (insideRight, right));
        check (maxDifference (inside, left) == 0.0 && maxDifference (insideRight, right) == 0.0,
               "uncorrected take plays back exactly", d);

        bool outsideUntouched = true;

        for (int64_t i = 0; i < margin; ++i)
            outsideUntouched = outsideUntouched
                               && plain[0][(size_t) i] == 0.25f
                               && plain[0][(size_t) (margin + n + i)] == 0.25f;

        check (outsideUntouched, "track passes through outside the take", "");

        // Now correct it, and hold the session to what the renderer produces
        // on its own in even blocks.
        auto edits = session.getEdits();
        edits.settings.scale.root = 9;
        edits.settings.scale.type = ScaleType::naturalMinor;
        edits.settings.globalAmount = 1.0f;
        edits.refreshUntouched();
        session.setEdits (edits);
        edits = session.getEdits();

        corrected = play (session, kSongStart, kSongStart + n, 0.0f);

        RegionRenderer reference;
        reference.prepare (kMaxBlock, kSr);
        reference.setAnalysis (analysis);

        const auto pad = (int64_t) (0.5 * kSr);
        std::vector<float> padded ((size_t) (n + 2 * pad), 0.0f);
        std::copy (left.begin(), left.end(), padded.begin() + pad);

        std::vector<float> expected ((size_t) n, 0.0f);

        for (int64_t pos = 0; pos < n; pos += kMaxBlock)
        {
            const auto len = std::min<int64_t> (kMaxBlock, n - pos);
            reference.render (padded.data(), -pad, (int64_t) padded.size(), edits,
                              pos, pos + len, expected.data() + pos, pos);
        }

        std::snprintf (d, sizeof d, "max error %.2g", maxDifference (corrected[0], expected));
        check (maxDifference (corrected[0], expected) == 0.0, "uneven blocks match the renderer", d);

        std::snprintf (d, sizeof d, "differs from the take by %.2g", maxDifference (corrected[0], left));
        check (maxDifference (corrected[0], left) > 0.01, "correction is audible", d);

        session.saveState (saved);
        std::snprintf (d, sizeof d, "%.0f kB for %.1fs stereo", (double) saved.getSize() / 1024.0,
                       (double) n / kSr);
        check (saved.getSize() > 0 && saved.getSize() < (size_t) n * 2 * sizeof (float),
               "state is saved, compressed", d);
    }

    {
        // A project reloaded: no editor, no transfer, just the saved state.
        TransferSession session;
        session.prepare (kSr, kMaxBlock, 2);
        session.restoreState (saved.getData(), (int) saved.getSize());

        const auto analysed = waitForAnalysis (session);
        check (analysed, "restored take is analysed again", "");

        if (analysed)
        {
            const auto replayed = play (session, kSongStart, kSongStart + n, 0.0f);

            // The take is stored as 24-bit FLAC scaled to its own peak, so the
            // reload is exact to within that word length and no further.
            double peak = 0.0;

            for (const auto s : corrected[0])
                peak = std::max (peak, (double) std::abs (s));

            const auto errorLeft = maxDifference (replayed[0], corrected[0]) / peak;
            const auto errorRight = maxDifference (replayed[1], corrected[1]) / peak;

            std::snprintf (d, sizeof d, "max error L %.1f dB R %.1f dB",
                           20.0 * std::log10 (errorLeft + 1.0e-30),
                           20.0 * std::log10 (errorRight + 1.0e-30));
            check (errorLeft < 1.0e-6 && errorRight < 1.0e-6, "reloaded project sounds the same", d);
        }
    }

    {
        // A loop back to the start must end the take, not splice into it.
        TransferSession session;
        session.prepare (kSr, kMaxBlock, 2);
        session.toggleTransfer();

        juce::AudioBuffer<float> block (2, kMaxBlock);
        block.clear();

        for (int i = 0; i < 40; ++i)
            session.process (block, positionAt (kSongStart + (int64_t) i * kMaxBlock, true));

        session.process (block, positionAt (kSongStart, true));

        check (session.getState() == TransferSession::State::finishing, "a jump ends the recording", "");

        pump (200);

        std::snprintf (d, sizeof d, "%.3fs kept", session.getRecordedSeconds());
        check (std::abs (session.getRecordedSeconds() - 40.0 * kMaxBlock / kSr) < 1.0e-9
                   && session.getTakeSampleRate() > 0.0,
               "and keeps what came before it", d);
    }

    std::printf ("%s\n", failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
