#include "PluginEditor.h"

namespace onirique
{

using namespace theme;

namespace
{
    OniriqueLookAndFeel& sharedLookAndFeel()
    {
        static OniriqueLookAndFeel laf;
        return laf;
    }
}

PitchBoxEditor::PitchBoxEditor (PitchBoxProcessor& p)
    : AudioProcessorEditor (&p),
      AudioProcessorEditorARAExtension (&p),
      processor (p)
{
    setLookAndFeel (&sharedLookAndFeel());

    addAndMakeVisible (grid);
    grid.onEdit = [this] { pushEdits(); };

    for (int i = 0; i < 12; ++i)
        rootBox.addItem (pb::noteName (i), i + 1);

    rootBox.setSelectedId (1, dontSendNotification);
    rootBox.onChange = [this]
    {
        beginEdit();
        edits.settings.scale.root = rootBox.getSelectedId() - 1;
        edits.refreshUntouched();
        pushEdits();
    };
    addAndMakeVisible (rootBox);

    for (int i = 0; i < (int) pb::ScaleType::numTypes; ++i)
        if ((pb::ScaleType) i != pb::ScaleType::custom)
            scaleBox.addItem (pb::scaleTypeName ((pb::ScaleType) i), i + 1);

    scaleBox.setSelectedId ((int) pb::ScaleType::chromatic + 1, dontSendNotification);
    scaleBox.onChange = [this]
    {
        beginEdit();
        edits.settings.scale.type = (pb::ScaleType) (scaleBox.getSelectedId() - 1);
        edits.refreshUntouched();
        pushEdits();
    };
    addAndMakeVisible (scaleBox);

    auto setUpKnob = [this] (Slider& s, Label& l, const String& name,
                             double min, double max, double value, const String& suffix)
    {
        s.setSliderStyle (Slider::RotaryVerticalDrag);
        s.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        s.setRange (min, max, 0.001);
        s.setValue (value, dontSendNotification);
        s.setTextValueSuffix (suffix);
        s.onDragStart = [this] { beginEdit(); };
        addAndMakeVisible (s);

        l.setText (name, dontSendNotification);
        l.setJustificationType (Justification::centred);
        l.setColour (Label::textColourId, haze);
        addAndMakeVisible (l);
    };

    setUpKnob (amountSlider, amountLabel, "AMOUNT", 0.0, 1.0, 0.0, "");
    setUpKnob (transitionSlider, transitionLabel, "SPEED", 0.0, 0.25, 0.035, " s");
    setUpKnob (formantSlider, formantLabel, "FORMANT", 0.7, 1.4, 1.0, "");
    setUpKnob (modulationSlider, modulationLabel, "MODULATION", 0.0, 2.0, 1.0, "");

    // How much of the sung movement inside a note is kept: vibrato, scoops and
    // drift alike. At 0 the pitch line of the selected notes is drawn flat,
    // at 1 it is as sung, and above that it is exaggerated.
    modulationSlider.setDoubleClickReturnValue (true, 1.0);
    modulationSlider.onValueChange = [this]
    {
        const auto selection = grid.getSelection();

        if (selection.empty())
            return;

        if (! modulationSlider.isMouseButtonDown())
            beginEdit();

        edits.touched.resize (edits.notes.size(), false);

        for (const auto index : selection)
        {
            edits.notes[(size_t) index].vibratoScale = (float) modulationSlider.getValue();
            edits.touched[(size_t) index] = true;
        }

        edits.bump();
        pushEdits();
        grid.repaint();
    };

    amountSlider.onValueChange = [this]
    {
        edits.settings.globalAmount = (float) amountSlider.getValue();
        edits.refreshUntouched();
        pushEdits();
    };

    transitionSlider.onValueChange = [this]
    {
        edits.settings.transitionSeconds = transitionSlider.getValue();
        edits.bump();
        pushEdits();
    };

    formantSlider.onValueChange = [this]
    {
        edits.settings.formantRatio = (float) formantSlider.getValue();
        edits.bump();
        pushEdits();
    };

    enabledButton.setClickingTogglesState (true);
    enabledButton.setToggleState (true, dontSendNotification);
    enabledButton.onClick = [this]
    {
        beginEdit();
        edits.settings.enabled = enabledButton.getToggleState();
        edits.bump();
        pushEdits();
    };
    addAndMakeVisible (enabledButton);

    detectKeyButton.onClick = [this]
    {
        if (auto a = currentAnalysis())
        {
            beginEdit();
            edits.settings.scale = a->key.scale;
            rootBox.setSelectedId (a->key.scale.root + 1, dontSendNotification);
            scaleBox.setSelectedId ((int) a->key.scale.type + 1, dontSendNotification);
            edits.refreshUntouched();
            pushEdits();
        }
    };
    addAndMakeVisible (detectKeyButton);

    undoButton.onClick = [this] { undo(); };
    redoButton.onClick = [this] { redo(); };
    resetButton.onClick = [this] { resetNotes(); };

    for (auto* b : { &undoButton, &redoButton, &resetButton })
        addAndMakeVisible (b);

    grid.onBeginEdit = [this] { beginEdit(); };
    grid.onSelectionChanged = [this] { selectionChanged(); };
    grid.onAudition = [this] (int index) { auditionNote (index); };

    updateHistoryButtons();
    selectionChanged();

    transferButton.onClick = [this]
    {
        processor.getTransfer().toggleTransfer();
        updateTransferStatus();
    };
    addChildComponent (transferButton);

    processor.getTransfer().addChangeListener (this);
    analysisQueue->addListener (this);

    setResizable (true, true);
    setResizeLimits (980, 600, 2400, 1500);
    setSize (1160, 720);

    refreshTarget();
    startTimerHz (12);
}

PitchBoxEditor::~PitchBoxEditor()
{
    analysisQueue->removeListener (this);
    processor.getTransfer().removeChangeListener (this);
    processor.getTransfer().stopAudition();
    setLookAndFeel (nullptr);
}

//==============================================================================
void PitchBoxEditor::refreshTarget()
{
    currentSource = nullptr;
    currentModification = nullptr;

    auto* view = getARAEditorView();

    if (view == nullptr)
    {
        // No ARA, so nobody is going to hand us a take: the user transfers one.
        if (! transferMode)
        {
            transferMode = true;
            transferButton.setVisible (true);
            resized();
        }

        refreshTransfer();
        return;
    }

    if (transferMode)
    {
        transferMode = false;
        transferButton.setVisible (false);
        shownAnalysis = nullptr;
        resized();
    }

    auto* documentController = view->getDocumentController();

    if (documentController == nullptr)
        return;

    auto* specialisation =
        ARADocumentControllerSpecialisation::getSpecialisedDocumentController<PitchBoxDocumentController> (
            documentController);

    if (specialisation == nullptr)
        return;

    auto* document = specialisation->getDocument();

    if (document == nullptr)
        return;

    // Follow the host's selection when there is one; otherwise fall back to the
    // first take in the document, so the editor is never blank for no reason.
    for (const auto* regionSequence : view->getViewSelection().getRegionSequences())
    {
        for (auto* region : regionSequence->getPlaybackRegions())
        {
            currentModification = region->getAudioModification<PitchBoxAudioModification>();
            currentSource = static_cast<PitchBoxAudioSource*> (
                region->getAudioModification()->getAudioSource());
            break;
        }

        if (currentSource != nullptr)
            break;
    }

    if (currentSource == nullptr)
    {
        for (auto* source : document->getAudioSources<PitchBoxAudioSource>())
        {
            const auto modifications = source->getAudioModifications<PitchBoxAudioModification>();

            if (! modifications.empty())
            {
                currentSource = source;
                currentModification = modifications.front();
                break;
            }
        }
    }

    if (currentModification != nullptr)
        edits = currentModification->getEdits();

    if (currentSource != nullptr)
    {
        auto analysis = currentSource->getAnalysis();

        if (analysis != nullptr && edits.notes.empty())
        {
            edits.adoptAnalysis (*analysis);
            pushEdits();
        }

        grid.setAnalysis (analysis);
    }

    grid.setEditState (&edits);
}

void PitchBoxEditor::pushEdits()
{
    if (transferMode)
        processor.getTransfer().setEdits (edits);
    else if (currentModification != nullptr)
        currentModification->setEdits (edits);
}

//==============================================================================
void PitchBoxEditor::beginEdit()
{
    undoStack.push_back (edits);

    if (undoStack.size() > 200)
        undoStack.erase (undoStack.begin());

    redoStack.clear();
    updateHistoryButtons();
}

void PitchBoxEditor::restore (pb::EditState state)
{
    // Versions only ever go up: the renderer keys its curve cache on them, so
    // coming back to an old state must not come back to its old number.
    state.version = jmax (state.version, edits.version) + 1;
    edits = std::move (state);

    pushEdits();
    syncControls();
    grid.setEditState (&edits);
    selectionChanged();
    updateHistoryButtons();
}

void PitchBoxEditor::undo()
{
    if (undoStack.empty())
        return;

    redoStack.push_back (edits);
    auto state = std::move (undoStack.back());
    undoStack.pop_back();
    restore (std::move (state));
}

void PitchBoxEditor::redo()
{
    if (redoStack.empty())
        return;

    undoStack.push_back (edits);
    auto state = std::move (redoStack.back());
    redoStack.pop_back();
    restore (std::move (state));
}

void PitchBoxEditor::resetNotes()
{
    auto analysis = currentAnalysis();

    if (analysis == nullptr)
        return;

    beginEdit();

    // Back to what the analysis found, with the scale settings still applied:
    // every hand edit goes, the global correction stays.
    edits.notes = analysis->notes;
    edits.touched.assign (edits.notes.size(), false);
    edits.refreshUntouched();

    pushEdits();
    grid.setEditState (&edits);
    selectionChanged();
}

void PitchBoxEditor::clearHistory()
{
    undoStack.clear();
    redoStack.clear();
    updateHistoryButtons();
}

void PitchBoxEditor::updateHistoryButtons()
{
    auto show = [] (Button& b, bool usable)
    {
        b.setEnabled (usable);
        b.setAlpha (usable ? 1.0f : 0.4f);
    };

    show (undoButton, ! undoStack.empty());
    show (redoButton, ! redoStack.empty());
    show (resetButton, ! edits.notes.empty());
}

void PitchBoxEditor::selectionChanged()
{
    const auto selection = grid.getSelection();
    const auto usable = ! selection.empty();

    for (auto* c : std::initializer_list<Component*> { &modulationSlider, &modulationLabel })
    {
        c->setEnabled (usable);
        c->setAlpha (usable ? 1.0f : 0.4f);
    }

    const auto shown = usable && (size_t) selection.front() < edits.notes.size()
                           ? edits.notes[(size_t) selection.front()].vibratoScale
                           : 1.0f;

    modulationSlider.setValue (shown, dontSendNotification);
}

void PitchBoxEditor::auditionNote (int index)
{
    // Only in transfer mode: that is where the plugin holds the audio itself.
    if (! transferMode)
        return;

    auto& session = processor.getTransfer();

    if (index < 0 || (size_t) index >= edits.notes.size())
    {
        session.stopAudition();
        return;
    }

    const auto& n = edits.notes[(size_t) index];
    session.startAudition (n.startSample, n.endSample);
}

pb::Analysis::Ptr PitchBoxEditor::currentAnalysis() const
{
    if (transferMode)
        return processor.getTransfer().getAnalysis();

    return currentSource != nullptr ? currentSource->getAnalysis() : nullptr;
}

//==============================================================================
void PitchBoxEditor::refreshTransfer()
{
    auto& session = processor.getTransfer();

    edits = session.getEdits();

    // Only when the take itself changed: setting the analysis also resets the
    // zoom, which must not happen every time a transfer is armed.
    if (auto latest = session.getAnalysis(); latest != shownAnalysis)
    {
        shownAnalysis = latest;
        grid.setAnalysis (latest);

        // The history describes notes of a take that is no longer there.
        clearHistory();
    }

    grid.setEditState (&edits);
    syncControls();
    updateHistoryButtons();
    updateTransferStatus();
}

void PitchBoxEditor::syncControls()
{
    const auto& s = edits.settings;

    rootBox.setSelectedId (s.scale.root + 1, dontSendNotification);
    scaleBox.setSelectedId ((int) s.scale.type + 1, dontSendNotification);
    amountSlider.setValue (s.globalAmount, dontSendNotification);
    transitionSlider.setValue (s.transitionSeconds, dontSendNotification);
    formantSlider.setValue (s.formantRatio, dontSendNotification);
    enabledButton.setToggleState (s.enabled, dontSendNotification);
}

void PitchBoxEditor::showStatus (const String& headerText, const String& gridText, float newGridProgress)
{
    if (headerText != status)
    {
        status = headerText;
        repaint (getLocalBounds().removeFromTop (58));
    }

    if (gridText != gridStatus || ! approximatelyEqual (newGridProgress, gridProgress))
    {
        gridStatus = gridText;
        gridProgress = newGridProgress;
        grid.setStatus (gridText, newGridProgress);
    }
}

void PitchBoxEditor::updateTransferStatus()
{
    auto& session = processor.getTransfer();
    const auto state = session.getState();

    using State = TransferSession::State;

    transferButton.setToggleState (state != State::idle, dontSendNotification);
    transferButton.setButtonText (state == State::idle ? "TRANSFER"
                                                       : state == State::armed ? "CANCEL" : "STOP");

    if (state == State::armed)
    {
        const String text ("Armed. Press play in the host over the passage to transfer.");
        showStatus (text, text, 0.0f);
        return;
    }

    if (state == State::recording || state == State::finishing)
    {
        const auto text = "RECORDING  " + String (session.getRecordedSeconds(), 1) + " s";
        showStatus (text, text, 0.0f);
        return;
    }

    if (session.isAnalysing())
    {
        const auto p = session.getAnalysisProgress();
        const auto text = "ANALYSING  " + String (roundToInt (p * 100.0f)) + "%";
        showStatus (text, text, p);
        return;
    }

    if (auto analysis = session.getAnalysis())
    {
        const auto takeRate = session.getTakeSampleRate();

        // A take is only played back at the rate it was recorded at; say so
        // rather than leave the user wondering why nothing is corrected.
        if (! approximatelyEqual (takeRate, session.getHostSampleRate()))
        {
            showStatus ("Take recorded at " + String (roundToInt (takeRate))
                            + " Hz. Transfer it again at the current rate.",
                        {}, 1.0f);
            return;
        }

        showStatus (String (analysis->notes.size()) + String (CharPointer_UTF8 (" notes  \xc2\xb7  key "))
                        + String (analysis->key.scale.describe()),
                    {}, 1.0f);
        return;
    }

    const String text ("Click TRANSFER, then play the passage in the host.");
    showStatus (text, text, 0.0f);
}

void PitchBoxEditor::changeListenerCallback (ChangeBroadcaster*)
{
    if (transferMode)
        refreshTransfer();
}

//==============================================================================
void PitchBoxEditor::analysisFinished (PitchBoxAudioSource* source)
{
    MessageManager::callAsync ([this, source]
    {
        if (source != currentSource)
            return;

        auto analysis = source->getAnalysis();

        if (analysis == nullptr)
            return;

        edits.adoptAnalysis (*analysis);
        grid.setAnalysis (analysis);
        grid.setStatus ({}, 1.0f);
        clearHistory();

        status = String (analysis->notes.size()) + " notes  ·  key "
                 + String (analysis->key.scale.describe());

        pushEdits();
        repaint();
    });
}

void PitchBoxEditor::analysisProgressed (PitchBoxAudioSource* source, float fraction)
{
    if (source == currentSource)
        progress = fraction;
}

void PitchBoxEditor::timerCallback()
{
    if (transferMode && getARAEditorView() == nullptr)
    {
        updateTransferStatus();

        // The analysis thread adopts its result into the edit state itself.
        if (processor.getTransfer().getEditVersion() != edits.version)
        {
            edits = processor.getTransfer().getEdits();
            grid.setEditState (&edits);
        }

        return;
    }

    if (currentSource == nullptr)
    {
        refreshTarget();
        return;
    }

    if (currentSource->analysisInProgress)
    {
        const auto p = currentSource->analysisProgress.load();
        grid.setStatus ("ANALYSING  " + String (roundToInt (p * 100.0f)) + "%", p);
    }

    // The host may have changed the selection, or the user may have edited the
    // same take from another plugin window.
    if (currentModification != nullptr
        && currentModification->getEditVersion() != edits.version)
    {
        edits = currentModification->getEdits();
        grid.setEditState (&edits);
    }
}

//==============================================================================
void PitchBoxEditor::paint (Graphics& g)
{
    g.fillAll (night);

    auto header = getLocalBounds().removeFromTop (58);

    g.setGradientFill (ColourGradient (deep2, 0.0f, 0.0f, night, 0.0f, (float) header.getBottom(), false));
    g.fillRect (header);

    g.setColour (line);
    g.drawHorizontalLine (header.getBottom() - 1, 0.0f, (float) getWidth());

    g.setColour (mist);
    g.setFont (displayFont (19.0f));
    g.drawText ("ONIRIQUE", header.withTrimmedLeft (22).withWidth (140),
                Justification::centredLeft, false);

    g.setColour (ember);
    g.setFont (monoFont (12.0f));
    g.drawText ("PITCHBOX", header.withTrimmedLeft (150).withWidth (160),
                Justification::centredLeft, false);

    g.setColour (haze);
    g.setFont (monoFont (11.0f));
    g.drawText (status, header.withTrimmedRight (transferMode ? 300 : 170).withTrimmedLeft (476),
                Justification::centredRight, true);

    // Side panel ground.
    auto side = getLocalBounds().withTop (58).removeFromLeft (178);
    g.setColour (deep);
    g.fillRect (side);
    g.setColour (line);
    g.drawVerticalLine (side.getRight(), (float) side.getY(), (float) side.getBottom());
}

void PitchBoxEditor::resized()
{
    auto bounds = getLocalBounds();
    auto header = bounds.removeFromTop (58);

    enabledButton.setBounds (header.removeFromRight (150).reduced (14, 16));

    if (transferMode)
        transferButton.setBounds (header.removeFromRight (130).withTrimmedRight (-8).reduced (0, 16));

    auto history = header.withTrimmedLeft (236).withWidth (228).reduced (0, 16);

    for (auto* b : { &undoButton, &redoButton, &resetButton })
    {
        b->setBounds (history.removeFromLeft (72));
        history.removeFromLeft (6);
    }

    auto side = bounds.removeFromLeft (178).reduced (14, 16);

    side.removeFromTop (4);
    rootBox.setBounds (side.removeFromTop (30));
    side.removeFromTop (8);
    scaleBox.setBounds (side.removeFromTop (30));
    side.removeFromTop (8);
    detectKeyButton.setBounds (side.removeFromTop (30));
    side.removeFromTop (14);

    // Four knobs share whatever height is left, so the panel still fits at the
    // smallest window size.
    const auto knobHeight = jlimit (86, 112, side.getHeight() / 4 - 4);

    auto knob = [&side, knobHeight] (Slider& s, Label& l)
    {
        auto area = side.removeFromTop (knobHeight);
        s.setBounds (area.removeFromTop (knobHeight - 22).reduced (14, 0));
        l.setBounds (area);
        side.removeFromTop (4);
    };

    knob (amountSlider, amountLabel);
    knob (transitionSlider, transitionLabel);
    knob (formantSlider, formantLabel);
    knob (modulationSlider, modulationLabel);

    grid.setBounds (bounds);
}

} // namespace onirique
