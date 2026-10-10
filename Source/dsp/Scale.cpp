#include "Scale.h"

#include <algorithm>
#include <cmath>

namespace pb
{

namespace
{
    // Semitone offsets from the root, one entry per ScaleType.
    const std::vector<int>& degreesFor (ScaleType t)
    {
        static const std::vector<int> chromatic      { 0,1,2,3,4,5,6,7,8,9,10,11 };
        static const std::vector<int> major          { 0,2,4,5,7,9,11 };
        static const std::vector<int> naturalMinor   { 0,2,3,5,7,8,10 };
        static const std::vector<int> harmonicMinor  { 0,2,3,5,7,8,11 };
        static const std::vector<int> melodicMinor   { 0,2,3,5,7,9,11 };
        static const std::vector<int> dorian         { 0,2,3,5,7,9,10 };
        static const std::vector<int> phrygian       { 0,1,3,5,7,8,10 };
        static const std::vector<int> lydian         { 0,2,4,6,7,9,11 };
        static const std::vector<int> mixolydian     { 0,2,4,5,7,9,10 };
        static const std::vector<int> locrian        { 0,1,3,5,6,8,10 };
        static const std::vector<int> majorPent      { 0,2,4,7,9 };
        static const std::vector<int> minorPent      { 0,3,5,7,10 };
        static const std::vector<int> blues          { 0,3,5,6,7,10 };

        switch (t)
        {
            case ScaleType::major:           return major;
            case ScaleType::naturalMinor:    return naturalMinor;
            case ScaleType::harmonicMinor:   return harmonicMinor;
            case ScaleType::melodicMinor:    return melodicMinor;
            case ScaleType::dorian:          return dorian;
            case ScaleType::phrygian:        return phrygian;
            case ScaleType::lydian:          return lydian;
            case ScaleType::mixolydian:      return mixolydian;
            case ScaleType::locrian:         return locrian;
            case ScaleType::majorPentatonic: return majorPent;
            case ScaleType::minorPentatonic: return minorPent;
            case ScaleType::blues:           return blues;
            case ScaleType::chromatic:
            case ScaleType::custom:
            case ScaleType::numTypes:
            default:                         return chromatic;
        }
    }

    const char* const kNoteNames[12] =
        { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    /** Krumhansl-Kessler key profiles: how strongly each scale degree is
        expected to appear in a piece in that key. These come from listener
        experiments, and they beat a plain "is the note in the scale?" count
        because they know the tonic and the fifth carry the key. */
    const double kMajorProfile[12] =
        { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    const double kMinorProfile[12] =
        { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

    double correlate (const std::array<float, 12>& weights, const double* profile, int root)
    {
        double meanW = 0.0, meanP = 0.0;

        for (int i = 0; i < 12; ++i)
        {
            meanW += weights[(size_t) i];
            meanP += profile[i];
        }

        meanW /= 12.0;
        meanP /= 12.0;

        double num = 0.0, dw = 0.0, dp = 0.0;

        for (int i = 0; i < 12; ++i)
        {
            const double w = weights[(size_t) (((i + root) % 12 + 12) % 12)] - meanW;
            const double p = profile[i] - meanP;
            num += w * p;
            dw  += w * w;
            dp  += p * p;
        }

        const auto den = std::sqrt (dw * dp);
        return den > 1.0e-12 ? num / den : 0.0;
    }
}

//==============================================================================
const char* scaleTypeName (ScaleType t) noexcept
{
    switch (t)
    {
        case ScaleType::chromatic:       return "Chromatic";
        case ScaleType::major:           return "Major";
        case ScaleType::naturalMinor:    return "Minor";
        case ScaleType::harmonicMinor:   return "Harmonic minor";
        case ScaleType::melodicMinor:    return "Melodic minor";
        case ScaleType::dorian:          return "Dorian";
        case ScaleType::phrygian:        return "Phrygian";
        case ScaleType::lydian:          return "Lydian";
        case ScaleType::mixolydian:      return "Mixolydian";
        case ScaleType::locrian:         return "Locrian";
        case ScaleType::majorPentatonic: return "Major pentatonic";
        case ScaleType::minorPentatonic: return "Minor pentatonic";
        case ScaleType::blues:           return "Blues";
        case ScaleType::custom:          return "Custom";
        case ScaleType::numTypes:
        default:                         return "?";
    }
}

const char* noteName (int pitchClass) noexcept
{
    return kNoteNames[(size_t) (((pitchClass % 12) + 12) % 12)];
}

//==============================================================================
std::array<bool, 12> Scale::pitchClasses() const noexcept
{
    std::array<bool, 12> out {};
    out.fill (false);

    if (type == ScaleType::custom)
    {
        for (int i = 0; i < 12; ++i)
            out[(size_t) ((((i + root) % 12) + 12) % 12)] = customMask[(size_t) i];

        // Never leave the mask empty: that would make snapping undefined.
        if (std::none_of (out.begin(), out.end(), [] (bool b) { return b; }))
            out.fill (true);

        return out;
    }

    for (int d : degreesFor (type))
        out[(size_t) ((((d + root) % 12) + 12) % 12)] = true;

    return out;
}

bool Scale::allows (int midiNote) const noexcept
{
    return pitchClasses()[(size_t) (((midiNote % 12) + 12) % 12)];
}

int Scale::snapMidi (float cents) const noexcept
{
    const auto classes = pitchClasses();
    const auto exact   = cents / 100.0f;
    const int  nearest = (int) std::lround (exact);

    // Search outwards from the nearest chromatic note. Twelve steps is always
    // enough: every scale we offer has at least one note per octave.
    for (int d = 0; d <= 12; ++d)
    {
        // Ties resolve upwards, so a note dead between two scale tones rises.
        const int up   = nearest + d;
        const int down = nearest - d;

        if (classes[(size_t) (((up % 12) + 12) % 12)])
            if (d == 0 || std::abs ((float) up - exact) <= std::abs ((float) down - exact))
                return up;

        if (classes[(size_t) (((down % 12) + 12) % 12)])
            return down;

        if (classes[(size_t) (((up % 12) + 12) % 12)])
            return up;
    }

    return nearest;
}

float Scale::snapCents (float cents) const noexcept
{
    return (float) snapMidi (cents) * 100.0f;
}

std::string Scale::describe() const
{
    return std::string (noteName (root)) + " " + scaleTypeName (type);
}

//==============================================================================
ScaleEstimate estimateScale (const std::array<float, 12>& pitchClassWeights)
{
    struct Candidate { double score; Scale scale; };
    std::vector<Candidate> scored;
    scored.reserve (24);

    for (int root = 0; root < 12; ++root)
    {
        Scale maj; maj.root = root; maj.type = ScaleType::major;
        Scale min; min.root = root; min.type = ScaleType::naturalMinor;

        scored.push_back ({ correlate (pitchClassWeights, kMajorProfile, root), maj });
        scored.push_back ({ correlate (pitchClassWeights, kMinorProfile, root), min });
    }

    std::sort (scored.begin(), scored.end(),
               [] (const Candidate& a, const Candidate& b) { return a.score > b.score; });

    ScaleEstimate est;
    est.scale = scored.front().scale;
    // Confidence is the margin over the runner-up, not the absolute score:
    // a phrase that fits two keys equally well should report low confidence
    // even when it fits both of them very well.
    est.confidence = (float) std::clamp (scored[0].score - scored[1].score, 0.0, 1.0);
    return est;
}

} // namespace pb
