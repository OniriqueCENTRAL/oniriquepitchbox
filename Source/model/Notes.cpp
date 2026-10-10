#include "Notes.h"

#include <algorithm>
#include <cmath>

namespace pb
{

namespace
{
    float medianOf (std::vector<float>& v)
    {
        if (v.empty())
            return 0.0f;

        const auto mid = v.begin() + (long) (v.size() / 2);
        std::nth_element (v.begin(), mid, v.end());
        return *mid;
    }

    /** Smoothstep, so a correction ramp has no corner at either end. */
    inline double smoothStep (double t) noexcept
    {
        t = std::clamp (t, 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }
}

//==============================================================================
std::vector<Note> detectNotes (const PitchTrack& track, const NoteDetectorParams& params)
{
    std::vector<Note> notes;

    if (track.empty())
        return notes;

    const auto hop          = (double) track.hopSize;
    const auto framesPerSec = track.sampleRate / hop;
    const int  maxGapFrames = std::max (1, (int) std::lround (params.maxGapSeconds * framesPerSec));
    const int  minFrames    = std::max (1, (int) std::lround (params.minNoteSeconds * framesPerSec));
    const int  numFrames    = (int) track.frames.size();

    // A note runs while the signal stays voiced and the pitch stays put. We
    // compare against a running median of the note so far rather than against
    // the previous frame, so a vibrato peak does not split the note.
    int  start = -1;
    int  unvoicedRun = 0;
    std::vector<float> centsSoFar;

    auto flush = [&] (int endFrame)
    {
        if (start < 0)
            return;

        const int last = endFrame;

        if (last - start >= minFrames && ! centsSoFar.empty())
        {
            Note n;
            n.firstFrame = start;
            n.lastFrame  = last;

            // Weight the median towards the sustained, loud part of the note.
            float peak = 0.0f;

            for (int i = start; i < last; ++i)
                peak = std::max (peak, track.frames[(size_t) i].rms);

            std::vector<float> loudCents;
            loudCents.reserve ((size_t) (last - start));

            for (int i = start; i < last; ++i)
            {
                const auto& f = track.frames[(size_t) i];

                if (f.voiced && f.rms >= peak * params.quietFraction)
                    loudCents.push_back ((float) hzToCents (f.f0Hz));
            }

            if (loudCents.empty())
                loudCents = centsSoFar;

            n.peakRms       = peak;
            n.centsDetected = medianOf (loudCents);
            n.targetCents   = n.centsDetected;
            n.startSample   = track.frames[(size_t) start].sampleIndex;
            n.endSample     = (last < numFrames ? track.frames[(size_t) last].sampleIndex
                                                : track.frames[(size_t) (last - 1)].sampleIndex
                                                      + (int64_t) track.hopSize);
            notes.push_back (n);
        }

        start = -1;
        centsSoFar.clear();
    };

    for (int i = 0; i < numFrames; ++i)
    {
        const auto& f = track.frames[(size_t) i];

        if (! f.voiced || f.f0Hz <= 0.0f)
        {
            if (start >= 0 && ++unvoicedRun > maxGapFrames)
                flush (i - unvoicedRun + 1);

            continue;
        }

        unvoicedRun = 0;
        const auto cents = (float) hzToCents (f.f0Hz);

        if (start < 0)
        {
            start = i;
            centsSoFar.clear();
            centsSoFar.push_back (cents);
            continue;
        }

        auto copy = centsSoFar;
        const auto reference = medianOf (copy);

        if (std::abs (cents - reference) > params.splitCents)
        {
            // Only split if the new pitch sticks — one frame off is tracker noise.
            bool sustained = true;

            for (int j = i + 1; j < std::min (i + 3, numFrames); ++j)
            {
                const auto& g = track.frames[(size_t) j];

                if (! g.voiced || std::abs ((float) hzToCents (g.f0Hz) - reference) <= params.splitCents)
                {
                    sustained = false;
                    break;
                }
            }

            if (sustained)
            {
                flush (i);
                start = i;
                centsSoFar.push_back (cents);
                continue;
            }
        }

        centsSoFar.push_back (cents);
    }

    flush (numFrames);
    return notes;
}

//==============================================================================
std::array<float, 12> pitchClassHistogram (const std::vector<Note>& notes)
{
    std::array<float, 12> h {};
    h.fill (0.0f);

    for (const auto& n : notes)
    {
        const auto midi = (int) std::lround (n.centsDetected / 100.0);
        const auto pc   = ((midi % 12) + 12) % 12;
        // Longer and louder notes say more about the key than passing tones.
        h[(size_t) pc] += (float) n.lengthSamples() * std::max (n.peakRms, 1.0e-4f);
    }

    float total = 0.0f;

    for (auto v : h)
        total += v;

    if (total > 0.0f)
        for (auto& v : h)
            v /= total;

    return h;
}

//==============================================================================
void applyScaleToNotes (std::vector<Note>& notes,
                        const CorrectionSettings& settings,
                        const std::vector<bool>& touched)
{
    for (size_t i = 0; i < notes.size(); ++i)
    {
        if (i < touched.size() && touched[i])
            continue;

        auto& n = notes[i];
        n.targetCents   = settings.scale.snapCents (n.centsDetected);
        n.correctAmount = settings.globalAmount;
    }
}

//==============================================================================
int noteAtSample (const std::vector<Note>& notes, int64_t sample) noexcept
{
    int lo = 0, hi = (int) notes.size() - 1, found = -1;

    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;

        if (sample < notes[(size_t) mid].startSample)
            hi = mid - 1;
        else if (sample >= notes[(size_t) mid].endSample)
            lo = mid + 1;
        else
        {
            found = mid;
            break;
        }
    }

