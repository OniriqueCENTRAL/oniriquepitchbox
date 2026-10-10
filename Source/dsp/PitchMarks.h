// Onirique PitchBox — period marks for time-domain PSOLA
#pragma once

#include "PitchTrack.h"

#include <cstdint>
#include <vector>

namespace pb
{

/** One analysis grain anchor. */
struct PitchMark
{
    int64_t position = 0;     // sample index of the anchor
    float   period   = 0.0f;  // local period in samples
    bool    voiced   = false;
};

/** A stretch where the voice has stopped: [begin, end) in samples, from the
    first unvoiced mark to the mark where the voice comes back in. */
struct Pause
{
    int64_t begin = 0;
    int64_t end   = 0;
};

struct PitchMarkSet
{
    std::vector<PitchMark> marks;

    /** The pauses, in order. Only real ones: a gap in the voicing too short to
        be anything but a flicker of the detector is not listed. The stretch
        before the first voiced mark and the one after the last are pauses
        whatever their length, and reach to the ends of the take and beyond.

        Inside a pause the renderer does not resynthesise anything; it hands
        the recording through as it is. There is no pitch there to correct, and
        laying a breath down again out of grains only moves and doubles the
        small sounds in it. */
    std::vector<Pause> pauses;

    /** How long the hand-over between the recording and the resynthesis takes
        at each end of a pause, in samples. */
    int64_t pauseEdge = 0;

    /** The widest gap between two marks, or the longest period if that is
        wider. The renderer needs it on every block, so it is worked out once
        here; 0 means "not worked out", and it is then measured on demand. */
    double widestSpacing = 0.0;

    /** Index of the mark whose position is closest to the given sample.
        Returns -1 when empty. Marks are sorted, so this is a binary search. */
    int nearest (double samplePosition) const noexcept;
};

/** Per-sample period estimate, interpolated from the voiced frames of the track.
    Unvoiced stretches hold the nearest voiced period so grains stay plausible. */
std::vector<float> buildPeriodCurve (const PitchTrack& track, int64_t numSamples);

/** Places grain anchors on energy peaks, stepping forward by the local period.

    The signal is low-passed to isolate the first few harmonics so the peaks
    line up with the glottal pulses rather than with formant ringing.
*/
PitchMarkSet detectPitchMarks (const float* samples,
                               int64_t numSamples,
                               const PitchTrack& track,
                               const std::vector<float>& periodCurve);

} // namespace pb
