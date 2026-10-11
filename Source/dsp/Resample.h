// Onirique PitchBox — reading a signal at another rate
#pragma once

#include <cmath>
#include <cstdint>

namespace pb
{

/** Reads `count` samples out of `src` at evenly spaced fractional positions,
    starting at `firstPosition` and `step` apart, both in samples of `src`.

    This is how a take recorded at one rate is played in a session running at
    another: the take is rendered at its own rate, where its analysis lives,
    and read out at the session's. Nothing is carried from one call to the
    next. Each output sample depends only on where it falls in the source, so
    a host that asks for the audio in blocks of any size gets exactly what it
    would have got in one.

    A cubic through the four samples around each position. `src` must hold one
    sample before the first position and two after the last.
*/
inline void readAtRate (const float* src, double firstPosition, double step, float* dst, int count) noexcept
{
    for (int i = 0; i < count; ++i)
    {
        const auto position = firstPosition + (double) i * step;
        const auto whole = (int64_t) std::floor (position);
        const auto f = (float) (position - (double) whole);

        const auto p0 = src[whole - 1], p1 = src[whole], p2 = src[whole + 1], p3 = src[whole + 2];

        dst[i] = p1 + 0.5f * f * (p2 - p0
                                  + f * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3
                                         + f * (3.0f * (p1 - p2) + p3 - p0)));
    }
}

} // namespace pb