    return found;
}

namespace
{
    /** Linear interpolation of the analysed pitch, in cents, at an arbitrary
        sample. Returns false where the track has nothing voiced to say. */
    bool interpolatedCents (const PitchTrack& track, int64_t sample, float& out) noexcept
    {
        if (track.empty())
            return false;

        const auto hop = (double) track.hopSize;
        const auto approx = (double) (sample - track.frames.front().sampleIndex) / hop;
        int idx = (int) std::floor (approx);
        idx = std::clamp (idx, 0, (int) track.frames.size() - 1);

        // Walk out to the nearest voiced frames on either side.
        int lo = idx;

        while (lo >= 0 && ! track.frames[(size_t) lo].voiced)
            --lo;

        int hi = idx + 1;

        while (hi < (int) track.frames.size() && ! track.frames[(size_t) hi].voiced)
            ++hi;

        const bool haveLo = lo >= 0;
        const bool haveHi = hi < (int) track.frames.size();

        if (! haveLo && ! haveHi)
            return false;

        if (! haveHi)
        {
            out = (float) hzToCents (track.frames[(size_t) lo].f0Hz);
            return true;
        }

        if (! haveLo)
        {
            out = (float) hzToCents (track.frames[(size_t) hi].f0Hz);
            return true;
        }

        const auto& a = track.frames[(size_t) lo];
        const auto& b = track.frames[(size_t) hi];
        const auto span = (double) (b.sampleIndex - a.sampleIndex);
        const auto t = span > 0.0 ? std::clamp ((double) (sample - a.sampleIndex) / span, 0.0, 1.0) : 0.0;

        out = (float) ((1.0 - t) * hzToCents (a.f0Hz) + t * hzToCents (b.f0Hz));
        return true;
    }
}

//==============================================================================
void buildPitchRatioCurve (const PitchTrack& track,
                           const std::vector<Note>& notes,
                           const CorrectionSettings& settings,
                           int64_t first,
                           int64_t count,
                           float* ratioOut)
{
    std::fill (ratioOut, ratioOut + count, 1.0f);

    if (! settings.enabled || notes.empty() || track.empty())
        return;

    const auto ramp = std::max (1.0, settings.transitionSeconds * track.sampleRate);

    // Walk the requested span note by note rather than sample by sample
    // searching the whole list: the renderer calls this on every block.
    int idx = noteAtSample (notes, first);

    if (idx < 0)
    {
        // Start before the first note in range; find the first note that overlaps.
        idx = 0;

        while (idx < (int) notes.size() && notes[(size_t) idx].endSample <= first)
            ++idx;
    }

    for (; idx < (int) notes.size(); ++idx)
    {
        const auto& n = notes[(size_t) idx];

        if (n.startSample >= first + count)
            break;

        if (n.bypassed || n.correctAmount <= 0.0f)
            if (std::abs (n.vibratoScale - 1.0f) < 1.0e-6f)
                continue;

        const auto from = std::max (n.startSample, first);
        const auto to   = std::min (n.endSample, first + count);
        const auto shift = n.bypassed ? 0.0f
                                      : n.correctAmount * (n.targetCents - n.centsDetected);

        for (auto s = from; s < to; ++s)
        {
            float origCents = 0.0f;

            if (! interpolatedCents (track, s, origCents))
                continue;

            // The performance inside the note, relative to where the note sits.
            const auto residual = origCents - n.centsDetected;
            const auto vib = n.bypassed ? residual : n.vibratoScale * residual;

            // Ease the correction in and out so note edges do not click.
            const auto dIn  = (double) (s - n.startSample) / ramp;
            const auto dOut = (double) (n.endSample - 1 - s) / ramp;
            const auto env  = smoothStep (std::min (dIn, dOut));

            const auto newCents = n.centsDetected + (float) env * (shift + vib)
                                                  + (float) (1.0 - env) * residual;
            const auto delta = newCents - origCents;

            ratioOut[s - first] = (float) std::pow (2.0, delta / 1200.0);
        }
    }
}

//==============================================================================
void buildGainCurve (const std::vector<Note>& notes,
                     const CorrectionSettings& settings,
                     int64_t first,
                     int64_t count,
                     float* gainOut)
{
    std::fill (gainOut, gainOut + count, 1.0f);

    if (! settings.enabled)
        return;

    int idx = 0;

    while (idx < (int) notes.size() && notes[(size_t) idx].endSample <= first)
        ++idx;

    for (; idx < (int) notes.size(); ++idx)
    {
        const auto& n = notes[(size_t) idx];

        if (n.startSample >= first + count)
            break;

        if (std::abs (n.gainDb) < 1.0e-4f)
            continue;

        const auto g    = (float) std::pow (10.0, n.gainDb / 20.0);
        const auto from = std::max (n.startSample, first);
        const auto to   = std::min (n.endSample, first + count);
        // 5 ms of ramp at each edge so a lifted note does not click in.
        const auto ramp = std::max<int64_t> (1, n.lengthSamples() / 20);

        for (auto s = from; s < to; ++s)
        {
            const auto d = std::min (s - n.startSample, n.endSample - 1 - s);
            const auto env = smoothStep ((double) d / (double) ramp);
            gainOut[s - first] = 1.0f + (g - 1.0f) * (float) env;
        }
    }
}

} // namespace pb
