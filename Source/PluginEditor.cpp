#include "PluginEditor.h"

#include <BinaryData.h>

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

    /** The first of the dropped files that is audio we can read, if any. */
    File droppedAudioFile (const StringArray& files)
    {
        for (const auto& path : files)
        {
            const File file (path);

            if (file.hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;wma;m4a"))
                return file;
        }

        return {};
    }

    constexpr int kHeaderHeight = 68;
    constexpr int kToolbarHeight = 40;
    constexpr int kTitleLeft = 72;

    Font titleFont (bool heavy)
    {
        return displayFont (23.0f, heavy).withExtraKerningFactor (0.09f);
    }

    /** A four-pointed star, as on the Onirique sigil. */
    Path star (Point<float> centre, float radius)
    {
        Path p;
        const auto waist = radius * 0.16f;

        p.startNewSubPath (centre.x, centre.y - radius);
        p.lineTo (centre.x + waist, centre.y - waist);
        p.lineTo (centre.x + radius, centre.y);
        p.lineTo (centre.x + waist, centre.y + waist);
        p.lineTo (centre.x, centre.y + radius);
        p.lineTo (centre.x - waist, centre.y + waist);
        p.lineTo (centre.x - radius, centre.y);
        p.lineTo (centre.x - waist, centre.y - waist);
        p.closeSubPath();
        return p;
    }

    constexpr int kLogoWidth = 44;
    constexpr int kLogoMargin = 8;   // room for the glow around it

    Image makeLogo()
    {
        const auto sigil = ImageCache::getFromMemory (BinaryData::sigil_png, BinaryData::sigil_pngSize);

        if (! sigil.isValid())
            return {};

        const auto width = 2 * kLogoWidth;
        const auto height = width * sigil.getHeight() / sigil.getWidth();
        const auto margin = 2 * kLogoMargin;

        Image padded (Image::ARGB, width + 2 * margin, height + 2 * margin, true);

        {
            Graphics g (padded);
            g.setImageResamplingQuality (Graphics::highResamplingQuality);
            g.drawImage (sigil, Rectangle<int> (margin, margin, width, height).toFloat());
        }

        Image result (Image::ARGB, padded.getWidth(), padded.getHeight(), true);
        Graphics g (result);

        GlowEffect glow;
        glow.setGlowProperties (7.0f, ember.withAlpha (0.8f));
        glow.applyEffect (padded, g, 1.0f, 1.0f);

        return result;
    }
}

