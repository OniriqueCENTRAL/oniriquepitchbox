#include "PitchMarks.h"

#include <algorithm>
#include <cmath>

namespace pb
{

int PitchMarkSet::nearest (double samplePosition) const noexcept
{
    if (marks.empty())
        return -1;

    auto it = std::lower_bound (marks.begin(), marks.end(), samplePosition,
                                [] (const PitchMark& m, double v) { return (double) m.position < v; });

    if (it == marks.begin())
        return 0;

    if (it == marks.end())
        return (int) marks.size() - 1;

    const auto prev = it - 1;
    const double dPrev = samplePosition - (double) prev->position;
    const double dNext = (double) it->position - samplePosition;

    return (int) ((dPrev <= dNext ? prev : it) - marks.begin());
}

//==============================================================================
std::vector<float> buildPeriodCurve (const PitchTrack& track, int64_t numSamples)
{
    std::vector<float> curve ((size_t) numSamples, 0.0f);

    if (numSamples <= 0)
        return curve;

    // Collect voiced anchors (sample index -> period in samples).
    std::vector<std::pair<int64_t, float>> anchors;
    anchors.reserve (track.frames.size());

    for (const auto& f : track.frames)
        if (f.voiced && f.f0Hz > 0.0f)
            anchors.emplace_back (f.sampleIndex, (float) (track.sampleRate / (double) f.f0Hz));

    if (anchors.empty())
    {
        // No pitch anywhere: use a neutral 100 Hz grain size.
        std::fill (curve.begin(), curve.end(), (float) (track.sampleRate / 100.0));
        return curve;
    }

    size_t a = 0;
    for (int64_t i = 0; i < numSamples; ++i)
    {
        while (a + 1 < anchors.size() && anchors[a + 1].first <= i)
            ++a;

        if (i <= anchors.front().first)
        {
            curve[(size_t) i] = anchors.front().second;
        }
        else if (i >= anchors.back().first)
        {
            curve[(size_t) i] = anchors.back().second;
        }
        else
        {
            const auto& lo = anchors[a];
            const auto& hi = anchors[std::min (a + 1, anchors.size() - 1)];
            const double span = (double) (hi.first - lo.first);
            const double t = span > 0.0 ? (double) (i - lo.first) / span : 0.0;
            curve[(size_t) i] = (float) (lo.second + t * (hi.second - lo.second));
        }
    }

    return curve;
}

//==============================================================================
namespace
{

/** Zero-phase 2nd-order low pass (forward then backward). */
void lowPassZeroPhase (std::vector<float>& x, double sampleRate, double cutoffHz)
{
    const double w = std::tan (M_PI * cutoffHz / sampleRate);
    const double k = 1.41421356; // Butterworth Q
    const double norm = 1.0 / (1.0 + k * w + w * w);

    const double b0 = w * w * norm;
    const double b1 = 2.0 * b0;
    const double b2 = b0;
    const double a1 = 2.0 * (w * w - 1.0) * norm;
    const double a2 = (1.0 - k * w + w * w) * norm;

    auto runPass = [&] (bool reverse)
    {
        double z1 = 0.0, z2 = 0.0, y1 = 0.0, y2 = 0.0;
        const int64_t n = (int64_t) x.size();

        for (int64_t idx = 0; idx < n; ++idx)
        {
            const int64_t i = reverse ? n - 1 - idx : idx;
            const double in = x[(size_t) i];
            const double out = b0 * in + b1 * z1 + b2 * z2 - a1 * y1 - a2 * y2;
            z2 = z1; z1 = in;
            y2 = y1; y1 = out;
            x[(size_t) i] = (float) out;
        }
    };

    runPass (false);
    runPass (true);
}

/** Is this frame's sample position inside a voiced frame? */
std::vector<uint8_t> buildVoicedMask (const PitchTrack& track, int64_t numSamples)
{
    std::vector<uint8_t> mask ((size_t) numSamples, 0);

    if (track.frames.empty())
        return mask;

    const int64_t half = track.hopSize / 2;

    for (const auto& f : track.frames)
    {
        if (! f.voiced)
            continue;

        const int64_t lo = std::max<int64_t> (0, f.sampleIndex - half);
        const int64_t hi = std::min<int64_t> (numSamples, f.sampleIndex + half);

        for (int64_t i = lo; i < hi; ++i)
            mask[(size_t) i] = 1;
    }

    return mask;
}

} // namespace

PitchMarkSet detectPitchMarks (const float* samples,
                               int64_t numSamples,
                               const PitchTrack& track,
                               const std::vector<float>& periodCurve)
{
    PitchMarkSet result;

    if (numSamples <= 0 || periodCurve.empty())
        return result;

    // Low-passed copy: keeps the pulse shape, drops the formant ringing that
    // would otherwise create spurious peaks between glottal closures.
    std::vector<float> lp (samples, samples + numSamples);
    lowPassZeroPhase (lp, track.sampleRate, 900.0);

    const auto voicedMask = buildVoicedMask (track, numSamples);

    auto periodAt = [&] (int64_t i) -> float
    {
        i = std::clamp<int64_t> (i, 0, (int64_t) periodCurve.size() - 1);
        return std::max (8.0f, periodCurve[(size_t) i]);
    };

    // Peak of |lp| inside [lo, hi), preferring the signed maximum so every mark
    // sits on the same side of the waveform.
    auto peakIn = [&] (int64_t lo, int64_t hi) -> int64_t
    {
        lo = std::max<int64_t> (0, lo);
        hi = std::min<int64_t> (numSamples, hi);
        if (lo >= hi)
            return -1;

        int64_t best = lo;
        float bestVal = lp[(size_t) lo];

        for (int64_t i = lo + 1; i < hi; ++i)
        {
            if (lp[(size_t) i] > bestVal)
            {
                bestVal = lp[(size_t) i];
                best = i;
            }
        }

        return best;
    };

    // Seed on the strongest peak of the first period-and-a-half of the file.
    const float firstPeriod = periodAt (0);
    int64_t cursor = peakIn (0, (int64_t) (1.5f * firstPeriod));
    if (cursor < 0)
        cursor = 0;

    // --- walk backwards to the start of the file
    std::vector<PitchMark> backward;
    {
        int64_t pos = cursor;
        while (pos > 0)
        {
            const float T = periodAt (pos);
            const int64_t centre = pos - (int64_t) std::lround (T);
            if (centre <= 0)
                break;

            const int64_t lo = centre - (int64_t) std::lround (0.35f * T);
            const int64_t hi = centre + (int64_t) std::lround (0.35f * T) + 1;
            const int64_t found = peakIn (lo, hi);
            pos = found >= 0 ? found : centre;

            if (pos <= 0)
                break;

            PitchMark m;
            m.position = pos;
            m.period   = periodAt (pos);
            m.voiced   = voicedMask[(size_t) pos] != 0;
            backward.push_back (m);
        }
    }

    std::reverse (backward.begin(), backward.end());
    result.marks = std::move (backward);

    // --- the seed, then forwards to the end
    {
        PitchMark m;
        m.position = cursor;
        m.period   = periodAt (cursor);
        m.voiced   = voicedMask[(size_t) cursor] != 0;
        result.marks.push_back (m);
    }

    {
        int64_t pos = cursor;
        while (pos < numSamples - 1)
        {
            const float T = periodAt (pos);
            const int64_t centre = pos + (int64_t) std::lround (T);
            if (centre >= numSamples)
                break;

            const int64_t lo = centre - (int64_t) std::lround (0.35f * T);
            const int64_t hi = centre + (int64_t) std::lround (0.35f * T) + 1;
            const int64_t found = peakIn (lo, hi);
            int64_t next = found >= 0 ? found : centre;

            // Guard against the search collapsing onto the current mark.
            if (next <= pos)
                next = pos + (int64_t) std::lround (T);

            if (next >= numSamples)
                break;

            pos = next;

            PitchMark m;
            m.position = pos;
            m.period   = periodAt (pos);
            m.voiced   = voicedMask[(size_t) pos] != 0;
            result.marks.push_back (m);
        }
    }

    double widest = 16.0;

    for (const auto& m : result.marks)
        widest = std::max (widest, (double) m.period);

    for (size_t i = 1; i < result.marks.size(); ++i)
        widest = std::max (widest, (double) (result.marks[i].position - result.marks[i - 1].position));

    result.widestSpacing = widest;

    // The pauses: runs of unvoiced marks where the voice has really stopped.
    //
    // A long gap always is one. A short one need not be: when the pitch moves
    // quickly from one note to the next the detector can lose it for a few
    // hundredths of a second while the voice carries straight on, and treating
    // that as a pause would put a join, and the singer's own pitch, in the
    // middle of every such slide. What tells the two apart is the level. A
    // consonant closes the mouth and the sound all but goes; a slide is as
    // loud as the notes either side of it.
    const auto shortest = (int64_t) std::lround (0.02 * track.sampleRate);
    const auto certain  = (int64_t) std::lround (0.05 * track.sampleRate);
    const auto around   = (int64_t) std::lround (0.03 * track.sampleRate);
    const auto& m = result.marks;
    constexpr int64_t kForever = (int64_t) 1 << 60;

    result.pauseEdge = std::max<int64_t> (1, (int64_t) std::lround (0.008 * track.sampleRate));

    auto level = [samples, numSamples] (int64_t from, int64_t to)
    {
        from = std::clamp<int64_t> (from, 0, numSamples);
        to   = std::clamp<int64_t> (to, from, numSamples);

        if (to <= from)
            return 0.0;

        double sum = 0.0;

        for (auto i = from; i < to; ++i)
            sum += (double) samples[i] * (double) samples[i];

        return std::sqrt (sum / (double) (to - from));
    };

    for (size_t i = 0; i < m.size();)
    {
        if (m[i].voiced)
        {
            ++i;
            continue;
        }

        auto j = i;

        while (j < m.size() && ! m[j].voiced)
            ++j;

        const auto atStart = i == 0, atEnd = j == m.size();
        const auto begin = atStart ? -kForever : m[i].position;
        const auto end   = atEnd ? kForever : m[j].position;

        auto isPause = atStart || atEnd || end - begin >= certain;

        if (! isPause && end - begin >= shortest)
            isPause = level (begin, end) < 0.25 * std::max (level (begin - around, begin), level (end, end + around));

        if (isPause)
            result.pauses.push_back ({ begin, end });

        i = j;
    }
    return result;
}

} // namespace pb
