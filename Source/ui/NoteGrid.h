// Onirique PitchBox — the note editor
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../model/Analysis.h"
#include "../model/EditState.h"
#include "Theme.h"

namespace onirique
{

using namespace juce;

/** Draws the take as notes over a pitch grid, and lets the user move them.

    The sung pitch curve is drawn underneath the notes rather than replaced by
    them: the whole difficulty of tuning a vocal is seeing where a note actually
    sits against where it is being pulled, and a row of rectangles alone hides
    exactly that.
*/
class NoteGrid final : public Component
{
public:
    NoteGrid();

    /** Callback fired whenever the user changes something. The editor pushes
        the change into the ARA audio modification, which notifies the host,
        which is what makes the edit audible on the next buffer. */
    std::function<void()> onEdit;

    void setAnalysis (pb::Analysis::Ptr);
    void setEditState (pb::EditState* state) { edits = state; repaint(); }

    void paint (Graphics&) override;
    void resized() override;

    void mouseDown (const MouseEvent&) override;
    void mouseDrag (const MouseEvent&) override;
    void mouseUp (const MouseEvent&) override;
    void mouseDoubleClick (const MouseEvent&) override;
    void mouseWheelMove (const MouseEvent&, const MouseWheelDetails&) override;
    void mouseMove (const MouseEvent&) override;

    void setStatus (String newStatus, float newProgress);
    void zoomToFit();

private:
    int noteAt (Point<float>) const;
    float timeToX (int64_t sample) const;
    int64_t xToTime (float x) const;
    float centsToY (float cents) const;
    float yToCents (float y) const;

    void drawGrid (Graphics&);
    void drawPitchCurve (Graphics&);
    void drawNotes (Graphics&);

    pb::Analysis::Ptr analysis;
    pb::EditState* edits = nullptr;

    // Visible window, in samples and in cents.
    int64_t viewStart = 0, viewLength = 1;
    float viewLowCents = 4000.0f, viewHighCents = 8400.0f;

    int   draggingNote = -1;
    int   hoveredNote = -1;
    float dragStartCents = 0.0f;
    float dragStartTarget = 0.0f;
    bool  dragWasTouched = false;

    String status;
    float progress = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteGrid)
};

} // namespace onirique