PitchBoxEditor::PitchBoxEditor (PitchBoxProcessor& p)
    : AudioProcessorEditor (&p),
      AudioProcessorEditorARAExtension (&p),
      processor (p)
{
    setLookAndFeel (&sharedLookAndFeel());
    logo = makeLogo();

    addAndMakeVisible (grid);
    grid.onEdit = [this] { pushEdits(); };

    rootBox.addSectionHeading ("KEY");

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

    scaleBox.addSectionHeading ("SCALE");

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

    auto setUpKnob = [this] (Slider& s, Label& l, Label& readout, const String& name,
                             double min, double max, double value,
                             const String& low, const String& high)
    {
        s.setSliderStyle (Slider::RotaryVerticalDrag);
        s.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
        s.setRange (min, max, 0.001);
        s.setValue (value, dontSendNotification);

        // Double click puts a knob back where it started.
        s.setDoubleClickReturnValue (true, value);

        s.getProperties().set ("low", low);
        s.getProperties().set ("high", high);
        s.onDragStart = [this] { beginEdit(); };
        addAndMakeVisible (s);

        l.setText (name, dontSendNotification);
        l.setJustificationType (Justification::centred);
        l.setColour (Label::textColourId, mist);
        addAndMakeVisible (l);

        readout.getProperties().set ("pill", true);
        readout.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (readout);
    };

    setUpKnob (amountSlider, amountLabel, amountValue, "AMOUNT", 0.0, 1.0, 0.0, "0", "100");
    setUpKnob (transitionSlider, transitionLabel, transitionValue, "SPEED", 0.0, 0.25, 0.035, "0", "250");
    setUpKnob (formantSlider, formantLabel, formantValue, "FORMANT", 0.7, 1.4, 1.0, "0.7", "1.4");
    setUpKnob (modulationSlider, modulationLabel, modulationValue, "MODULATION", 0.0, 2.0, 1.0, "0", "200");
    updateValueReadouts();
    // How much of the sung movement inside a note is kept: vibrato, scoops and
    // drift alike. At 0 the pitch line of the selected notes is drawn flat,
    // at 1 it is as sung, and above that it is exaggerated.

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
    grid.onAudition = [this] (int index, int64_t position) { auditionNote (index, position); };

    updateHistoryButtons();
    selectionChanged();

    stopButton.onClick = [this] { stopLocally(); };
    playButton.onClick = [this] { playLocally(); };

    loopButton.setClickingTogglesState (true);
    loopButton.onClick = [this] { applyLoop(); };


    for (auto* b : std::initializer_list<Component*> { &stopButton, &playButton, &loopButton })
        addChildComponent (b);

    midiButton.onClick = [this] { exportMidi(); };
    addAndMakeVisible (midiButton);

    // The level of the selected notes.
    volumeSlider.setSliderStyle (Slider::LinearHorizontal);
    volumeSlider.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
    volumeSlider.setRange (-24.0, 12.0, 0.1);
    volumeSlider.setValue (0.0, dontSendNotification);
    volumeSlider.setDoubleClickReturnValue (true, 0.0);
    volumeSlider.onDragStart = [this] { beginEdit(); };
    volumeSlider.onValueChange = [this]
    {
        const auto selection = grid.getSelection();

        if (selection.empty())
            return;

        if (! volumeSlider.isMouseButtonDown())
            beginEdit();

        edits.touched.resize (edits.notes.size(), false);

        for (const auto index : selection)
        {
            edits.notes[(size_t) index].gainDb = (float) volumeSlider.getValue();
            edits.touched[(size_t) index] = true;
        }

        edits.bump();
        pushEdits();
        grid.repaint();
    };
    addAndMakeVisible (volumeSlider);

    volumeLabel.setText ("VOLUME", dontSendNotification);
    volumeLabel.setJustificationType (Justification::centredRight);
    volumeLabel.setColour (Label::textColourId, mist);
    addAndMakeVisible (volumeLabel);

    volumeValue.getProperties().set ("pill", true);
    volumeValue.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (volumeValue);

    inspector.setJustificationType (Justification::centredRight);
    inspector.setColour (Label::textColourId, Colour (0xffe9c9cc));
    inspector.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (inspector);

    // What each control is, for the pointer that rests on it. The name and the
    // readout of a knob are part of the knob.
    describe ({ &rootBox }, "Key: the note the scale is built on.");
    describe ({ &scaleBox }, "Scale: which notes the correction pulls towards.");
    describe ({ &detectKeyButton }, "Detect key: set the key and scale to the ones found in the take.");
    describe ({ &amountSlider, &amountLabel, &amountValue },
              "Amount: how far every untouched note is pulled onto the scale. Double-click to reset.");
    describe ({ &transitionSlider, &transitionLabel, &transitionValue },
              "Speed: how long the pitch takes to glide from one note to the next. 0 is a hard jump.");
    describe ({ &formantSlider, &formantLabel, &formantValue },
              "Formant: shifts the colour of the voice without changing its pitch.");
    describe ({ &modulationSlider, &modulationLabel, &modulationValue },
              "Modulation: how much of the vibrato and drift inside the selected notes is kept. 0 flattens them.");
    describe ({ &volumeSlider, &volumeLabel, &volumeValue },
              "Volume of the selected notes, in dB. Double-click to reset.");
    describe ({ &undoButton }, "Undo the last edit.");
    describe ({ &redoButton }, "Redo the edit that was undone.");
    describe ({ &resetButton },
              "Reset: drop every hand edit and go back to the analysed notes. The key and knobs are kept.");
    describe ({ &transferButton },
              "Transfer: arm, then play the passage in the host to record it into the plugin.");
    describe ({ &enabledButton }, "Correction on or off. Off plays the take as it was recorded.");
    describe ({ &stopButton }, "Stop. Pressed again: back to the start.");
    describe ({ &playButton }, "Play the take from here, without the host.");
    describe ({ &loopButton }, "Loop: go round the selected notes, or the whole take.");
    describe ({ &midiButton }, "Export MIDI: save the notes, as they will sound, to a MIDI file.");
    describe ({ &inspector }, "The selected note: what was sung, how far off it was, where it is going, how long it lasts.");

    hoverTip.setInterceptsMouseClicks (false, false);
    hoverTip.setAlwaysOnTop (true);
    addChildComponent (hoverTip);
    transferButton.onClick = [this]
    {
        processor.getTransfer().toggleTransfer();
        updateTransferStatus();
    };
    addChildComponent (transferButton);

    processor.getTransfer().addChangeListener (this);
    analysisQueue->addListener (this);

    setResizable (true, true);
    setResizeLimits (1060, 690, 2400, 1500);
    setSize (1200, 770);

    refreshTarget();
    startTimerHz (30);
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

            for (auto* b : std::initializer_list<Component*> { &stopButton, &playButton, &loopButton })
                b->setVisible (true);

            resized();
        }

        refreshTransfer();
        return;
    }

    if (transferMode)
    {
        transferMode = false;
        transferButton.setVisible (false);

        for (auto* b : std::initializer_list<Component*> { &stopButton, &playButton, &loopButton })
            b->setVisible (false);
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

    for (auto* c : std::initializer_list<Component*> { &modulationSlider, &modulationLabel, &modulationValue,
                                                       &volumeSlider, &volumeLabel, &volumeValue })
    {
        c->setEnabled (usable);
        c->setAlpha (usable ? 1.0f : 0.4f);
    }

    const auto shown = usable && (size_t) selection.front() < edits.notes.size()
                           ? edits.notes[(size_t) selection.front()].vibratoScale
                           : 1.0f;

    modulationSlider.setValue (shown, dontSendNotification);

    volumeSlider.setValue (usable && (size_t) selection.front() < edits.notes.size()
                               ? edits.notes[(size_t) selection.front()].gainDb
                               : 0.0f,
                           dontSendNotification);
    updateInspector();
}

