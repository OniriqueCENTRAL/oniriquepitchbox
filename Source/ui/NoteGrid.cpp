#include "NoteGrid.h"

namespace onirique
{

using namespace theme;

namespace
{
    constexpr float kRulerWidth = 46.0f;
    constexpr float kMinSemitonesVisible = 6.0f;
    constexpr float kMaxSemitonesVisible = 60.0f;

    // The whole keyboard, C-1 to C9: as far as the view can be scrolled.
    constexpr float kLowestCents = 0.0f;
    constexpr float kHighestCents = 12000.0f;

    /** Fraction of the view one notch of the wheel moves it by. */
    constexpr float kScrollPerNotch = 0.35f;
}

NoteGrid::NoteGrid()
{
    setWantsKeyboardFocus (false);
    setMouseCursor (MouseCursor::NormalCursor);
}

void NoteGrid::setAnalysis (pb::Analysis::Ptr newAnalysis)
{
    analysis = std::move (newAnalysis);

    // A different take: the indices no longer mean the same notes.
    selected.clear();
    selectionChanged();

    zoomToFit();
    repaint();
}

void NoteGrid::setStatus (String newStatus, float newProgress)
{
    status = std::move (newStatus);
    progress = newProgress;
    repaint();
}

void NoteGrid::zoomToFit()
{
    if (analysis == nullptr || analysis->notes.empty())
    {
        viewStart = 0;
        viewLength = jmax ((int64_t) 1, analysis != nullptr ? analysis->numSamples : (int64_t) 1);
        return;
    }

    viewStart = 0;
    viewLength = jmax ((int64_t) 1, analysis->numSamples);

    // Fit the sung range with a couple of semitones of air above and below, so
    // a note dragged to the edge still has somewhere to go.
    float lo = 1.0e9f, hi = -1.0e9f;

    for (const auto& n : analysis->notes)
    {
        lo = jmin (lo, n.centsDetected);
        hi = jmax (hi, n.centsDetected);
    }

    if (hi < lo)
    {
        lo = 4800.0f;
        hi = 7200.0f;
    }

    const auto centre = (lo + hi) * 0.5f;
    const auto span = jmax (hi - lo + 600.0f, kMinSemitonesVisible * 100.0f);

    viewLowCents  = centre - span * 0.5f;
    viewHighCents = centre + span * 0.5f;
}

//==============================================================================
float NoteGrid::timeToX (int64_t sample) const
{
    const auto w = (float) getWidth() - kRulerWidth;
    return kRulerWidth + w * (float) ((double) (sample - viewStart) / (double) viewLength);
}

int64_t NoteGrid::xToTime (float x) const
{
    const auto w = jmax (1.0f, (float) getWidth() - kRulerWidth);
    return viewStart + (int64_t) ((double) ((x - kRulerWidth) / w) * (double) viewLength);
}

float NoteGrid::centsToY (float cents) const
{
    const auto h = (float) getHeight();
    return h * (viewHighCents - cents) / jmax (1.0f, viewHighCents - viewLowCents);
}

float NoteGrid::yToCents (float y) const
{
    const auto h = jmax (1.0f, (float) getHeight());
    return viewHighCents - (y / h) * (viewHighCents - viewLowCents);
}

//==============================================================================
void NoteGrid::paint (Graphics& g)
{
    g.fillAll (night);

    drawGrid (g);

    if (analysis == nullptr)
    {
        g.setColour (haze);
        g.setFont (monoFont (12.0f));
        g.drawText (status.isNotEmpty() ? status : "No take loaded",
                    getLocalBounds(), Justification::centred, false);
        return;
    }

    drawPitchCurve (g);
    drawNotes (g);

    if (lassoing)
    {
        const Rectangle<float> box (lassoStart, lassoEnd);

        g.setColour (ember.withAlpha (0.12f));
        g.fillRect (box);
        g.setColour (ember.withAlpha (0.8f));
        g.drawRect (box, 1.0f);
    }

    if (progress > 0.0f && progress < 1.0f)
    {
        // Analysing. Say so over the top rather than showing an empty grid.
        auto bar = getLocalBounds().toFloat().removeFromBottom (26.0f).reduced (12.0f, 7.0f);

        g.setColour (Colour (0xaa0d0809));
        g.fillRoundedRectangle (bar.expanded (6.0f, 5.0f), 6.0f);

        g.setColour (line);
        g.drawRoundedRectangle (bar, 4.0f, 1.0f);

        g.setColour (ember);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * progress), 4.0f);

