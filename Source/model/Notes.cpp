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

namespace
{
    /** Notes closer together than this are one phrase: the correction glides
        from one to the next across the gap instead of letting go in between. */
    constexpr double kLegatoGapSeconds = 0.1;

    /** How far a note is moved as a whole, in cents. */
    inline float shiftOf (const Note& n) noexcept
    {
        return n.bypassed ? 0.0f : n.correctAmount * (n.targetCents - n.centsDetected);
    }

    /** The correction, in cents, at one sample. The curve and the single-sample
        query both come through here, so they cannot disagree.

        It has two parts. One is how far the note as a whole is moved, and that
        part never lets go at a note boundary: it glides from one note's shift to
        the next one's across the boundary, over the transition time. Letting it
        fall back to nothing at every edge, as it once did, hands the voice its
        own pitch back for a moment between any two notes, several times a
        second in a legato line, and that is heard as a judder. Only at the edge
        of a phrase, where no note follows, is there nothing to glide to; and
        there the shift is simply held, because the voice is stopping anyway.

        The other part reshapes the movement inside the note, the vibrato, and
        is eased in and out at the note's own edges.
    */
    bool correctionCentsAt (const PitchTrack& track,
                            const std::vector<Note>& notes,
                            double ramp,
                            int64_t s,
                            float& cents) noexcept
    {
        const auto legatoGap = (int64_t) (kLegatoGapSeconds * track.sampleRate);

        auto middle = [] (const Note& n) { return (double) (n.startSample + n.endSample) * 0.5; };

        // The shift half way across the join between two neighbouring notes.
        auto glide = [&] (const Note& from, const Note& to, float& shift)
        {
            if (to.startSample - from.endSample > legatoGap)
                return false;

            const auto begins = std::max ((double) from.endSample - ramp, middle (from));
            const auto ends   = std::min ((double) to.startSample + ramp, middle (to));

            if ((double) s < begins || (double) s >= ends || ends <= begins)
                return false;

            const auto u = (float) smoothStep (((double) s - begins) / (ends - begins));
            shift = shiftOf (from) + u * (shiftOf (to) - shiftOf (from));
            return true;
        };

        const auto count = (int) notes.size();
        const auto idx = noteAtSample (notes, s);

        if (idx < 0)
        {
            // Between two notes: all there is here is the glide, if they are
            // close enough to have one.
            int next = 0;

            while (next < count && notes[(size_t) next].startSample <= s)
                ++next;

            float shift = 0.0f;

            if (next <= 0 || next >= count || ! glide (notes[(size_t) next - 1], notes[(size_t) next], shift))
                return false;

            cents = shift;
            return true;
        }

        const auto& n = notes[(size_t) idx];
        auto shift = shiftOf (n);

        if (idx > 0)
            glide (notes[(size_t) idx - 1], n, shift);

        if (idx + 1 < count)
            glide (n, notes[(size_t) idx + 1], shift);

        float movement = 0.0f;

        if (! n.bypassed && std::abs (n.vibratoScale - 1.0f) >= 1.0e-6f)
        {
            float origCents = 0.0f;

            if (interpolatedCents (track, s, origCents))
            {
                // The performance inside the note, relative to where the note sits.
                const auto residual = origCents - n.centsDetected;

                const auto dIn  = (double) (s - n.startSample) / ramp;
                const auto dOut = (double) (n.endSample - 1 - s) / ramp;
                const auto env  = (float) smoothStep (std::min (dIn, dOut));

                movement = env * (n.vibratoScale - 1.0f) * residual;
            }
        }

        cents = shift + movement;
        return true;
    }

    inline float ratioFromCents (float cents) noexcept
    {
        return std::abs (cents) < 1.0e-4f ? 1.0f : (float) std::pow (2.0, (double) cents / 1200.0);
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
    for (int64_t i = 0; i < count; ++i)
        ratioOut[i] = pitchRatioAt (track, notes, settings, first + i);
}

float pitchRatioAt (const PitchTrack& track,
                    const std::vector<Note>& notes,
                    const CorrectionSettings& settings,
                    int64_t sample) noexcept
{
    if (! settings.enabled || notes.empty() || track.empty())
        return 1.0f;

    float cents = 0.0f;

    if (! correctionCentsAt (track, notes, std::max (1.0, settings.transitionSeconds * track.sampleRate), sample, cents))
        return 1.0f;

    return ratioFromCents (cents);
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
