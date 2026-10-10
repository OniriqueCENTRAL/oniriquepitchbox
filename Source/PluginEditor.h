// Onirique PitchBox — editor
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "ui/NoteGrid.h"
#include "ui/Theme.h"

namespace onirique
{

class PitchBoxEditor final : public AudioProcessorEditor,
                             public AudioProcessorEditorARAExtension,
                             private AnalysisQueue::Listener,
                             private ChangeListener,
                             private Timer
{
public:
    explicit PitchBoxEditor (PitchBoxProcessor&);
    ~PitchBoxEditor() override;

    void paint (Graphics&) override;
    void resized() override;

private:
    void analysisFinished (PitchBoxAudioSource*) override;
    void analysisProgressed (PitchBoxAudioSource*, float) override;
    void changeListenerCallback (ChangeBroadcaster*) override;
    void timerCallback() override;

    /** Finds the take the user is looking at. */
    void refreshTarget();
    void pushEdits();

    /** The same two jobs in a host without ARA, where the take is one the
        plugin recorded itself. */
    void refreshTransfer();
    void updateTransferStatus();

    void syncControls();
    void showStatus (const String& headerText, const String& gridText, float gridProgress);
    pb::Analysis::Ptr currentAnalysis() const;

    PitchBoxProcessor& processor;

    NoteGrid grid;

    ComboBox rootBox, scaleBox;
    Slider amountSlider, transitionSlider, formantSlider;
    Label amountLabel, transitionLabel, formantLabel;
    TextButton detectKeyButton { "DETECT KEY" };
    TextButton enabledButton { "CORRECTION" };
    TextButton transferButton { "TRANSFER" };

    bool transferMode = false;
    pb::Analysis::Ptr shownAnalysis;
    String gridStatus;
    float gridProgress = 0.0f;

    PitchBoxAudioSource* currentSource = nullptr;
    PitchBoxAudioModification* currentModification = nullptr;
    pb::EditState edits;

    String status { "Waiting for an ARA host" };
    float progress = 0.0f;

    SharedResourcePointer<AnalysisQueue> analysisQueue;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchBoxEditor)
};

} // namespace onirique
