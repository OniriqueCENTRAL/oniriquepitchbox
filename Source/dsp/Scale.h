// Onirique PitchBox — key / scale handling (the Scale Assistant side)
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pb
{

enum class ScaleType
{
    chromatic = 0,
    major,
    naturalMinor,
    harmonicMinor,
    melodicMinor,
    dorian,
    phrygian,
    lydian,
    mixolydian,
    locrian,
    majorPentatonic,
    minorPentatonic,
    blues,
    custom,
    numTypes
};

const char* scaleTypeName (ScaleType) noexcept;

/** A pitch-class set plus a root. Degrees are semitones above the root. */
struct Scale
{
    int       root = 0;                       // 0 = C, 11 = B
    ScaleType type = ScaleType::chromatic;
    std::array<bool, 12> customMask { { true, true, true, true, true, true,
                                        true, true, true, true, true, true } };

    /** Pitch classes allowed by this scale, as absolute classes 0..11. */
    std::array<bool, 12> pitchClasses() const noexcept;

    bool allows (int midiNote) const noexcept;

    /** Nearest allowed pitch in cents (MIDI * 100). Ties resolve upwards. */
    float snapCents (float cents) const noexcept;

    /** Nearest allowed MIDI note number. */
    int snapMidi (float cents) const noexcept;

    std::string describe() const;
};

/** Scores every root/type pair against a weighted histogram of pitch classes
    and returns the best fit — the "detect key from the audio" button. */
struct ScaleEstimate
{
    Scale scale;
    float confidence = 0.0f; // 0..1, gap between the best and second-best fit
};

ScaleEstimate estimateScale (const std::array<float, 12>& pitchClassWeights);

const char* noteName (int pitchClass) noexcept;

} // namespace pb
