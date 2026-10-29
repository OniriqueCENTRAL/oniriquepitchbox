#include "PluginEditor.h"
#include "ui/OniriqueWindowSize.h"

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

    constexpr int kBarUnits = 1200;   // the width the bar is drawn for at 100 %
    constexpr int kToolsHeight = 40;
    constexpr int kToolbarHeight = 40;

    int barHeight() { return nuit::Bar::heightFor (kBarUnits); }

    Image sigilImage()
    {
        return ImageCache::getFromMemory (BinaryData::sigil_png, BinaryData::sigil_pngSize);
    }
}

PitchBoxEditor::PitchBoxEditor (PitchBoxProcessor& p)
    : AudioProcessorEditor (&p),
      AudioProcessorEditorARAExtension (&p),
      processor (p),
      bar (p.presets, p.apvts, "PITCHBOX", sigilImage(), kBarUnits, kBarUnits)
{
    setLookAndFeel (&sharedLookAndFeel());

    addAndMakeVisible (grid);
    grid.onEdit = [this]
    {
        pushEdits();

        // A tool may have changed what the knob and the sliders show for the
        // selected notes.
        selectionChanged();
    };

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
    updateValueReadouts();

    amountSlider.onValueChange = [this]
    {
        edits.settings.globalAmount = (float) amountSlider.getValue();
        edits.refreshUntouched();
        pushEdits();
        mirrorSettings();
    };

    transitionSlider.onValueChange = [this]
    {
        edits.settings.transitionSeconds = transitionSlider.getValue();
        edits.bump();
        pushEdits();
        mirrorSettings();
    };

    formantSlider.onValueChange = [this]
    {
        edits.settings.formantRatio = (float) formantSlider.getValue();
        edits.bump();
        pushEdits();
        mirrorSettings();
    };

    // The tools of the note editor. Each one is a segment of the row over
    // the grid, its star lit while it is the one in hand.
    for (const auto& [tool, name] : std::initializer_list<std::pair<NoteGrid::Tool, const char*>> {
             { NoteGrid::Tool::select, "SELECT" },
             { NoteGrid::Tool::modulation, "MODULATION" },
             { NoteGrid::Tool::correction, "CORRECTION" },
             { NoteGrid::Tool::volume, "VOLUME" },
             { NoteGrid::Tool::bypass, "BYPASS" } })
    {
        auto button = std::make_unique<nuit::Segment> (name, true);
        button->onClick = [this, t = tool] { selectTool (t); };
        addAndMakeVisible (*button);
        tools.push_back ({ tool, std::move (button) });
    }

    selectTool (NoteGrid::Tool::select);

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

    // The fine pitch of the selected notes, in cents around their semitone.
    centsSlider.setSliderStyle (Slider::LinearHorizontal);
    centsSlider.setTextBoxStyle (Slider::NoTextBox, false, 0, 0);
    centsSlider.setRange (-50.0, 50.0, 1.0);
    centsSlider.setValue (0.0, dontSendNotification);
    centsSlider.setDoubleClickReturnValue (true, 0.0);
    centsSlider.onDragStart = [this] { beginEdit(); };
    centsSlider.onValueChange = [this]
    {
        const auto selection = grid.getSelection();

        if (selection.empty())
            return;

        if (! centsSlider.isMouseButtonDown())
            beginEdit();

        if (centsBases.size() != edits.notes.size())
            takeCentsBases();

        edits.touched.resize (edits.notes.size(), false);

        for (const auto index : selection)
        {
            const auto i = (size_t) index;

            if (i >= edits.notes.size())
                continue;

            auto& n = edits.notes[i];
            n.targetCents = centsBases[i] + (float) centsSlider.getValue();

            // As with a drag: a note tuned by hand is a note the user wants
            // moved, so it gets full correction unless it was dialled back.
            if (! edits.touched[i])
                n.correctAmount = 1.0f;

            edits.touched[i] = true;
        }

        edits.bump();
        pushEdits();
        updateInspector();
        grid.repaint();
    };
    addAndMakeVisible (centsSlider);

    centsLabel.setText ("CENTS", dontSendNotification);
    centsLabel.setJustificationType (Justification::centredRight);
    centsLabel.setColour (Label::textColourId, mist);
    addAndMakeVisible (centsLabel);

    centsValue.getProperties().set ("pill", true);
    centsValue.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (centsValue);

    inspector.setJustificationType (Justification::centredRight);
    inspector.setColour (Label::textColourId, Colour (0xffe9c9cc));
    inspector.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (inspector);

    // What each control is, for the pointer that rests on it. The name and the
    // readout of a knob are part of the knob.
    describe ({ &rootBox }, "Tonalité : la note sur laquelle la gamme est construite.");
    describe ({ &scaleBox }, "Gamme : les notes vers lesquelles la correction attire la voix.");
    describe ({ &detectKeyButton }, "Détecter : règle la tonalité et la gamme sur celles trouvées dans la prise.");
    describe ({ &amountSlider, &amountLabel, &amountValue },
              "Amount : à quel point les notes non retouchées sont ramenées sur la gamme. Double-clic pour remettre à zéro.");
    describe ({ &transitionSlider, &transitionLabel, &transitionValue },
              "Speed : le temps que met la hauteur à glisser d'une note à la suivante. À 0, le saut est franc.");
    describe ({ &formantSlider, &formantLabel, &formantValue },
              "Formant : change la couleur de la voix sans toucher à sa hauteur.");
    describe ({ &volumeSlider, &volumeLabel, &volumeValue },
              "Volume des notes sélectionnées, en dB. Double-clic pour remettre à zéro.");
    describe ({ &centsSlider, &centsLabel, &centsValue },
              "Cents : accorde finement les notes sélectionnées, de -50 à +50 cents autour de leur demi-ton. Double-clic pour les remettre juste.");
    describe ({ &undoButton }, "Annuler la dernière modification.");
    describe ({ &redoButton }, "Rétablir la modification annulée.");
    describe ({ &resetButton },
              "Reset : efface toutes les retouches faites à la main et revient aux notes analysées. La tonalité et les potards sont conservés.");
    describe ({ &transferButton },
              "Transfer : arme l'enregistrement, puis lance la lecture du passage dans le DAW pour le faire entrer dans le plugin.");
    describe ({ &stopButton }, "Stop. Une seconde fois : retour au début.");
    describe ({ &playButton }, "Lire la prise depuis ce point, sans passer par le DAW.");
    describe ({ &loopButton }, "Boucle : tourne sur les notes sélectionnées, ou sur toute la prise.");
    describe ({ &midiButton }, "Export MIDI : enregistre les notes, telles qu'elles sonneront, dans un fichier MIDI.");
    describe ({ &inspector },
              "La note sélectionnée : ce qui a été chanté, son écart en cents, sa destination et sa durée.");

    const char* toolTexts[] = {
        "Sélection : clic sur une note pour la choisir, cadre sur la grille pour en prendre plusieurs ; glisser une note la monte ou la descend sur la gamme (Alt : librement). Double-clic : la note sort de la correction.",
        "Modulation : glisser une note vers le haut ou le bas règle ce qu'elle garde de son vibrato, de ses attaques et de ses dérives, de 0 % (un trait droit) à 200 % (exagéré). Le trait se redessine pendant qu'on glisse. Maj : plus fin, double-clic : 100 %.",
        "Correction : glisser une note règle à quel point elle est tirée vers sa note cible, de 0 % (telle que chantée) à 100 %. Maj : plus fin, double-clic : 100 %.",
        "Volume : glisser une note la monte ou la baisse, de -24 à +12 dB. Maj : plus fin, double-clic : 0 dB.",
        "Bypass : un clic sort la note de la correction, un autre l'y remet. Pour un glissé ou une note bleue voulus.",
    };

    for (size_t i = 0; i < tools.size(); ++i)
        describe ({ tools[i].button.get() }, toolTexts[i]);

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
    // Small enough to fit the strip a host docks an ARA editor in, and with no
    // real ceiling: a host that stretches the editor across a wide screen must
    // not be stopped short and left with a bare band beside it.
    setResizeLimits (900, 500, 8000, 4000);
    // taille d'ouverture : la règle commune des plugins ONIRIQUE (ui/OniriqueWindowSize.h)
    const auto opening = onirique::window::openingSize ({}, "ONIRIQUE PITCHBOX", { 1200, 770 }, { 900, 500 }, { 8000, 4000 });
    setSize (opening.x, opening.y);

    // The bar goes in last but for the licence: the panels it opens lie over
    // everything else in the window.
    addAndMakeVisible (bar);
    mirrored = parameterValues();

    refreshTarget();
    // The licence pill and the activation page, over everything else.
    licenseOverlay = std::make_unique<license::Overlay> ("ONIRIQUE PITCHBOX", processor.license);
    addAndMakeVisible (*licenseOverlay);
    resized();

    startTimerHz (30);
}

