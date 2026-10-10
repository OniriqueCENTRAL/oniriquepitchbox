#include "EditState.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace pb
{

namespace
{
    /** How much two notes overlap in time, as a fraction of the shorter one. */
    double overlapFraction (const Note& a, const Note& b) noexcept
    {
        const auto from = std::max (a.startSample, b.startSample);
        const auto to   = std::min (a.endSample, b.endSample);

        if (to <= from)
            return 0.0;

        const auto shorter = std::min (a.lengthSamples(), b.lengthSamples());
        return shorter > 0 ? (double) (to - from) / (double) shorter : 0.0;
    }
}

void EditState::adoptAnalysis (const Analysis& analysis)
{
    const auto previousNotes = notes;
    const auto previousTouched = touched;

    notes = analysis.notes;
    touched.assign (notes.size(), false);

    // Carry forward any hand edit whose note still exists. Matching on overlap
    // rather than on index means one extra detected breath at the start of the
    // take does not slide every edit one note to the right.
    for (size_t i = 0; i < previousNotes.size(); ++i)
    {
        if (i >= previousTouched.size() || ! previousTouched[i])
            continue;

        const auto& old = previousNotes[i];

        size_t best = notes.size();
        double bestOverlap = 0.5;   // demand a real match, not a graze

        for (size_t j = 0; j < notes.size(); ++j)
        {
            const auto o = overlapFraction (old, notes[j]);

            if (o > bestOverlap)
            {
                bestOverlap = o;
                best = j;
            }
        }

        if (best < notes.size())
        {
            // The edit is expressed relative to the detected pitch, so that a
            // re-analysis which nudges the detected centre by a few cents moves
            // the correction with it instead of leaving it behind.
            const auto offset = old.targetCents - old.centsDetected;

            notes[best].targetCents   = notes[best].centsDetected + offset;
            notes[best].correctAmount = old.correctAmount;
            notes[best].vibratoScale  = old.vibratoScale;
            notes[best].gainDb        = old.gainDb;
            notes[best].bypassed      = old.bypassed;
            touched[best] = true;
        }
    }

    applyScaleToNotes (notes, settings, touched);
    bump();
}

//==============================================================================
std::string EditState::toString() const
{
    std::ostringstream out;
    out.precision (6);
    out << "pitchbox 1\n";
    out << "scale " << settings.scale.root << ' ' << (int) settings.scale.type << '\n';

    out << "mask";

    for (int i = 0; i < 12; ++i)
        out << ' ' << (settings.scale.customMask[(size_t) i] ? 1 : 0);

    out << '\n';

    out << "global " << settings.globalAmount << ' '
        << settings.transitionSeconds << ' '
        << settings.formantRatio << ' '
        << (settings.enabled ? 1 : 0) << '\n';

    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& n = notes[i];

        // Only hand edits are worth storing: everything else is reproduced by
        // re-running the analysis and the global snap.
        if (i < touched.size() && ! touched[i])
            continue;

        out << "note " << n.startSample << ' ' << n.endSample << ' '
            << n.centsDetected << ' ' << n.targetCents << ' '
            << n.correctAmount << ' ' << n.vibratoScale << ' '
            << n.gainDb << ' ' << (n.bypassed ? 1 : 0) << '\n';
    }

    return out.str();
}

EditState EditState::fromString (const std::string& text)
{
    EditState state;
    std::istringstream in (text);
    std::string line;

    if (! std::getline (in, line) || line.rfind ("pitchbox", 0) != 0)
        return state;

    while (std::getline (in, line))
    {
        std::istringstream ls (line);
        std::string tag;
        ls >> tag;

        if (tag == "scale")
        {
            int root = 0, type = 0;
            ls >> root >> type;
            state.settings.scale.root = std::clamp (root, 0, 11);

            if (type >= 0 && type < (int) ScaleType::numTypes)
                state.settings.scale.type = (ScaleType) type;
        }
        else if (tag == "mask")
        {
            for (int i = 0; i < 12; ++i)
            {
                int v = 1;
                ls >> v;
                state.settings.scale.customMask[(size_t) i] = (v != 0);
            }
        }
        else if (tag == "global")
        {
            int enabled = 1;
            ls >> state.settings.globalAmount
               >> state.settings.transitionSeconds
               >> state.settings.formantRatio
               >> enabled;
            state.settings.enabled = (enabled != 0);
        }
        else if (tag == "note")
        {
            Note n;
            int bypassed = 0;
            ls >> n.startSample >> n.endSample >> n.centsDetected >> n.targetCents
               >> n.correctAmount >> n.vibratoScale >> n.gainDb >> bypassed;
            n.bypassed = (bypassed != 0);
            state.notes.push_back (n);
            state.touched.push_back (true);
        }
    }

    state.bump();
    return state;
}

} // namespace pb
