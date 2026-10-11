// Onirique PitchBox — ARA model objects and document controller
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "../model/Analysis.h"
#include "../model/EditState.h"
#include "../model/RegionRenderer.h"

namespace onirique
{

using namespace juce;

//==============================================================================
/** One background thread shared by every reader in the process, kept just above
    normal priority so that filling read-ahead buffers never starves playback
    and never competes with it either. */
class SharedReadThread final : public TimeSliceThread
{
public:
    SharedReadThread() : TimeSliceThread ("PitchBox ARA sample reading")
    {
        startThread (Priority::high);
    }

    ~SharedReadThread() override { stopThread (2000); }
};

//==============================================================================
/** What the playback renderer last did, for the editor to show.

    An ARA host that plays nothing gives no other clue as to why: the renderer
    may never be asked for audio, may be asked for a stretch of the song that
    none of its regions cover, or may have a take it cannot play. These say
    which. Process-wide rather than per instance, because the instance the host
    renders with is often not the one whose window is open.
*/
struct AraPlaybackReport
{
    enum Problem { none = 0, regionNotPrepared, rateMismatch, noRegionAtPlayhead, rendererNotPrepared };

    static inline std::atomic<int> calls { 0 }, playingBlocks { 0 }, renderedBlocks { 0 }, regions { 0 }, problem { none };
    static inline std::atomic<int> takeRate { 0 }, hostRate { 0 };
    static inline std::atomic<float> playheadSeconds { 0.0f }, regionStartSeconds { 0.0f }, regionEndSeconds { 0.0f };
};

//==============================================================================
/** Per-take state: what the analysis found, and what the user did about it.

    ARA gives every audio source an audio modification, and that is the right
    home for edits — the host persists it, clones it when the user duplicates a
    region, and hands it to every renderer that plays the take. Analysis results
    are shared from the audio source below, because they describe the recording
    rather than any one edit of it.
*/
class PitchBoxAudioModification final : public ARAAudioModification
{
public:
    PitchBoxAudioModification (ARAAudioSource* audioSource,
                               ARA::ARAAudioModificationHostRef hostRef,
                               const ARAAudioModification* optionalModificationToClone)
        : ARAAudioModification (audioSource, hostRef, optionalModificationToClone)
    {
        if (auto* other = dynamic_cast<const PitchBoxAudioModification*> (optionalModificationToClone))
        {
            const ScopedLock lock (other->stateLock);
            edits = other->edits;
        }
    }

    /** Snapshot of the edit state for the audio thread. Copying a few hundred
        notes per block would be wasteful, so the renderer keeps its own copy
        and only refreshes it when the version has moved on. */
    uint64_t getEditVersion() const noexcept
    {
        const ScopedLock lock (stateLock);
        return edits.version;
    }

    void copyEditsTo (pb::EditState& destination) const
    {
        const ScopedLock lock (stateLock);
        destination = edits;
    }

    /** Runs `fn` against the edit state and then tells the host that the take
        now sounds different. That notification is what makes an edit audible
        straight away: the host drops whatever it had cached for this take and
        asks us for those samples again. */
    template <typename Fn>
    void modifyEdits (Fn&& fn)
    {
        {
            const ScopedLock lock (stateLock);
            fn (edits);
            edits.bump();
        }

        notifyContentChanged (ARAContentUpdateScopes::samplesAreAffected(), true);
    }

    void setEdits (pb::EditState newEdits)
    {
        {
            const ScopedLock lock (stateLock);
            edits = std::move (newEdits);
            edits.bump();
        }

        notifyContentChanged (ARAContentUpdateScopes::samplesAreAffected(), true);
    }

    pb::EditState getEdits() const
    {
        const ScopedLock lock (stateLock);
        return edits;
    }

private:
    mutable CriticalSection stateLock;
    pb::EditState edits;
};

//==============================================================================
/** An audio source plus its analysis, and the thread that produces it. */
class PitchBoxAudioSource final : public ARAAudioSource
{
public:
    PitchBoxAudioSource (ARADocument* document, ARA::ARAAudioSourceHostRef hostRef)
        : ARAAudioSource (document, hostRef) {}

    pb::Analysis::Ptr getAnalysis() const
    {
        const ScopedLock lock (analysisLock);
        return analysis;
    }

