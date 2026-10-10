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
    double getHostSampleRate() const noexcept       { return sampleRate; }

    pb::Analysis::Ptr getAnalysis() const;

    /** Plays one stretch of the take, corrected, round and round while the
        transport is stopped: what the user hears while holding a note. It
        follows the edits like everything else, so a note being dragged is
        heard moving. */
    void startAudition (int64 firstSample, int64 lastSample);
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
    void refreshAudioState() noexcept;
    void renderAudition (AudioBuffer<float>&) noexcept;

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
    uint64_t editCounter = 1;
    std::atomic<uint64_t> revision { 1 };

    std::atomic<State> state { State::idle };
    std::atomic<bool>  stopRequested { false };
    std::atomic<bool>  analysing { false };
    std::atomic<float> analysisProgress { 0.0f };
    std::atomic<int64> captureStart { 0 };
    std::atomic<int64> recordedSamples { 0 };

    std::atomic<bool>  auditionOn { false };
    std::atomic<int>   auditionSerial { 0 };
    std::atomic<int64> auditionFirst { 0 }, auditionLast { 0 };

    // Capture: the audio thread writes the fifo, the timer empties it.
    AbstractFifo fifo { 1 };
    AudioBuffer<float> fifoBuffer;
    int64 expectedTime = 0;

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

    std::vector<float> auditionScratch;
    int64 auditionPosition = 0;
    int   auditionSeenSerial = 0;
    float auditionGain = 0.0f;

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