void PitchBoxEditor::describe (std::initializer_list<Component*> components, const String& text)
{
    for (auto* c : components)
        described.push_back ({ c, text });
}

void PitchBoxEditor::updateHoverTip()
{
    const auto now = Time::getMillisecondCounter();
    const auto at = getMouseXYRelative();

    // Only while the pointer is over this window with no button held.
    auto* under = Desktop::getInstance().getMainMouseSource().getComponentUnderMouse();
    const auto here = under != nullptr && (under == this || isParentOf (under))
                      && ! ModifierKeys::currentModifiers.isAnyMouseButtonDown();

    const Described* over = nullptr;

    if (here)
        for (const auto& d : described)
            if (d.component->isVisible() && d.component->getBounds().contains (at))
                over = &d;

    // Any movement starts the wait again, and takes the description away.
    if (over != resting || at.getDistanceFrom (restingAt) > 3)
    {
        resting = over;
        restingAt = at;
        restingSince = now;
        hoverTip.setVisible (false);
        return;
    }

    if (resting == nullptr || hoverTip.isVisible() || now - restingSince < 2000)
        return;

    hoverTip.text = resting->text;
    hoverTip.setBounds (getLookAndFeel().getTooltipBounds (resting->text, at, getLocalBounds().reduced (6)));
    hoverTip.setVisible (true);
    hoverTip.toFront (false);
    hoverTip.repaint();
}