    void setAnalysis (pb::Analysis::Ptr newAnalysis)
    {
        const ScopedLock lock (analysisLock);
        analysis = std::move (newAnalysis);
    }

    std::atomic<bool> analysisInProgress { false };
    std::atomic<float> analysisProgress { 0.0f };

private:
    mutable CriticalSection analysisLock;
    pb::Analysis::Ptr analysis;
};

//==============================================================================
/** Turns a take into pitch data on a background thread.

    ARA hands us the whole recording up front, which is the entire reason this
    plugin can behave like an editor rather than a live effect — but a take can
    be minutes long, so the analysis cannot happen on the message thread and
    certainly not on the audio thread. Each job reads the source through an
    ARAAudioSourceReader, runs the analysis chain, and publishes the result.
*/
class AnalysisQueue final : private Thread
{
public:
    AnalysisQueue() : Thread ("PitchBox analysis") {}

    ~AnalysisQueue() override
    {
        stopThread (4000);
    }

    /** Listener for "an analysis finished", so the editor can redraw. */
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void analysisFinished (PitchBoxAudioSource*) = 0;
        virtual void analysisProgressed (PitchBoxAudioSource*, float) {}
    };

    void addListener (Listener* l)    { listeners.add (l); }
    void removeListener (Listener* l) { listeners.remove (l); }

    void request (PitchBoxAudioSource* source)
    {
        if (source == nullptr || ! source->isSampleAccessEnabled())
            return;

        {
            const ScopedLock lock (queueLock);

            if (pending.contains (source))
                return;

            pending.add (source);
        }

        if (! isThreadRunning())
            startThread (Priority::low);

        notify();
    }

    void cancel (PitchBoxAudioSource* source)
    {
        const ScopedLock lock (queueLock);
        pending.removeAllInstancesOf (source);

        if (current == source)
            cancelCurrent = true;
    }

    void cancelAll()
    {
        const ScopedLock lock (queueLock);
        pending.clear();
        cancelCurrent = true;
    }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            PitchBoxAudioSource* source = nullptr;

            {
                const ScopedLock lock (queueLock);
                cancelCurrent = false;

                if (! pending.isEmpty())
                {
                    source = pending.removeAndReturn (0);
                    current = source;
                }
            }

            if (source == nullptr)
            {
                wait (200);
                continue;
            }

            analyse (source);

            {
                const ScopedLock lock (queueLock);
                current = nullptr;
            }
        }
    }

    void analyse (PitchBoxAudioSource* source)
    {
        source->analysisInProgress = true;
        source->analysisProgress = 0.0f;

        const ScopeGuard finished { [source] { source->analysisInProgress = false; } };

        const auto numSamples = (int64) source->getSampleCount();
        const auto numChannels = source->getChannelCount();
        const auto sampleRate = source->getSampleRate();

        if (numSamples <= 0 || numChannels <= 0)
            return;

        ARAAudioSourceReader reader (source);

        if (! reader.isValid())
            return;

        // Analysis is monophonic: pitch is a property of the voice, not of the
        // stereo image, and summing to mono is both cheaper and steadier than
        // picking one channel.
        std::vector<float> mono ((size_t) numSamples, 0.0f);

        {
            const int chunk = 1 << 16;
            AudioBuffer<float> buffer (numChannels, chunk);

            for (int64 pos = 0; pos < numSamples; pos += chunk)
            {
                if (threadShouldExit() || isCancelled())
                    return;

                const auto count = (int) jmin ((int64) chunk, numSamples - pos);

                if (! reader.read (&buffer, 0, count, pos, true, true))
                    return;

                for (int c = 0; c < numChannels; ++c)
                {
                    const auto* in = buffer.getReadPointer (c);

                    for (int i = 0; i < count; ++i)
                        mono[(size_t) (pos + i)] += in[i];
                }

                source->analysisProgress = 0.15f * (float) pos / (float) numSamples;
            }

            if (numChannels > 1)
            {
                const auto scale = 1.0f / (float) numChannels;

                for (auto& s : mono)
                    s *= scale;
            }
        }

        pb::AnalysisParams params;
        params.sampleRate = sampleRate;

        auto result = pb::runAnalysis (mono.data(), numSamples, params,
                                       [this, source] (float fraction)
                                       {
                                           source->analysisProgress = 0.15f + 0.85f * fraction;
                                           listeners.call ([source, fraction] (Listener& l)
                                                           { l.analysisProgressed (source, fraction); });
                                           return ! threadShouldExit() && ! isCancelled();
                                       });

        if (result == nullptr)
            return;

        source->setAnalysis (std::move (result));
        source->analysisProgress = 1.0f;

        listeners.call ([source] (Listener& l) { l.analysisFinished (source); });
    }

    bool isCancelled() const
    {
        const ScopedLock lock (queueLock);
        return cancelCurrent;
    }

    mutable CriticalSection queueLock;
    Array<PitchBoxAudioSource*> pending;
    PitchBoxAudioSource* current = nullptr;
    bool cancelCurrent = false;
    ListenerList<Listener> listeners;
};

