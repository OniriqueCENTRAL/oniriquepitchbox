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
        addAndMakeVisible (s);

        l.setText (name, dontSendNotification);
        l.setJustificationType (Justification::centred);
        l.setColour (Label::textColourId, haze);
        addAndMakeVisible (l);
    };

    setUpKnob (amountSlider, amountLabel, "AMOUNT", 0.0, 1.0, 0.0, "");
    setUpKnob (transitionSlider, transitionLabel, "SPEED", 0.0, 0.25, 0.035, " s");
    setUpKnob (formantSlider, formantLabel, "FORMANT", 0.7, 1.4, 1.0, "");

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
        edits.settings.enabled = enabledButton.getToggleState();
        edits.bump();
        pushEdits();
    };
    addAndMakeVisible (enabledButton);

    detectKeyButton.onClick = [this]
    {
        if (currentSource == nullptr)
            return;

        if (auto a = currentSource->getAnalysis())
        {
            edits.settings.scale = a->key.scale;
            rootBox.setSelectedId (a->key.scale.root + 1, dontSendNotification);
            scaleBox.setSelectedId ((int) a->key.scale.type + 1, dontSendNotification);
            edits.refreshUntouched();
            pushEdits();
        }
    };
    addAndMakeVisible (detectKeyButton);

    analysisQueue->addListener (this);

    setResizable (true, true);
    setResizeLimits (820, 520, 2400, 1500);
    setSize (1160, 720);

    refreshTarget();
    startTimerHz (12);
}

PitchBoxEditor::~PitchBoxEditor()
{
    analysisQueue->removeListener (this);
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
        status = "This plugin needs an ARA host. Load it as an ARA extension on a track.";
        grid.setStatus (status, 0.0f);
        return;
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
    if (currentModification != nullptr)
        currentModification->setEdits (edits);
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
    g.drawText (status, header.withTrimmedRight (170).withTrimmedLeft (330),
                Justification::centredRight, false);

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

    auto side = bounds.removeFromLeft (178).reduced (14, 16);

    side.removeFromTop (4);
    rootBox.setBounds (side.removeFromTop (30));
    side.removeFromTop (8);
    scaleBox.setBounds (side.removeFromTop (30));
    side.removeFromTop (8);
    detectKeyButton.setBounds (side.removeFromTop (30));
    side.removeFromTop (18);

    auto knob = [&side] (Slider& s, Label& l)
    {
        auto area = side.removeFromTop (112);
        s.setBounds (area.removeFromTop (88).reduced (14, 0));
        l.setBounds (area);
        side.removeFromTop (4);
    };

    knob (amountSlider, amountLabel);
    knob (transitionSlider, transitionLabel);
    knob (formantSlider, formantLabel);

    grid.setBounds (bounds);
}

} // namespace onirique
