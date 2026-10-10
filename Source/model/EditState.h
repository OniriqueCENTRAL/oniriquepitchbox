// Onirique PitchBox — what the user changed, kept apart from what was sung
#pragma once

#include "Analysis.h"
#include "Notes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pb
{

/** The editable half of a take.

    This is deliberately separate from Analysis. The analysis says what the
    singer did; this says what the user wants instead. Keeping them apart buys
    two things: an edit never triggers a re-analysis, which is why a change is
    audible on the next buffer; and a re-analysis (new algorithm, changed
    settings) can be folded in without throwing the user's work away.

    `version` ticks on every change. The renderer uses it to know when its
    cached correction curve is stale, so it never rebuilds the curve on a block
    where nothing moved.
*/
struct EditState
{
    std::vector<Note>  notes;
    CorrectionSettings settings;

    /** Per-note flag: has the user moved this note by hand? Untouched notes
        follow the global scale snap; touched ones keep what the user set. */
    std::vector<bool> touched;

    uint64_t version = 1;

    void bump() noexcept { ++version; }

    bool empty() const noexcept { return notes.empty(); }

    /** Seeds the edit state from a fresh analysis, keeping any edits whose note
        still lines up with a detected note. Notes are matched by time overlap
        rather than by index, so inserting or losing one note at the top of a
        take does not shift every later edit onto the wrong syllable. */
    void adoptAnalysis (const Analysis& analysis);

    /** Re-applies the global scale snap to every untouched note. */
    void refreshUntouched()
    {
        applyScaleToNotes (notes, settings, touched);
        bump();
    }

    void markTouched (int index)
    {
        if (index >= 0 && index < (int) touched.size())
            touched[(size_t) index] = true;

        bump();
    }

    //==============================================================================
    /** Serialised form for the host session. Plain text, versioned, and it
        stores each note's detected pitch alongside the edit so a reload can
        tell whether it is still describing the same note. */
    std::string toString() const;
    static EditState fromString (const std::string& text);
};

} // namespace pb