void PitchBoxEditor::updateInspector()
{
    const auto selection = grid.getSelection();
    const auto analysis = currentAnalysis();
    String text;

    auto keyName = [] (float cents)
    {
        const auto key = roundToInt (cents / 100.0f);
        return String (pb::noteName (((key % 12) + 12) % 12)) + String (key / 12 - 1);
    };

    if (selection.size() == 1 && analysis != nullptr && (size_t) selection.front() < edits.notes.size())
    {
        // One note: what was sung, how far off, where it is going, how long.
        const auto& n = edits.notes[(size_t) selection.front()];
        const auto sungKey = (float) roundToInt (n.centsDetected / 100.0f) * 100.0f;
        const auto off = roundToInt (n.centsDetected - sungKey);

        text << keyName (n.centsDetected) << " " << (off >= 0 ? "+" : "") << off << " ct";

        if (n.bypassed)
            text << "   left alone";
        else
            text << "   to " << keyName (n.resultCents());

        text << "   " << String ((double) (n.endSample - n.startSample) / analysis->sampleRate, 2) << " s";
    }
    else if (selection.size() > 1)
    {
        text << (int) selection.size() << " notes selected";
    }

    inspector.setText (text, dontSendNotification);
}

void PitchBoxEditor::auditionNote (int index, int64_t position)
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
    session.startAudition (n.startSample, n.endSample, position);
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
        repaint (getLocalBounds().removeFromTop (kHeaderHeight));
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

    if (session.isImporting())
    {
        const String text ("IMPORTING");
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

        // A dropped file is all here, but where it sits on the timeline is
        // only known once the track has been heard going past.
        if (! session.isPlaced())
        {
            showStatus (session.placingFailed()
                            ? "This file does not match what the track plays. Use TRANSFER instead."
                            : "Play the vocal for a second so the file can be lined up with the track.",
                        {}, 1.0f);
            return;
        }

        auto text = String (analysis->notes.size()) + String (CharPointer_UTF8 (" notes  \xc2\xb7  key "))
                        + String (analysis->key.scale.describe());

        // Only in a host whose position wanders: how often, and how far.
        if (const auto slips = session.getPositionSlips(); slips > 0)
            text << String (CharPointer_UTF8 ("  \xc2\xb7  sync ")) << slips << "/" << session.getWorstPositionSlip();

        showStatus (text, {}, 1.0f);
        return;
    }

    const String text (session.importFailed() ? "That file could not be read. Drop a WAV, AIFF, FLAC, OGG or MP3."
                                              : "Drop the vocal's file here, or click TRANSFER and play the passage.");
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

        araTakeStatus = String (analysis->notes.size()) + String (CharPointer_UTF8 (" notes  \xc2\xb7  key "))
                        + String (analysis->key.scale.describe());
        status = araTakeStatus;

        pushEdits();
        repaint();
    });
}

void PitchBoxEditor::analysisProgressed (PitchBoxAudioSource* source, float fraction)
{
    if (source == currentSource)
        progress = fraction;
}

void PitchBoxEditor::updateValueReadouts()
{
    const auto db = volumeSlider.getValue();
    volumeValue.setText ((db > 0.0 ? "+" : "") + String (db, 1) + " dB", dontSendNotification);

    auto percent = [] (double v) { return String (roundToInt (v * 100.0)) + " %"; };

    amountValue.setText (percent (amountSlider.getValue()), dontSendNotification);
    transitionValue.setText (String (roundToInt (transitionSlider.getValue() * 1000.0)) + " ms", dontSendNotification);
    formantValue.setText (String (formantSlider.getValue(), 2), dontSendNotification);
    modulationValue.setText (percent (modulationSlider.getValue()), dontSendNotification);
}

void PitchBoxEditor::playLocally()
{
    auto& session = processor.getTransfer();

    if (session.getAnalysis() == nullptr)
        return;

    // From the first selected note when there is a selection, otherwise from
    // wherever the last click or the last stop left it.
    const auto selection = grid.getSelection();

    if (! selection.empty() && (size_t) selection.front() < edits.notes.size())
        localStart = edits.notes[(size_t) selection.front()].startSample;

    applyLoop();
    session.startLocalPlayback (localStart);
}

void PitchBoxEditor::stopLocally()
{
    auto& session = processor.getTransfer();

    // Stopping keeps the place; stopping again goes back to the start.
    if (session.isLocalPlaying())
    {
        localStart = session.getLocalPosition();
        session.stopLocalPlayback();
    }
    else
    {
        localStart = 0;
    }
}

