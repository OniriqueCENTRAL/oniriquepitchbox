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

struct PitchMarkSet
{
    std::vector<PitchMark> marks;

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
