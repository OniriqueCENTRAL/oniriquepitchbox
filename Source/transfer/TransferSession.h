// Onirique PitchBox — transfer mode, for hosts without ARA
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include "../model/Analysis.h"
#include "../model/EditState.h"
#include "../model/RegionRenderer.h"

namespace onirique
{

using namespace juce;

//==============================================================================
/** A passage recorded from the track, and where on the timeline it belongs.

    Published whole and never modified afterwards, for the same reason Analysis
    is: the audio thread reads it while the message thread may be replacing it.
    Each channel carries `padding` samples of silence either side of the take,
    so the renderer always has the source context it asks for.
*/
struct TransferTake
{
    using Ptr = std::shared_ptr<const TransferTake>;

    double sampleRate  = 48000.0;
    int64  startInSong = 0;
    int64  numSamples  = 0;
    int64  padding     = 0;

    std::vector<std::vector<float>> channels;
};

//==============================================================================
/** What a held note sounds like: one cycle of the corrected voice, frozen.

    A voiced note is held the way a wavetable holds a tone. One pitch period is
    lifted out of the corrected audio, resampled into a table, and read round
    and round at exactly the rate that gives the period back, so the pitch
    heard is the pitch the note has. Anything longer than one period would be
    heard for what it is, a loop.

    Built off the audio thread, so that all the audio thread does is read it.
*/
struct AuditionClip
{
    using Ptr = std::shared_ptr<const AuditionClip>;

    int64 length = 0;

    /** Table samples per output sample. */
    double increment = 1.0;