void PitchBoxEditor::applyLoop()
{
    auto& session = processor.getTransfer();
    const auto analysis = session.getAnalysis();

    if (analysis == nullptr)
        return;

    int64 from = 0, to = analysis->numSamples;
    const auto selection = grid.getSelection();

    if (! selection.empty())
    {
        from = to;
        to = 0;

        for (const auto index : selection)
        {
            if ((size_t) index >= edits.notes.size())
                continue;

            from = jmin (from, (int64) edits.notes[(size_t) index].startSample);
            to = jmax (to, (int64) edits.notes[(size_t) index].endSample);
        }
    }

    session.setLocalLoop (loopButton.getToggleState(), from, to);
}

void PitchBoxEditor::exportMidi()
{
    const auto analysis = currentAnalysis();

    if (analysis == nullptr || edits.notes.empty())
        return;

    // The notes as they will sound, each on the nearest key. Seconds become
    // ticks at 120 beats a minute, which every sequencer reads the same way.
    constexpr int ticksPerBeat = 960;
    const auto ticksPerSecond = 2.0 * ticksPerBeat;

    MidiMessageSequence sequence;
    sequence.addEvent (MidiMessage::tempoMetaEvent (500000));

    for (const auto& n : edits.notes)
    {
        const auto key = jlimit (0, 127, roundToInt (n.resultCents() / 100.0f));
        const auto on = (double) n.startSample / analysis->sampleRate * ticksPerSecond;
        const auto off = (double) n.endSample / analysis->sampleRate * ticksPerSecond;

        sequence.addEvent (MidiMessage::noteOn (1, key, (uint8) 100), on);
        sequence.addEvent (MidiMessage::noteOff (1, key), jmax (on + 1.0, off));
    }

    sequence.updateMatchedPairs();

    MidiFile midi;
    midi.setTicksPerQuarterNote (ticksPerBeat);
    midi.addTrack (sequence);

    fileChooser = std::make_unique<FileChooser> ("Export the notes as MIDI",
                                                 File::getSpecialLocation (File::userDesktopDirectory)
                                                     .getChildFile ("PitchBox notes.mid"),
                                                 "*.mid");

    fileChooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles
                                  | FileBrowserComponent::warnAboutOverwriting,
                              [midi] (const FileChooser& chooser)
                              {
                                  const auto file = chooser.getResult();

                                  if (file == File())
                                      return;

                                  file.deleteFile();

                                  if (auto stream = file.withFileExtension ("mid").createOutputStream())
                                      midi.writeTo (*stream);
                              });
}

void PitchBoxEditor::updateAraStatus()
{
    using Report = AraPlaybackReport;

    // Says what playback is doing, because a host that plays nothing gives no
    // other sign of why.
    String playback;

    if (Report::calls.load() == 0)
    {
        playback = "ARA: the host has not asked this plugin for audio";
    }
    else if (Report::problem.load() == Report::rendererNotPrepared)
    {
        playback = "ARA: the host never prepared the renderer";
    }
    else if (Report::playingBlocks.load() == 0)
    {
        playback = "ARA: asked for audio, but the host says its transport is stopped";
    }
    else
    {
        switch (Report::problem.load())
        {
            case Report::regionNotPrepared:
                playback = "ARA: region not prepared";
                break;

            case Report::rateMismatch:
                playback = "ARA: take at " + String (Report::takeRate.load()) + " Hz, session at "
                           + String (Report::hostRate.load()) + " Hz";
                break;

            case Report::noRegionAtPlayhead:
                playback = "ARA: nothing at " + String (Report::playheadSeconds.load(), 1) + " s ("
                           + String (Report::regions.load()) + " regions, first "
                           + String (Report::regionStartSeconds.load(), 1) + "-"
                           + String (Report::regionEndSeconds.load(), 1) + " s)";
                break;

            default:
                playback = "ARA: playing";
                break;
        }

        playback << "  [" << Report::renderedBlocks.load() << "/" << Report::playingBlocks.load() << "]";
    }

    auto text = araTakeStatus;

    if (text.isNotEmpty())
        text << String (CharPointer_UTF8 ("  \xc2\xb7  "));

    showStatus (text + playback, gridStatus, gridProgress);
}