        g.setColour (mist);
        g.setFont (monoFont (10.0f));
        g.drawText (status, bar, Justification::centred, false);
    }
}

void NoteGrid::drawGrid (Graphics& g)
{
    const auto h = (float) getHeight();
    const auto w = (float) getWidth();

    const auto lowMidi  = (int) std::floor (viewLowCents / 100.0f);
    const auto highMidi = (int) std::ceil (viewHighCents / 100.0f);

    std::array<bool, 12> allowed {};
    allowed.fill (true);

    if (edits != nullptr)
        allowed = edits->settings.scale.pitchClasses();

    const auto rowHeight = h / jmax (1.0f, (viewHighCents - viewLowCents) / 100.0f);
    const auto labelled = rowHeight > 11.0f;

    for (int midi = lowMidi; midi <= highMidi; ++midi)
    {
        const auto y = centsToY ((float) midi * 100.0f);
        const auto pc = ((midi % 12) + 12) % 12;
        const auto inScale = allowed[(size_t) pc];

        // Rows that belong to the scale are lifted out of the background, so
        // where a note is allowed to land is readable at a glance.
        if (inScale)
        {
            g.setColour (Colour (0x14ffd2d6));
            g.fillRect (kRulerWidth, y - rowHeight * 0.5f, w - kRulerWidth, rowHeight);
        }

        g.setColour (pc == 0 ? Colour (0x3affd2d6) : Colour (0x12ffd2d6));
        g.drawHorizontalLine (roundToInt (y - rowHeight * 0.5f), kRulerWidth, w);

        if (labelled)
        {
            g.setColour (inScale ? haze : Colour (0x55a88f92));
            g.setFont (monoFont (jmin (10.0f, rowHeight * 0.62f)));
            g.drawText (String (pb::noteName (pc)) + String (midi / 12 - 1),
                        Rectangle<float> (2.0f, y - rowHeight * 0.5f, kRulerWidth - 6.0f, rowHeight),
                        Justification::centredRight, false);
        }
    }

    g.setColour (line);
    g.drawVerticalLine (roundToInt (kRulerWidth), 0.0f, h);
}

void NoteGrid::drawPitchCurve (Graphics& g)
{
    const auto& track = analysis->track;

    if (track.empty())
        return;

    Path sung, corrected;
    bool sungOpen = false, correctedOpen = false;

    const auto haveEdits = edits != nullptr && ! edits->notes.empty();

    for (const auto& frame : track.frames)
    {
        if (! frame.voiced || frame.f0Hz <= 0.0f)
        {
            sungOpen = false;
            correctedOpen = false;
            continue;
        }

        if (frame.sampleIndex < viewStart || frame.sampleIndex > viewStart + viewLength)
            continue;

        const auto cents = (float) pb::hzToCents (frame.f0Hz);
        const auto x = timeToX (frame.sampleIndex);
        const auto y = centsToY (cents);

        if (! sungOpen) { sung.startNewSubPath (x, y); sungOpen = true; }
        else            { sung.lineTo (x, y); }

        if (! haveEdits)
            continue;

        // Where that frame will end up once the correction is applied. Drawing
        // both curves is what lets someone judge a setting without listening
        // to the whole phrase first.
        const auto idx = pb::noteAtSample (edits->notes, frame.sampleIndex);

        if (idx < 0)
        {
            correctedOpen = false;
            continue;
        }

        const auto& n = edits->notes[(size_t) idx];
        const auto residual = cents - n.centsDetected;
        const auto shift = n.bypassed ? 0.0f : n.correctAmount * (n.targetCents - n.centsDetected);
        const auto vib = n.bypassed ? residual : n.vibratoScale * residual;
        const auto outCents = n.centsDetected + shift + vib;
        const auto cy = centsToY (outCents);

        if (! correctedOpen) { corrected.startNewSubPath (x, cy); correctedOpen = true; }
        else                 { corrected.lineTo (x, cy); }
    }

    g.setColour (Colour (0x66a88f92));
    g.strokePath (sung, PathStrokeType (1.2f));

    g.setColour (emberLit.withAlpha (0.95f));
    g.strokePath (corrected, PathStrokeType (1.8f, PathStrokeType::curved, PathStrokeType::rounded));
}