PitchBoxEditor::~PitchBoxEditor()
{
    onirique::window::rememberOnThisComputer ("ONIRIQUE PITCHBOX", getWidth(), getHeight());
    analysisQueue->removeListener (this);
    processor.getTransfer().removeChangeListener (this);
    processor.getTransfer().stopAudition();

    if (araController != nullptr)
        araController->getAudition().release();

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

    araController = specialisation;

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

    if (currentModification != nullptr)
        syncControls();
}

void PitchBoxEditor::pushEdits()
{
    if (transferMode)
        processor.getTransfer().setEdits (edits);
    else if (currentModification != nullptr)
    {
        currentModification->setEdits (edits);

        // A note being held while it is edited has to be heard changing.
        rebuildAraAudition();
    }
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

    for (auto* c : std::initializer_list<Component*> { &volumeSlider, &volumeLabel, &volumeValue,
                                                       &centsSlider, &centsLabel, &centsValue })
    {
        c->setEnabled (usable);
        c->setAlpha (usable ? 1.0f : 0.4f);
    }

    volumeSlider.setValue (usable && (size_t) selection.front() < edits.notes.size()
                               ? edits.notes[(size_t) selection.front()].gainDb
                               : 0.0f,
                           dontSendNotification);

    takeCentsBases();
    updateInspector();
}