void PitchBoxEditor::updatePlayhead()
{
    int64 song = 0;
    const auto playing = processor.getTransport (song);

    int64 inTake = 0;
    bool visible = false;

    if (transferMode)
    {
        auto& session = processor.getTransfer();
        const auto local = session.isLocalPlaying();
        int64 start = 0;

        playButton.setToggleState (local, dontSendNotification);

        if (local)
        {
            grid.setPlayhead (session.getLocalPosition(), true, true);
            return;
        }

        if (playing && session.getTakeStartInSong (start))
        {
            inTake = song - start;
            visible = true;
        }
        else
        {
            // Stopped: show where the plugin's own playback would start.
            inTake = localStart;
            visible = session.getAnalysis() != nullptr;
        }
    }
    else if (currentModification != nullptr)
    {
        // Song time to take time, through whichever region of this take the
        // transport is in.
        for (auto* region : currentModification->getPlaybackRegions())
        {
            const auto range = region->getSampleRange (processor.getSampleRate(),
                                                       ARAPlaybackRegion::IncludeHeadAndTail::no);

            if (range.contains (song))
            {
                // The take counts in samples of its own rate, not the session's.
                const auto takeRate = currentSource != nullptr ? currentSource->getSampleRate() : 0.0;
                const auto step = takeRate > 0.0 && processor.getSampleRate() > 0.0
                                      ? takeRate / processor.getSampleRate() : 1.0;

                inTake = (int64) std::llround ((double) (song - range.getStart()) * step)
                         + region->getStartInAudioModificationSamples();
                visible = true;
                break;
            }
        }
    }

    grid.setPlayhead (inTake, visible, playing);
}

