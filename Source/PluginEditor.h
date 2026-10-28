// Onirique PitchBox — editor
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "License/LicenseOverlay.h"
#include "PluginProcessor.h"
#include "ui/NoteGrid.h"
#include "ui/OniriqueBar.h"
#include "ui/Theme.h"

namespace onirique
{

class PitchBoxEditor final : public AudioProcessorEditor,
                             public AudioProcessorEditorARAExtension,
                             public FileDragAndDropTarget,
                             private AnalysisQueue::Listener,
                             private ChangeListener,
                             private Timer
{
public:
    explicit PitchBoxEditor (PitchBoxProcessor&);
    ~PitchBoxEditor() override;

    void paint (Graphics&) override;
    void paintOverChildren (Graphics&) override;
    void resized() override;

    // Dropping the vocal's file on the window brings it in, in a host that
    // cannot hand it over itself.
    bool isInterestedInFileDrag (const StringArray& files) override;
    void fileDragEnter (const StringArray&, int, int) override;
    void fileDragExit (const StringArray&) override;
    void filesDropped (const StringArray& files, int, int) override;

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

    /** Undo works on whole edit states: they are small, and restoring one is
        the same operation as any other edit. beginEdit() is called before a
        change, never after it. */
    void beginEdit();
    void undo();
    void redo();
    void resetNotes();
    void restore (pb::EditState);
    void clearHistory();
    void updateHistoryButtons();

    void updatePlayhead();
    void updateAraStatus();

    // The plugin's own transport, for a take it holds itself.
    void playLocally();
    void stopLocally();
    void applyLoop();
    void exportMidi();
    void updateInspector();
    void updateValueReadouts();
    void selectionChanged();
    void auditionNote (int index, int64_t position);

    void syncControls();

    /** The presets and the A/B of the bar hold the correction settings as the
        processor's parameters; the take holds them in its edit state. A change
        on either side is carried to the other: a knob or a new take writes the
        parameters, a preset, the A/B or IN write the take. */
    void mirrorSettings();
    void followParameters();
    std::array<float, 4> parameterValues() const;
    std::array<float, 4> mirrored {};

    void selectTool (NoteGrid::Tool);
    void showStatus (const String& headerText, const String& gridText, float gridProgress);
    pb::Analysis::Ptr currentAnalysis() const;

    PitchBoxProcessor& processor;

    /** The bar of the range over everything: the brand, presets, IN, A/B,
        copy and paste, the licence badge and the menu. */
    nuit::Bar bar;

    NoteGrid grid;

    /** The tools of the note editor, in a row over it. */
    struct ToolChoice
    {
        NoteGrid::Tool tool;
        std::unique_ptr<nuit::Segment> button;
    };

    std::vector<ToolChoice> tools;
    Rectangle<int> toolWell, statusArea;

    ComboBox rootBox, scaleBox;
    Slider amountSlider, transitionSlider, formantSlider, modulationSlider;
    Label amountLabel, transitionLabel, formantLabel, modulationLabel;
    Label amountValue, transitionValue, formantValue, modulationValue;
    TextButton undoButton { "UNDO" }, redoButton { "REDO" }, resetButton { "RESET" };
    std::vector<pb::EditState> undoStack, redoStack;
    TextButton detectKeyButton { "DETECT KEY" };
    TextButton transferButton { "TRANSFER" };

    theme::TransportButton stopButton { theme::TransportButton::Kind::stop },
                           playButton { theme::TransportButton::Kind::play },
                           loopButton { theme::TransportButton::Kind::loop };
    TextButton midiButton { "EXPORT MIDI" };
    std::unique_ptr<FileChooser> fileChooser;

    // What is selected, and how loud it is: the per-note controls that live in
    // the strip above the grid rather than among the knobs.
    Slider volumeSlider;
    Label volumeLabel, volumeValue, inspector;

    /** Fine tuning of the selected notes: how many cents each sits above or
        below its semitone. The semitones are taken when the selection or the
        notes change, not on every step of the slider, so a note pushed to
        +50 does not flip to the next semitone and run away. */
    Slider centsSlider;
    Label centsLabel, centsValue;
    std::vector<float> centsBases;
    void takeCentsBases();

    //==============================================================================
    /** The description of whatever the pointer is resting on.

        Shown once the pointer has been still on a control for two full
        seconds, and then left up for as long as the pointer stays on that
        control, moving or not. The stock tooltip window does neither: it shows
        the next one at once after the first, and drops it on any movement.
        A control here can be several components — a knob, its name and its
        readout all say the same thing. */
    struct HoverTip final : Component
    {
        void paint (Graphics& g) override
        {
            getLookAndFeel().drawTooltip (g, text, getWidth(), getHeight());
        }

        String text;
    };

    struct Described
    {
        Component* component;
        String text;
    };

    void describe (std::initializer_list<Component*>, const char* utf8Text);
    void updateHoverTip();

    std::vector<Described> described;
    HoverTip hoverTip;
    const Described* resting = nullptr;
    Point<int> restingAt;
    uint32 restingSince = 0;

    /** Where the plugin's own playback starts from next. */
    int64 localStart = 0;

    bool transferMode = false;
    bool fileHovering = false;
    pb::Analysis::Ptr shownAnalysis;
    String gridStatus;
    float gridProgress = 0.0f;

    PitchBoxAudioSource* currentSource = nullptr;
    PitchBoxAudioModification* currentModification = nullptr;
    pb::EditState edits;

    String status { "Waiting for an ARA host" };

    /** What the take is, without what playback is doing appended. */
    String araTakeStatus;
    float progress = 0.0f;

    SharedResourcePointer<AnalysisQueue> analysisQueue;

    // Last, so that it is destroyed first and drawn over everything else.
    std::unique_ptr<license::Overlay> licenseOverlay;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchBoxEditor)
};

} // namespace onirique