void PitchBoxEditor::takeCentsBases()
{
    centsBases.resize (edits.notes.size());

    for (size_t i = 0; i < edits.notes.size(); ++i)
        centsBases[i] = std::round (edits.notes[i].targetCents / 100.0f) * 100.0f;

    const auto selection = grid.getSelection();
    auto shown = 0.0f;

    if (! selection.empty() && (size_t) selection.front() < edits.notes.size())
    {
        const auto i = (size_t) selection.front();
        shown = edits.notes[i].targetCents - centsBases[i];
    }

    centsSlider.setValue (shown, dontSendNotification);
    updateValueReadouts();
}

void PitchBoxEditor::describe (std::initializer_list<Component*> components, const char* utf8Text)
{
    const auto text = String::fromUTF8 (utf8Text);

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

    // Onto another control, or off them all: the description goes, and the
    // wait starts again.
    if (over != resting)
    {
        resting = over;
        restingAt = at;
        restingSince = now;
        hoverTip.setVisible (false);
        return;
    }

    // Once it is up it stays up for as long as the pointer is on that control,
    // wherever on it the pointer goes.
    if (resting == nullptr || hoverTip.isVisible())
        return;

    // Until then, the pointer has to rest: moving it starts the wait again.
    if (at.getDistanceFrom (restingAt) > 3)
    {
        restingAt = at;
        restingSince = now;
        return;
    }

    if (now - restingSince < 2000)
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
    if (! transferMode)
    {
        auditionAra (index, position);
        return;
    }

    auto& session = processor.getTransfer();

    if (index < 0 || (size_t) index >= edits.notes.size())
    {
        session.stopAudition();
        return;
    }

    const auto& n = edits.notes[(size_t) index];
    session.startAudition (n.startSample, n.endSample, position);
}