void NoteGrid::drawNotes (Graphics& g)
{
    if (edits == nullptr)
        return;

    const auto rowHeight = (float) getHeight() / jmax (1.0f, (viewHighCents - viewLowCents) / 100.0f);
    const auto noteHeight = jlimit (5.0f, 26.0f, rowHeight * 0.74f);

    for (size_t i = 0; i < edits->notes.size(); ++i)
    {
        const auto& n = edits->notes[i];

        const auto x0 = timeToX (n.startSample);
        const auto x1 = timeToX (n.endSample);

        if (x1 < kRulerWidth || x0 > (float) getWidth())
            continue;

        const auto resting = centsToY (n.centsDetected);
        const auto landing = centsToY (n.resultCents());

        const Rectangle<float> blob (x0, landing - noteHeight * 0.5f,
                                     jmax (3.0f, x1 - x0), noteHeight);

        // A ghost where the note was actually sung, when correction moved it.
        if (std::abs (landing - resting) > 1.0f)
        {
            g.setColour (Colour (0x33a88f92));
            g.drawRoundedRectangle ({ x0, resting - noteHeight * 0.5f,
                                      jmax (3.0f, x1 - x0), noteHeight }, 3.0f, 1.0f);

            g.setColour (Colour (0x44ffd2d6));
            g.drawLine (x0 + 2.0f, resting, x0 + 2.0f, landing, 1.0f);
        }

        const auto touched = i < edits->touched.size() && edits->touched[i];
        const auto picked = isSelected (i);
        const auto hovered = picked || (int) i == hoveredNote || (int) i == draggingNote;

        if (n.bypassed)
        {
            g.setColour (Colour (0x33a88f92));
            g.fillRoundedRectangle (blob, 3.0f);
            g.setColour (haze);
            g.drawRoundedRectangle (blob, 3.0f, 1.0f);
        }
        else
        {
            g.setGradientFill (ColourGradient (ember.withAlpha (hovered ? 0.85f : 0.6f),
                                               blob.getX(), blob.getY(),
                                               blood.withAlpha (hovered ? 0.9f : 0.7f),
                                               blob.getX(), blob.getBottom(), false));
            g.fillRoundedRectangle (blob, 3.0f);

            g.setColour (touched ? bone : ember.withAlpha (0.85f));
            g.drawRoundedRectangle (blob, 3.0f, touched ? 1.6f : 1.0f);
        }

        if (picked)
        {
            g.setColour (Colours::white);
            g.drawRoundedRectangle (blob.expanded (2.0f), 4.5f, 1.6f);
        }

        // How far off the note was, shown only when there is room for it.
        if (blob.getWidth() > 44.0f && noteHeight > 11.0f)
        {
            const auto off = n.centsDetected - (edits->settings.scale.snapCents (n.centsDetected));

            g.setColour (bone.withAlpha (0.8f));
            g.setFont (monoFont (jmin (10.0f, noteHeight * 0.6f)));
            g.drawText (String (off >= 0.0f ? "+" : "") + String (roundToInt (off)),
                        blob.reduced (5.0f, 0.0f), Justification::centredLeft, false);
        }
    }
}

//==============================================================================
int NoteGrid::noteAt (Point<float> p) const
{
    if (edits == nullptr)
        return -1;

    for (size_t i = 0; i < edits->notes.size(); ++i)
        if (blobFor (i).expanded (0.0f, 3.0f).contains (p))
            return (int) i;

    return -1;
}

Rectangle<float> NoteGrid::blobFor (size_t index) const
{
    const auto rowHeight = (float) getHeight() / jmax (1.0f, (viewHighCents - viewLowCents) / 100.0f);
    const auto noteHeight = jlimit (5.0f, 26.0f, rowHeight * 0.74f);
    const auto& n = edits->notes[index];

    return { timeToX (n.startSample),
             centsToY (n.resultCents()) - noteHeight * 0.5f,
             jmax (3.0f, timeToX (n.endSample) - timeToX (n.startSample)),
             noteHeight };
}