void PitchBoxEditor::timerCallback()
{
    updatePlayhead();
    updateValueReadouts();
    updateInspector();
    updateHoverTip();

    // Only what moves in the header: the light along its foot, and the stars.
    repaint (0, kHeaderHeight - 8, getWidth(), 9);
    repaint (titleRight(), 0, 56, kHeaderHeight - 8);

    if (transferMode && getARAEditorView() == nullptr)
    {
        updateTransferStatus();

        // The analysis thread adopts its result into the edit state itself.
        if (processor.getTransfer().getEditVersion() != edits.version)
        {
            edits = processor.getTransfer().getEdits();
            grid.setEditState (&edits);
            syncControls();
        }

        return;
    }

    updateAraStatus();

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
int PitchBoxEditor::titleRight() const
{
    return kTitleLeft
           + roundToInt (GlyphArrangement::getStringWidth (titleFont (true), "ONIRIQUE")
                         + GlyphArrangement::getStringWidth (titleFont (false), "PITCHBOX"))
           + 12;
}

void PitchBoxEditor::paintHeader (Graphics& g, Rectangle<int> header)
{
    const auto area = header.toFloat();
    const auto seconds = (Time::getMillisecondCounterHiRes() - openedAt) * 0.001;

    // The crimson of the range: darkest at the sigil, brightest behind the
    // name, and falling away again to the right and towards the foot.
    ColourGradient ground (Colour (0xff2c060b), 0.0f, 0.0f, Colour (0xff3b080e), area.getRight(), 0.0f, false);
    ground.addColour (0.32, Colour (0xff7a0e1c));
    ground.addColour (0.7, Colour (0xff5a0a14));
    g.setGradientFill (ground);
    g.fillRect (area);

    g.setGradientFill (ColourGradient (Colours::transparentBlack, 0.0f, area.getY(),
                                       Colour (0x73000000), 0.0f, area.getBottom(), false));
    g.fillRect (area);

    if (logo.isValid())
    {
        const auto width = (float) logo.getWidth() * 0.5f;
        const auto height = (float) logo.getHeight() * 0.5f;

        g.drawImage (logo, { 16.0f - (float) kLogoMargin, area.getCentreY() - height * 0.5f, width, height });
    }

    // The name: the house in white, the product in a paler weight beside it.
    const auto nameWidth = GlyphArrangement::getStringWidth (titleFont (true), "ONIRIQUE");

    g.setColour (Colours::white);
    g.setFont (titleFont (true));
    g.drawText ("ONIRIQUE", area.withTrimmedLeft ((float) kTitleLeft).withWidth (nameWidth + 4.0f),
                Justification::centredLeft, false);

    g.setColour (Colour (0xffe3b5ba));
    g.setFont (titleFont (false));
    g.drawText ("PITCHBOX", area.withTrimmedLeft ((float) kTitleLeft + nameWidth + 10.0f).withWidth (260.0f),
                Justification::centredLeft, false);

    // Stars beside it, each breathing at its own pace.
    struct Spark { float x, y, radius, rate, offset; };
    static const Spark sparks[] = { { 10.0f, 17.0f, 5.5f, 0.9f, 0.0f },  { 27.0f, 11.0f, 3.0f, 1.3f, 1.7f },
                                    { 38.0f, 24.0f, 4.0f, 0.7f, 3.1f },  { 18.0f, 42.0f, 4.5f, 1.1f, 4.4f },
                                    { 34.0f, 48.0f, 2.5f, 1.6f, 2.3f },  { 46.0f, 38.0f, 2.0f, 1.0f, 5.2f } };

    const auto sparkLeft = (float) titleRight();

    for (const auto& s : sparks)
    {
        const auto glow = 0.5f + 0.5f * (float) std::sin (seconds * s.rate * 2.0 + s.offset);

        g.setColour (Colours::white.withAlpha (0.25f + 0.65f * glow));
        g.fillPath (star ({ sparkLeft + s.x, area.getY() + s.y }, s.radius * (0.8f + 0.2f * glow)));
    }

    g.setColour (Colour (0xffe9c9cc));
    g.setFont (monoFont (11.0f));
    g.drawText (status, header.withTrimmedRight (transferMode ? 300 : 170).withTrimmedLeft (titleRight() + 56 + 240),
                Justification::centredRight, true);

    // The foot of the header: a faint rule, and a streak of light that runs
    // along it from left to right and comes round again.
    const auto footY = area.getBottom() - 2.0f;

    g.setColour (Colour (0x30ffd2d6));
    g.fillRect (0.0f, footY, area.getWidth(), 1.0f);

    const auto length = jmax (160.0f, area.getWidth() * 0.22f);
    const auto cycle = 3.6;
    const auto travelled = (float) (std::fmod (seconds, cycle) / cycle);
    const auto head = -40.0f + travelled * (area.getWidth() + length + 80.0f);
    const auto tail = head - length;

    // Bright at the head, trailing off behind it, with a halo around the lot.
    ColourGradient halo (Colours::transparentBlack, tail, 0.0f, Colours::transparentBlack, head + 14.0f, 0.0f, false);
    halo.addColour (0.82, ember.withAlpha (0.55f));
    g.setGradientFill (halo);
    g.fillRect (tail, footY - 3.0f, length + 14.0f, 7.0f);

    ColourGradient streak (Colours::transparentWhite, tail, 0.0f, Colours::transparentWhite, head + 6.0f, 0.0f, false);
    streak.addColour (0.55, emberLit.withAlpha (0.9f));
    streak.addColour (0.93, Colours::white);
    g.setGradientFill (streak);
    g.fillRoundedRectangle (tail, footY - 0.5f, length + 6.0f, 2.0f, 1.0f);
}

void PitchBoxEditor::paint (Graphics& g)
{
    g.fillAll (night);

    paintHeader (g, getLocalBounds().removeFromTop (kHeaderHeight));

    // Side panel ground.
    auto side = getLocalBounds().withTop (kHeaderHeight).removeFromLeft (178);
    g.setColour (deep);
    g.fillRect (side);
    g.setColour (line);
    g.drawVerticalLine (side.getRight(), (float) side.getY(), (float) side.getBottom());

    // The strip of tools above the grid.
    const auto tools = getLocalBounds().withTop (kHeaderHeight).withTrimmedLeft (179).removeFromTop (kToolbarHeight);
    g.setColour (deep.withAlpha (0.7f));
    g.fillRect (tools);
    g.setColour (line);
    g.drawHorizontalLine (tools.getBottom() - 1, (float) tools.getX(), (float) tools.getRight());
}
void PitchBoxEditor::paintOverChildren (Graphics& g)
{
    if (! fileHovering)
        return;

    const auto area = getLocalBounds().withTrimmedTop (kHeaderHeight).toFloat().reduced (10.0f);

    g.setColour (night.withAlpha (0.78f));
    g.fillRoundedRectangle (area, 14.0f);
    g.setColour (emberLit);
    g.drawRoundedRectangle (area, 14.0f, 2.0f);

    g.setColour (Colours::white);
    g.setFont (capsFont (18.0f));
    g.drawText ("DROP TO IMPORT THE VOCAL", area, Justification::centred, false);
}

bool PitchBoxEditor::isInterestedInFileDrag (const StringArray& files)
{
    // Only where the plugin holds the take itself. Under ARA the host does.
    return transferMode && droppedAudioFile (files) != File();
}

void PitchBoxEditor::fileDragEnter (const StringArray&, int, int)
{
    fileHovering = true;
    repaint();
}

void PitchBoxEditor::fileDragExit (const StringArray&)
{
    fileHovering = false;
    repaint();
}

void PitchBoxEditor::filesDropped (const StringArray& files, int, int)
{
    fileHovering = false;
    repaint();

    if (const auto file = droppedAudioFile (files); file != File())
        processor.getTransfer().importFile (file);
}

void PitchBoxEditor::resized()
{
    auto bounds = getLocalBounds();
    auto header = bounds.removeFromTop (kHeaderHeight);

    // Controls sit in the upper part of the header, clear of the light that
    // runs along its foot.
    const auto row = header.withTrimmedBottom (6).withSizeKeepingCentre (header.getWidth(), 30);
    auto right = row;

    enabledButton.setBounds (right.removeFromRight (150).withTrimmedRight (14).withTrimmedLeft (0));
    right.removeFromRight (8);

    if (transferMode)
        transferButton.setBounds (right.removeFromRight (122));

    auto history = row.withTrimmedLeft (titleRight() + 56).withWidth (234);

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
    // smallest window size. Each is a name, the dial, and its readout.
    const auto knobHeight = jlimit (104, 132, side.getHeight() / 4 - 6);

    auto knob = [&side, knobHeight] (Slider& s, Label& l, Label& readout)
    {
        auto area = side.removeFromTop (knobHeight);
        l.setBounds (area.removeFromTop (16));
        readout.setBounds (area.removeFromBottom (22).withSizeKeepingCentre (96, 22));
        s.setBounds (area.reduced (14, 0));
        side.removeFromTop (6);
    };

    knob (amountSlider, amountLabel, amountValue);
    knob (transitionSlider, transitionLabel, transitionValue);
    knob (formantSlider, formantLabel, formantValue);
    knob (modulationSlider, modulationLabel, modulationValue);
    auto tools = bounds.removeFromTop (kToolbarHeight).reduced (10, 5);

    if (transferMode)
    {
        for (auto* b : std::initializer_list<Component*> { &stopButton, &playButton, &loopButton })
        {
            b->setBounds (tools.removeFromLeft (tools.getHeight()));
            tools.removeFromLeft (6);
        }

        tools.removeFromLeft (8);
    }

    midiButton.setBounds (tools.removeFromLeft (126));
    tools.removeFromLeft (10);

    volumeLabel.setBounds (tools.removeFromLeft (66));
    tools.removeFromLeft (6);
    volumeSlider.setBounds (tools.removeFromLeft (120));
    tools.removeFromLeft (8);
    volumeValue.setBounds (tools.removeFromLeft (78).withSizeKeepingCentre (78, 22));

    inspector.setBounds (tools.withTrimmedLeft (10));

    grid.setBounds (bounds);
}

} // namespace onirique