void PitchBoxEditor::auditionAra (int index, int64_t position)
{
    if (araController == nullptr)
        return;

    auto& audition = araController->getAudition();

    if (index < 0 || (size_t) index >= edits.notes.size() || currentSource == nullptr || currentModification == nullptr)
    {
        araAuditionNote = -1;
        audition.release();
        return;
    }

    const auto analysis = currentSource->getAnalysis();
    const auto rate = currentSource->getSampleRate();
    const auto takeLength = (int64) currentSource->getSampleCount();
    const auto channels = jlimit (1, 2, currentSource->getChannelCount());

    if (analysis == nullptr || rate <= 0.0 || takeLength <= 0)
        return;

    // The take around the note, with room for the clip and for the renderer's
    // context, read once now rather than on every step of the drag.
    const auto& n = edits.notes[(size_t) index];
    const auto margin = AuditionBuilder::marginSamples (rate) + jmax (analysis->paddingFor (1.0f), analysis->paddingFor (1.4f),
                                                                      analysis->paddingFor (0.7f));
    const auto first = n.startSample - margin;
    const auto length = jmin ((int64) (120.0 * rate), n.endSample - n.startSample + 2 * margin);

    araAuditionWindow.setSize (channels, (int) length, false, false, true);
    araAuditionWindow.clear();

    ARAAudioSourceReader reader (currentSource);

    if (! reader.isValid())
        return;

    reader.read (&araAuditionWindow, 0, (int) length, first, true, channels > 1);

    araAuditionWindowStart = first;
    araAuditionNote = index;
    araAuditionCentre = position;
    araAuditionBuilder.startNewNote();

    rebuildAraAudition();
}

void PitchBoxEditor::rebuildAraAudition()
{
    if (araAuditionNote < 0 || (size_t) araAuditionNote >= edits.notes.size()
        || araController == nullptr || currentSource == nullptr || currentModification == nullptr)
        return;

    AuditionTake take;
    take.rate = currentSource->getSampleRate();
    take.numSamples = (int64) currentSource->getSampleCount();
    take.firstSample = araAuditionWindowStart;
    take.length = araAuditionWindow.getNumSamples();

    for (int c = 0; c < araAuditionWindow.getNumChannels(); ++c)
        take.channels.push_back (araAuditionWindow.getReadPointer (c));

    const auto& n = edits.notes[(size_t) araAuditionNote];
    auto clip = araAuditionBuilder.build (take, currentSource->getAnalysis(), edits,
                                          n.startSample, n.endSample, araAuditionCentre);

    araController->getAudition().hold (currentModification, std::move (clip), take.rate);
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
    updateValueReadouts();
    mirrorSettings();
}

std::array<float, 4> PitchBoxEditor::parameterValues() const
{
    auto value = [this] (const char* id) { return processor.apvts.getRawParameterValue (id)->load(); };
    return { value ("amount"), value ("speed"), value ("formant"), value ("bypass") };
}

void PitchBoxEditor::mirrorSettings()
{
    const auto& s = edits.settings;
    const std::array<std::pair<const char*, float>, 4> wanted { { { "amount", s.globalAmount },
                                                                  { "speed", (float) s.transitionSeconds },
                                                                  { "formant", s.formantRatio },
                                                                  { "bypass", s.enabled ? 0.0f : 1.0f } } };

    for (const auto& [id, value] : wanted)
    {
        auto* parameter = processor.apvts.getParameter (id);
        const auto normalised = parameter->convertTo0to1 (value);

        if (std::abs (parameter->getValue() - normalised) > 1.0e-5f)
            parameter->setValueNotifyingHost (normalised);
    }

    mirrored = parameterValues();
}

void PitchBoxEditor::followParameters()
{
    const auto now = parameterValues();

    if (now == mirrored)
        return;

    mirrored = now;

    auto& s = edits.settings;
    const auto enabled = now[3] < 0.5f;

    if (approximatelyEqual (s.globalAmount, now[0]) && approximatelyEqual ((float) s.transitionSeconds, now[1])
        && approximatelyEqual (s.formantRatio, now[2]) && s.enabled == enabled)
        return;

    // A preset, the other side of the A/B, IN, or the host: the take follows,
    // and the change can be undone like any other.
    beginEdit();
    s.globalAmount = now[0];
    s.transitionSeconds = now[1];
    s.formantRatio = now[2];
    s.enabled = enabled;
    edits.refreshUntouched();
    pushEdits();

    amountSlider.setValue (s.globalAmount, dontSendNotification);
    transitionSlider.setValue (s.transitionSeconds, dontSendNotification);
    formantSlider.setValue (s.formantRatio, dontSendNotification);
    updateValueReadouts();
    grid.repaint();
}