//==============================================================================
bool NoteGrid::isSelected (size_t index) const
{
    return index < selected.size() && selected[index] != 0;
}

void NoteGrid::fitSelectionToNotes()
{
    selected.resize (edits != nullptr ? edits->notes.size() : (size_t) 0, 0);
}

void NoteGrid::selectionChanged()
{
    if (onSelectionChanged)
        onSelectionChanged();
}

std::vector<int> NoteGrid::getSelection() const
{
    std::vector<int> result;

    if (edits == nullptr)
        return result;

    for (size_t i = 0; i < edits->notes.size(); ++i)
        if (isSelected (i))
            result.push_back ((int) i);

    return result;
}

void NoteGrid::mouseMove (const MouseEvent& e)
{
    const auto was = hoveredNote;
    hoveredNote = noteAt (e.position);

    if (was != hoveredNote)
    {
        setMouseCursor (hoveredNote >= 0 ? MouseCursor::UpDownResizeCursor
                                         : MouseCursor::NormalCursor);
        repaint();
    }
}

void NoteGrid::mouseDown (const MouseEvent& e)
{
    draggingNote = -1;
    lassoing = false;

    if (edits == nullptr)
        return;

    fitSelectionToNotes();

    const auto hit = noteAt (e.position);
    const auto adding = e.mods.isCtrlDown() || e.mods.isShiftDown() || e.mods.isCommandDown();

    if (hit < 0)
    {
        // Empty grid: start a box, the way a desktop does.
        if (! adding)
            std::fill (selected.begin(), selected.end(), (char) 0);

        lassoing = true;
        lassoStart = lassoEnd = e.position;
        lassoBase = selected;

        selectionChanged();
        repaint();
        return;
    }

    if (adding)
    {
        selected[(size_t) hit] = selected[(size_t) hit] != 0 ? (char) 0 : (char) 1;
        selectionChanged();
        repaint();

        if (selected[(size_t) hit] == 0)
            return;
    }
    else if (selected[(size_t) hit] == 0)
    {
        std::fill (selected.begin(), selected.end(), (char) 0);
        selected[(size_t) hit] = 1;
        selectionChanged();
    }

    draggingNote = hit;
    dragChanged = false;
    dragStartCents = yToCents (e.position.y);

    dragStartTargets.resize (edits->notes.size());
    dragWasTouched.resize (edits->notes.size());

    for (size_t i = 0; i < edits->notes.size(); ++i)
    {
        dragStartTargets[i] = edits->notes[i].targetCents;
        dragWasTouched[i] = i < edits->touched.size() && edits->touched[i];
    }

    if (onAudition)
        onAudition (hit, xToTime (e.position.x));

    repaint();
}

void NoteGrid::mouseDrag (const MouseEvent& e)
{
    if (edits == nullptr)
        return;

    if (lassoing)
    {
        lassoEnd = e.position;
        const Rectangle<float> box (lassoStart, lassoEnd);

        fitSelectionToNotes();

        for (size_t i = 0; i < selected.size(); ++i)
        {
            const auto inBase = i < lassoBase.size() && lassoBase[i] != 0;
            selected[i] = (inBase || box.intersects (blobFor (i))) ? (char) 1 : (char) 0;
        }

        selectionChanged();
        repaint();
        return;
    }

    if (draggingNote < 0 || (size_t) draggingNote >= dragStartTargets.size()
        || edits->notes.size() != dragStartTargets.size())
        return;

    const auto grabbed = (size_t) draggingNote;
    const auto delta = yToCents (e.position.y) - dragStartCents;
    auto target = dragStartTargets[grabbed] + delta;

    // Free drag with a modifier, otherwise land on the scale. Dragging a note
    // by hand should put it exactly where the user means it, and most of the
    // time that is a scale tone.
    if (! e.mods.isAltDown())
        target = edits->settings.scale.snapCents (target);

    // Every selected note moves by what the grabbed one moved, so a phrase
    // keeps its shape when it is shifted as a whole.
    const auto shift = target - dragStartTargets[grabbed];

    if (! dragChanged)
    {
        if (approximatelyEqual (shift, 0.0f))
            return;   // a click, not a drag: nothing to undo yet

        dragChanged = true;

        if (onBeginEdit)
            onBeginEdit();
    }

    edits->touched.resize (edits->notes.size(), false);

    for (size_t i = 0; i < edits->notes.size(); ++i)
    {
        if (i != grabbed && ! isSelected (i))
            continue;

        edits->notes[i].targetCents = dragStartTargets[i] + shift;

        // Grabbing a note means you want it moved, so give it full correction
        // unless the user had already dialled it back.
        if (! dragWasTouched[i])
            edits->notes[i].correctAmount = 1.0f;

        edits->touched[i] = true;
    }

    edits->bump();

    if (onEdit)
        onEdit();

    repaint();
}

