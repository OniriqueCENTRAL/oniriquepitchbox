// Onirique PitchBox — pitch analysis data types
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace pb
{

/** One analysis frame of the fundamental-frequency track. */
struct PitchFrame
{
    int64_t sampleIndex = 0;   // centre of the analysis window
    float   f0Hz        = 0.0f; // 0 when unvoiced
    float   confidence  = 0.0f; // 1 - normalised YIN difference, 0..1
    float   rms         = 0.0f;
    bool    voiced      = false;
};

struct PitchTrack
{
    double                  sampleRate = 48000.0;
    int                     hopSize    = 256;
    std::vector<PitchFrame> frames;

    bool  empty() const noexcept { return frames.empty(); }
    size_t size() const noexcept { return frames.size(); }
};

//==============================================================================
// Pitch / cents helpers. MIDI note 69 == A440.

inline double hzToMidi (double hz) noexcept
{
    return 69.0 + 12.0 * std::log2 (hz / 440.0);
}

inline double midiToHz (double midi) noexcept
{
    return 440.0 * std::pow (2.0, (midi - 69.0) / 12.0);
}

inline double hzToCents (double hz) noexcept
{
    return 1200.0 * std::log2 (hz / 440.0) + 6900.0; // cents above MIDI 0
}

inline double centsToHz (double cents) noexcept
{
    return 440.0 * std::pow (2.0, (cents - 6900.0) / 1200.0);
}

} // namespace pb