//==============================================================================
/** Renders corrected audio for the host's playback regions.

    One of these exists per plugin instance that the host asks to play. It owns
    no state the editor can touch: everything it needs arrives either in the
    immutable analysis or in the audio modification's edit state, which it
    re-reads whenever the version number has moved.
*/
class PitchBoxPlaybackRenderer final : public ARAPlaybackRenderer
{
public:
    using ARAPlaybackRenderer::ARAPlaybackRenderer;

    void prepareToPlay (double sampleRateIn,
                        int maximumSamplesPerBlockIn,
                        int numChannelsIn,
                        AudioProcessor::ProcessingPrecision,
                        AlwaysNonRealtime alwaysNonRealtime) override;

    void releaseResources() override;

    bool processBlock (AudioBuffer<float>& buffer,
                       AudioProcessor::Realtime realtime,
                       const AudioPlayHead::PositionInfo& positionInfo) noexcept override;

    using ARAPlaybackRenderer::processBlock;

private:
    struct RegionState
    {
        std::unique_ptr<AudioFormatReader> reader;
        pb::RegionRenderer renderer;
        pb::EditState      edits;
        uint64_t           editVersion = 0;
        pb::Analysis::Ptr  analysis;
    };

    RegionState* stateFor (ARAPlaybackRegion*);
    std::unique_ptr<RegionState> makeState (ARAPlaybackRegion*);

    // A host is free to hand over regions after it has prepared the renderer;
    // one that does must still be played.
    void didAddPlaybackRegion (ARA::PlugIn::PlaybackRegion*) noexcept override;
    void willRemovePlaybackRegion (ARA::PlugIn::PlaybackRegion*) noexcept override;

    bool prepared = false;

    std::map<ARAPlaybackRegion*, std::unique_ptr<RegionState>> regions;
    SharedResourcePointer<SharedReadThread> sharedReadThread;
    AudioBuffer<float> sourceWindow, regionOutput, mixBuffer;
    SharedResourcePointer<AnalysisQueue> analysisQueue;

    double sampleRate = 48000.0;
    int maxBlock = 512;
    int numChannels = 2;
    int64 windowPadding = 0;
    bool buffered = true;
};

//==============================================================================
class PitchBoxDocumentController final : public ARADocumentControllerSpecialisation
{
public:
    using ARADocumentControllerSpecialisation::ARADocumentControllerSpecialisation;

    ~PitchBoxDocumentController() override;

    AnalysisQueue& getAnalysisQueue() noexcept { return *analysisQueue; }

protected:
    ARAAudioSource* doCreateAudioSource (ARADocument*, ARA::ARAAudioSourceHostRef) noexcept override;

    ARAAudioModification* doCreateAudioModification (ARAAudioSource*,
                                                     ARA::ARAAudioModificationHostRef,
                                                     const ARAAudioModification*) noexcept override;

    ARAPlaybackRenderer* doCreatePlaybackRenderer() noexcept override;

    bool doRestoreObjectsFromStream (ARAInputStream&, const ARARestoreObjectsFilter*) noexcept override;
    bool doStoreObjectsToStream (ARAOutputStream&, const ARAStoreObjectsFilter*) noexcept override;

    void didEnableAudioSourceSamplesAccess (ARAAudioSource*, bool enable) noexcept override;
    void willDestroyAudioSource (ARAAudioSource*) noexcept override;
    void doUpdateAudioSourceContent (ARAAudioSource*, ARAContentUpdateScopes) noexcept override;

private:
    SharedResourcePointer<AnalysisQueue> analysisQueue;
};

} // namespace onirique