void NoteGrid::mouseUp (const MouseEvent&)
{
    if (draggingNote >= 0 && onAudition)
        onAudition (-1, 0);

    draggingNote = -1;
    lassoing = false;
    repaint();
}

void NoteGrid::mouseDoubleClick (const MouseEvent& e)
{
    const auto index = noteAt (e.position);

    if (index < 0 || edits == nullptr)
        return;

    if (onBeginEdit)
        onBeginEdit();

    // Double click takes a note out of the correction entirely: the usual
    // reason a corrector sounds wrong is one deliberate bend it should not
    // have touched. With several selected, they all follow the one clicked.
    const auto bypass = ! edits->notes[(size_t) index].bypassed;

    edits->touched.resize (edits->notes.size(), false);

    for (size_t i = 0; i < edits->notes.size(); ++i)
    {
        if ((int) i != index && ! (isSelected ((size_t) index) && isSelected (i)))
            continue;

        edits->notes[i].bypassed = bypass;
        edits->touched[i] = true;
    }

    edits->bump();

    if (onEdit)
        onEdit();

    repaint();
}

void NoteGrid::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& wheel)
{
    // Some systems turn Shift + wheel into a sideways wheel and some do not,
    // so the modified gestures take whichever axis actually moved.
    const auto amount = std::abs (wheel.deltaY) >= std::abs (wheel.deltaX) ? wheel.deltaY : wheel.deltaX;
    const auto span = viewHighCents - viewLowCents;

    auto scrollTime = [this] (float notches)
    {
        if (analysis == nullptr)
            return;

        const auto shift = (int64_t) (-notches * kScrollPerNotch * (float) viewLength);
        viewStart = jlimit ((int64_t) 0, jmax ((int64_t) 0, analysis->numSamples - viewLength),
                            viewStart + shift);
    };

    auto setPitchView = [this] (float low, float newSpan)
    {
        viewLowCents  = jlimit (kLowestCents, jmax (kLowestCents, kHighestCents - newSpan), low);
        viewHighCents = viewLowCents + newSpan;
    };

    if (e.mods.isAltDown())
    {
        // Vertical zoom about the pointer.
        const auto anchor = yToCents (e.position.y);
        const auto factor = std::pow (1.2f, -amount * 3.0f);
        const auto newSpan = jlimit (kMinSemitonesVisible * 100.0f,
                                     kMaxSemitonesVisible * 100.0f, span * factor);
        const auto frac = (anchor - viewLowCents) / jmax (1.0f, span);

        setPitchView (anchor - newSpan * frac, newSpan);
    }
    else if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        if (analysis == nullptr)
            return;

        // Horizontal zoom about the pointer.
        const auto anchor = xToTime (e.position.x);
        const auto factor = std::pow (1.2, -amount * 3.0);
        const auto newLength = (int64_t) jlimit (1024.0, (double) jmax ((int64_t) 1024, analysis->numSamples),
                                                 (double) viewLength * factor);
        const auto frac = (double) (anchor - viewStart) / (double) jmax ((int64_t) 1, viewLength);

        viewStart = jlimit ((int64_t) 0, jmax ((int64_t) 0, analysis->numSamples - newLength),
                            anchor - (int64_t) (frac * (double) newLength));
        viewLength = newLength;
    }
    else if (e.mods.isShiftDown())
    {
        scrollTime (amount);
    }
    else
    {
        // The plain wheel walks up and down the keyboard; a trackpad's
        // sideways movement goes along the take.
        setPitchView (viewLowCents + wheel.deltaY * kScrollPerNotch * span, span);
        scrollTime (wheel.deltaX);
    }

    repaint();
}
void NoteGrid::resized()
{
    repaint();
}

} // namespace onirique