    std::vector<std::vector<float>> channels;
};
//==============================================================================
/** What the plugin does in a host that has no ARA.

    ARA is how a host hands over a whole take up front. Without it the plugin
    only ever sees the audio going past, so it has to be shown the passage
    first: the user arms a transfer, plays the passage once, and the plugin
    records it together with its position on the timeline. From then on it has
    what ARA would have given it — the whole take — and plays its own corrected
    copy back in place of the track whenever the playhead is inside that range.

    The take, its analysis and the edits are shared with the audio thread the
    same way throughout: published under a lock the audio thread only ever
    tries, and picked up at the top of a block when the revision has moved.
*/
class TransferSession final : public ChangeBroadcaster,
                              private Thread,
                              private Timer
{
public:
    enum class State { idle, armed, recording, finishing };

    TransferSession();
    ~TransferSession() override;

    //==============================================================================
    /** Call from prepareToPlay. */
    void prepare (double sampleRate, int maxBlockSamples, int numChannels);

    /** Audio thread. Records the block when a transfer is running, and
        otherwise replaces it with the corrected take wherever the two overlap.
        Outside the take the buffer is left as it arrived. */
    void process (AudioBuffer<float>&, const Optional<AudioPlayHead::PositionInfo>&) noexcept;

    //==============================================================================
    /** Arms a transfer, cancels an armed one, or stops a running one. Recording
        starts by itself when the host starts playing and ends when it stops. */
    void toggleTransfer();

    State getState() const noexcept                 { return state.load(); }
    double getRecordedSeconds() const noexcept;

    bool isAnalysing() const noexcept               { return analysing.load(); }
    float getAnalysisProgress() const noexcept      { return analysisProgress.load(); }

    /** Sample rate of the current take, or 0 when there is none. A take is only
        played back at the rate it was recorded at. */
    double getTakeSampleRate() const;

    /** How many blocks arrived at a position that did not quite follow on from
        the one before, and the furthest any was out, in samples. Zero in a host
        that reports its position to the sample. */
    int getPositionSlips() const noexcept           { return positionSlips.load(); }
    int getWorstPositionSlip() const noexcept       { return worstPositionSlip.load(); }

    /** Song position of the first sample of the take; false when there is none. */
    bool getTakeStartInSong (int64& start) const;
    double getHostSampleRate() const noexcept       { return sampleRate; }

    pb::Analysis::Ptr getAnalysis() const;

    /** Holds a note: sustains the corrected sound of the take around `centre`,
        inside [noteStart, noteEnd), for as long as it is wanted and while the
        transport is stopped. It follows the edits like everything else, so a
        note being dragged is heard moving. */
    void startAudition (int64 noteStart, int64 noteEnd, int64 centre);
    void stopAudition();
    pb::EditState getEdits() const;
    uint64_t getEditVersion() const;
    void setEdits (pb::EditState);

    //==============================================================================
    /** The take has no other home than the plugin state: unlike ARA, where the
        host owns the audio, nothing else in the session knows what was
        recorded. So the audio itself is saved, losslessly compressed. */
    void saveState (MemoryBlock&);
    void restoreState (const void* data, int sizeInBytes);

private:
    void run() override;
    void timerCallback() override;

    void resetCapture();
    void drainFifo();
    void finishCapture();
    bool pushToFifo (const AudioBuffer<float>&) noexcept;
    /** `playingAt` is where in the take playback is, when it is playing. */
    void refreshAudioState (bool playing, int64 playingAt = 0) noexcept;
    void renderAudition (AudioBuffer<float>&) noexcept;
    bool rebuildAuditionClip();
    void applyEdits (pb::EditState);

    /** Replaces the take and starts analysing the new one. */
    void install (TransferTake::Ptr newTake,
                  const pb::EditState* replacementEdits,
                  std::shared_ptr<const MemoryBlock> encodedAudio);

    std::shared_ptr<const MemoryBlock> encodedAudioFor (const TransferTake::Ptr&);

    //==============================================================================
    // Shared between threads, guarded by sharedLock.
    mutable SpinLock sharedLock;
    TransferTake::Ptr take, retiredTake;
    pb::Analysis::Ptr analysis, retiredAnalysis;
    pb::EditState edits;
    AuditionClip::Ptr auditionClip;
    uint64_t editCounter = 1;
    std::atomic<uint64_t> revision { 1 };

    std::atomic<State> state { State::idle };
    std::atomic<bool>  stopRequested { false };
    std::atomic<bool>  analysing { false };
    std::atomic<float> analysisProgress { 0.0f };
    std::atomic<int64> captureStart { 0 };
    std::atomic<int64> recordedSamples { 0 };

    std::atomic<bool>  auditionOn { false };
    std::atomic<int>   positionSlips { 0 }, worstPositionSlip { 0 };

    // Where the held note is, and the clips made for it so far. Clips are
    // kept until the next note is held, so that the last reference to one is
    // never dropped on the audio thread.
    CriticalSection auditionLock;
    pb::RegionRenderer auditionRenderer;
    pb::Analysis::Ptr auditionRendererAnalysis;
    int64 auditionNoteStart = 0, auditionNoteEnd = 0, auditionCentre = 0;
    std::vector<AuditionClip::Ptr> auditionClips, olderAuditionClips;

    // Capture: the audio thread writes the fifo, the timer empties it.
    AbstractFifo fifo { 1 };
    AudioBuffer<float> fifoBuffer;
    int64 expectedTime = 0;

    // Playback: where the next block should begin if the transport just runs on.
    int64 expectedPlayTime = 0;
    bool  wasPlaying = false;

    CriticalSection captureLock;
    std::vector<std::vector<float>> captured;
    double captureRate = 48000.0;
    int stopTicks = 0;

    // Audio thread only.
    pb::RegionRenderer renderer;
    TransferTake::Ptr audioTake;
    pb::Analysis::Ptr audioAnalysis;
    pb::EditState audioEdits;
    uint64_t audioRevision = 0;

    /** The corrected take, rendered a slice ahead of where it is being played.

        The renderer's cost is per call, not per sample: it walks back to the
        last phase anchor every time, however little it is then asked for. A
        host that hands out blocks of a few samples would pay that walk
        hundreds of times over for the same stretch of audio. So the take is
        rendered in slices of a fixed size, and the host's blocks, whatever
        their size, are copied out of the current slice. */
    void fillRenderSlice (int64 from) noexcept;

    AudioBuffer<float> renderSlice;
    int64 sliceStart = 0, sliceLength = 0;

    /** An edit made during playback is eased into rather than cut to.

        Two renderings of the same passage do not line up sample for sample:
        the grains fall in different places, so going straight from one to the
        other leaves a step in the waveform, and a knob being turned does that
        dozens of times a second. So when the edits change, what was being
        played goes on being rendered alongside the new version for a moment,
        and the one is faded into the other. Newer edits wait for the fade to
        finish, which is also what keeps a fast drag down to a rate the ear
        hears as movement rather than as a buzz. */
    void fillFadeSlice (int64 from) noexcept;

    pb::RegionRenderer fadeRenderer;
    pb::EditState fadeEdits;
    AudioBuffer<float> fadeSlice;
    int64 fadeSliceStart = 0, fadeSliceLength = 0;
    int fadeRemaining = 0, fadeLength = 1;

    /** One clip being played. Two of them, so that a new clip can fade in
        while the one it replaces fades out. */
    struct AuditionVoice
    {
        AuditionClip::Ptr clip;
        double position = 0.0;
        float gain = 0.0f;
    };

    void mixAudition (AuditionVoice&, AudioBuffer<float>&, bool wanted) noexcept;

    AuditionVoice heldVoice, fadingVoice;

    double sampleRate = 0.0;
    int maxBlock = 512;
    int numChannels = 2;

    // The compressed take, kept so that saving twice does not encode twice.
    CriticalSection encodeLock;
    std::shared_ptr<const MemoryBlock> encoded;
    TransferTake::Ptr encodedFor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransferSession)
};

} // namespace onirique