void PitchBoxEditor::selectTool (NoteGrid::Tool tool)
{
    grid.setTool (tool);

    for (auto& choice : tools)
        choice.button->setToggleState (choice.tool == tool, dontSendNotification);
}

void PitchBoxEditor::showStatus (const String& headerText, const String& gridText, float newGridProgress)
{
    if (headerText != status)
    {
        status = headerText;
        repaint (statusArea);
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

    const auto ct = roundToInt (centsSlider.getValue());
    centsValue.setText ((ct > 0 ? "+" : "") + String (ct) + " ct", dontSendNotification);

    auto percent = [] (double v) { return String (roundToInt (v * 100.0)) + " %"; };

    amountValue.setText (percent (amountSlider.getValue()), dontSendNotification);
    transitionValue.setText (String (roundToInt (transitionSlider.getValue() * 1000.0)) + " ms", dontSendNotification);
    formantValue.setText (String (formantSlider.getValue(), 2), dontSendNotification);
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

        // The counts only when something is wrong: they change on every
        // block, and there is no point redrawing them while all is well.
        if (Report::problem.load() != Report::none)
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
    auto playing = processor.getTransport (song);

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
        // Under ARA the position comes from whichever instance is rendering,
        // which need not be this one.
        using Report = AraPlaybackReport;

        if (Report::calls.load() > 0 && Report::hostRate.load() > 0)
        {
            song = Report::playheadSamples.load();
            playing = Time::getMillisecondCounter() - Report::lastPlayingAt.load() < 250;
        }

        const auto sessionRate = Report::hostRate.load() > 0 ? (double) Report::hostRate.load()
                                                             : processor.getSampleRate();
        // Song time to take time, through whichever region of this take the
        // transport is in.
        for (auto* region : currentModification->getPlaybackRegions())
        {
            const auto range = region->getSampleRange (sessionRate,
                                                       ARAPlaybackRegion::IncludeHeadAndTail::no);

            if (range.contains (song))
            {
                // The take counts in samples of its own rate, not the session's.
                const auto takeRate = currentSource != nullptr ? currentSource->getSampleRate() : 0.0;
                const auto step = takeRate > 0.0 && sessionRate > 0.0 ? takeRate / sessionRate : 1.0;

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

    followParameters();

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
        syncControls();
    }
}

//==============================================================================
void PitchBoxEditor::paint (Graphics& g)
{
    g.fillAll (night);

    // Side panel ground.
    auto side = getLocalBounds().withTop (barHeight()).removeFromLeft (178);
    g.setColour (deep);
    g.fillRect (side);
    g.setColour (line);
    g.drawVerticalLine (side.getRight(), (float) side.getY(), (float) side.getBottom());

    // The two strips above the grid: the tools, then what the selected notes are set to.
    const auto strips = getLocalBounds().withTop (barHeight()).withTrimmedLeft (179).removeFromTop (kToolsHeight + kToolbarHeight);
    g.setColour (deep.withAlpha (0.7f));
    g.fillRect (strips);
    g.setColour (line);
    g.drawHorizontalLine (strips.getY() + kToolsHeight - 1, (float) strips.getX(), (float) strips.getRight());
    g.drawHorizontalLine (strips.getBottom() - 1, (float) strips.getX(), (float) strips.getRight());

    nuit::drawWell (g, toolWell.toFloat());

    // What the take is and what playback is doing, at the end of the tools.
    g.setColour (Colour (0xffe9c9cc));
    g.setFont (monoFont (11.0f));
    g.drawText (status, statusArea, Justification::centredRight, true);
}
void PitchBoxEditor::paintOverChildren (Graphics& g)
{
    if (! fileHovering)
        return;

    const auto area = getLocalBounds().withTrimmedTop (barHeight()).toFloat().reduced (10.0f);

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

    // The bar keeps its height and takes the whole width; the licence badge
    // has its place in it, beside the menu.
    bar.setUnitsWidth (getWidth());
    bar.setPillWidth (licenseOverlay != nullptr ? licenseOverlay->pillWidth() : 0);
    bar.place (1.0f, getLocalBounds());

    if (licenseOverlay != nullptr)
        licenseOverlay->fit (getLocalBounds(), 1.0f, bar.getPillArea(), Justification::centredRight);

    bounds.removeFromTop (barHeight());

    auto side = bounds.removeFromLeft (178).reduced (14, 16);

    side.removeFromTop (4);

    // Without ARA the take comes in first: TRANSFER heads the panel.
    if (transferMode)
    {
        transferButton.setBounds (side.removeFromTop (30));
        side.removeFromTop (14);
    }

    rootBox.setBounds (side.removeFromTop (30));
    side.removeFromTop (8);
    scaleBox.setBounds (side.removeFromTop (30));
    side.removeFromTop (8);
    detectKeyButton.setBounds (side.removeFromTop (30));
    side.removeFromTop (14);

    // Three knobs share whatever height is left, so the panel still fits at the
    // smallest window size. Each is a name, the dial, and its readout.
    const auto knobHeight = jlimit (62, 132, side.getHeight() / 3 - 6);

    // In a low window there is no room for the readouts under the dials; the
    // dials stay, and the value is still there in the description.
    const auto showReadouts = knobHeight >= 100;

    auto knob = [&side, knobHeight, showReadouts] (Slider& s, Label& l, Label& readout)
    {
        auto area = side.removeFromTop (knobHeight);
        l.setBounds (area.removeFromTop (16));

        readout.setVisible (showReadouts);

        if (showReadouts)
            readout.setBounds (area.removeFromBottom (22).withSizeKeepingCentre (96, 22));

        s.setBounds (area.reduced (14, 0));
        side.removeFromTop (6);
    };

    knob (amountSlider, amountLabel, amountValue);
    knob (transitionSlider, transitionLabel, transitionValue);
    knob (formantSlider, formantLabel, formantValue);

    // The tools, then undo, redo and reset, then the status.
    auto toolRow = bounds.removeFromTop (kToolsHeight).reduced (10, 5);
    {
        int total = 4 + 2 * ((int) tools.size() - 1);

        for (const auto& choice : tools)
            total += choice.button->preferredWidth();

        toolWell = toolRow.removeFromLeft (total);
        auto inner = toolWell.reduced (2);

        for (auto& choice : tools)
        {
            choice.button->setBounds (inner.removeFromLeft (choice.button->preferredWidth()));
            inner.removeFromLeft (2);
        }
    }

    toolRow.removeFromLeft (12);

    for (auto* b : { &undoButton, &redoButton, &resetButton })
    {
        b->setBounds (toolRow.removeFromLeft (74));
        toolRow.removeFromLeft (6);
    }

    statusArea = toolRow.withTrimmedLeft (8);

    auto strip = bounds.removeFromTop (kToolbarHeight).reduced (10, 5);

    if (transferMode)
    {
        for (auto* b : std::initializer_list<Component*> { &stopButton, &playButton, &loopButton })
        {
            b->setBounds (strip.removeFromLeft (strip.getHeight()));
            strip.removeFromLeft (6);
        }

        strip.removeFromLeft (8);
    }

    midiButton.setBounds (strip.removeFromLeft (126));
    strip.removeFromLeft (10);

    // Volume and cents share the room left, so both still fit in the
    // narrowest window; the sliders give way first.
    const auto sliderWidth = jlimit (60, 120, (strip.getWidth() - (66 + 6 + 8 + 74) - (12 + 54 + 6 + 8 + 66) - 160) / 2);

    volumeLabel.setBounds (strip.removeFromLeft (66));
    strip.removeFromLeft (6);
    volumeSlider.setBounds (strip.removeFromLeft (sliderWidth));
    strip.removeFromLeft (8);
    volumeValue.setBounds (strip.removeFromLeft (74).withSizeKeepingCentre (74, 22));

    strip.removeFromLeft (12);
    centsLabel.setBounds (strip.removeFromLeft (54));
    strip.removeFromLeft (6);
    centsSlider.setBounds (strip.removeFromLeft (sliderWidth));
    strip.removeFromLeft (8);
    centsValue.setBounds (strip.removeFromLeft (66).withSizeKeepingCentre (66, 22));

    inspector.setBounds (strip.withTrimmedLeft (10));

    grid.setBounds (bounds);
}

} // namespace onirique
