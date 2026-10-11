#include "PitchBoxARA.h"

namespace onirique
{

//==============================================================================
void PitchBoxPlaybackRenderer::prepareToPlay (double sampleRateIn,
                                              int maximumSamplesPerBlockIn,
                                              int numChannelsIn,
                                              AudioProcessor::ProcessingPrecision,
                                              AlwaysNonRealtime alwaysNonRealtime)
{
    sampleRate  = sampleRateIn;
    maxBlock    = maximumSamplesPerBlockIn;
    numChannels = numChannelsIn;
    buffered    = alwaysNonRealtime == AlwaysNonRealtime::no;

    regions.clear();

    // The widest source context any region could ask for, so the read window is
    // sized once here rather than grown on the audio thread.
    windowPadding = (int64) std::ceil (0.25 * sampleRate);

    sourceWindow.setSize (jmax (1, numChannels), maxBlock + 2 * (int) windowPadding);
    regionOutput.setSize (jmax (1, numChannels), maxBlock);
    mixBuffer.setSize (jmax (1, numChannels), maxBlock);

    prepared = true;

    for (auto* region : getPlaybackRegions())
        regions.emplace (region, makeState (region));
}

std::unique_ptr<PitchBoxPlaybackRenderer::RegionState> PitchBoxPlaybackRenderer::makeState (ARAPlaybackRegion* region)
{
    auto state = std::make_unique<RegionState>();

    auto* source = region->getAudioModification()->getAudioSource<PitchBoxAudioSource>();
    auto reader = std::make_unique<ARAAudioSourceReader> (source);

    if (buffered)
    {
        const auto readAhead = jmax (4 * maxBlock, roundToInt (2.0 * sampleRate));
        state->reader = std::make_unique<BufferingAudioReader> (reader.release(),
                                                                *sharedReadThread,
                                                                readAhead);
    }
    else
    {
        state->reader = std::move (reader);
    }

    state->renderer.prepare (maxBlock, sampleRate);
    state->analysis = source->getAnalysis();
    state->renderer.setAnalysis (state->analysis);

    if (auto* modification = region->getAudioModification<PitchBoxAudioModification>())
    {
        modification->copyEditsTo (state->edits);
        state->editVersion = state->edits.version;
    }

    return state;
}

void PitchBoxPlaybackRenderer::didAddPlaybackRegion (ARA::PlugIn::PlaybackRegion* region) noexcept
{
    if (prepared)
        if (auto* added = static_cast<ARAPlaybackRegion*> (region))
            regions[added] = makeState (added);
}

void PitchBoxPlaybackRenderer::willRemovePlaybackRegion (ARA::PlugIn::PlaybackRegion* region) noexcept
{
    regions.erase (static_cast<ARAPlaybackRegion*> (region));
}

void PitchBoxPlaybackRenderer::releaseResources()
{
    prepared = false;
    regions.clear();
    sourceWindow.setSize (0, 0);
    regionOutput.setSize (0, 0);
    mixBuffer.setSize (0, 0);
}
PitchBoxPlaybackRenderer::RegionState* PitchBoxPlaybackRenderer::stateFor (ARAPlaybackRegion* region)
{
    const auto it = regions.find (region);
    return it != regions.end() ? it->second.get() : nullptr;
}

bool PitchBoxPlaybackRenderer::processBlock (AudioBuffer<float>& buffer,
                                             AudioProcessor::Realtime realtime,
                                             const AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    const auto numSamples = buffer.getNumSamples();
    const auto timeInSamples = positionInfo.getTimeInSamples().orFallback (0);

    // A playback renderer's output is its regions and nothing else: it starts
    // from silence and each region that is sounding is added in.
    buffer.clear();

    if (! positionInfo.getIsPlaying() || ! prepared || numSamples <= 0)
        return true;

    using Report = AraPlaybackReport;

    Report::playingBlocks.fetch_add (1);
    Report::regions.store ((int) getPlaybackRegions().size());
    Report::hostRate.store (roundToInt (sampleRate));
    Report::playheadSeconds.store ((float) ((double) timeInSamples / sampleRate));

    bool success = true;
    bool renderedAnything = false;
    auto problem = Report::none;

    const auto outputChannels = jmin (buffer.getNumChannels(), sourceWindow.getNumChannels());

    // In slices no longer than the renderer was prepared for, so that a host
    // which sends more than it announced is still played rather than muted.
    for (int done = 0; done < numSamples; done += maxBlock)
    {
        const auto sliceLength = jmin (maxBlock, numSamples - done);
        const auto blockRange = Range<int64>::withStartAndLength (timeInSamples + done, sliceLength);

        for (auto* region : getPlaybackRegions())
        {
            const auto regionRange = region->getSampleRange (sampleRate, ARAPlaybackRegion::IncludeHeadAndTail::no);

            if (done == 0 && region == getPlaybackRegions().front())
            {
                Report::regionStartSeconds.store ((float) ((double) regionRange.getStart() / sampleRate));
                Report::regionEndSeconds.store ((float) ((double) regionRange.getEnd() / sampleRate));
            }

            auto renderRange = blockRange.getIntersectionWith (regionRange);

            if (renderRange.isEmpty())
                continue;

            auto* state = stateFor (region);

            if (state == nullptr || state->reader == nullptr)
            {
                problem = Report::regionNotPrepared;
                success = false;
                continue;
            }

            // Song time to take time. This plugin does not stretch, so the two
            // run at the same rate and differ only by an offset.
            const Range<int64> modificationRange { region->getStartInAudioModificationSamples(),
                                                   region->getEndInAudioModificationSamples() };
            const auto offset = modificationRange.getStart() - regionRange.getStart();

            renderRange = renderRange.getIntersectionWith (modificationRange.movedToStartAt (regionRange.getStart()));

            if (renderRange.isEmpty())
                continue;

            auto* source = region->getAudioModification()->getAudioSource<PitchBoxAudioSource>();

            // A take is only played at the rate it was recorded at. Its channel
            // count is another matter: a mono take on a stereo track is the
            // ordinary case, and the reader spreads it across the channels.
            if (! approximatelyEqual (source->getSampleRate(), sampleRate))
            {
                Report::takeRate.store (roundToInt (source->getSampleRate()));
                problem = Report::rateMismatch;
                success = false;
                continue;
            }

            // Pick up a newly finished analysis, or edits the user just made.
            // Both are plain pointer or version comparisons, so a block in
            // which nothing changed costs nothing here.
            if (auto latest = source->getAnalysis(); latest != state->analysis)
            {
                state->analysis = latest;
                state->renderer.setAnalysis (latest);
            }

            auto* modification = region->getAudioModification<PitchBoxAudioModification>();

            if (modification != nullptr && modification->getEditVersion() != state->editVersion)
            {
                modification->copyEditsTo (state->edits);
                state->editVersion = state->edits.version;
            }

            const auto count = (int) renderRange.getLength();
            const auto startInBuffer = done + (int) (renderRange.getStart() - blockRange.getStart());
            const auto startInTake = renderRange.getStart() + offset;

            const auto wanted = (int64) state->renderer.paddingSamples (state->edits.settings.formantRatio);
            const auto padding = jlimit ((int64) 0, windowPadding, wanted);

            // Read the take around the block, so the grain machinery has the
            // context it needs on both sides.
            const auto windowStart = startInTake - padding;
            const auto windowLength = count + 2 * (int) padding;

            sourceWindow.clear (0, windowLength);

            auto* bufferedReader = state->reader.get();

            if (auto* b = dynamic_cast<BufferingAudioReader*> (bufferedReader))
                b->setReadTimeout (realtime == AudioProcessor::Realtime::no ? 100 : 0);

            // A read that comes back short has left silence where the audio
            // was not ready yet. That is played as it is: dropping the whole
            // block for it would mute what did arrive.
            if (! bufferedReader->read (&sourceWindow, 0, windowLength, windowStart, true, true))
                success = false;

            for (int c = 0; c < outputChannels; ++c)
            {
                state->renderer.render (sourceWindow.getReadPointer (c),
                                        windowStart,
                                        windowLength,
                                        state->edits,
                                        startInTake,
                                        startInTake + count,
                                        regionOutput.getWritePointer (c),
                                        startInTake);

                buffer.addFrom (c, startInBuffer, regionOutput, c, 0, count);
            }

            renderedAnything = true;
        }
    }

    if (renderedAnything)
        Report::renderedBlocks.fetch_add (1);
    else if (problem == Report::none)
        problem = Report::noRegionAtPlayhead;

    Report::problem.store (problem);
    return success;
}
//==============================================================================
PitchBoxDocumentController::~PitchBoxDocumentController()
{
    analysisQueue->cancelAll();
}

ARAAudioSource* PitchBoxDocumentController::doCreateAudioSource (ARADocument* document,
                                                                 ARA::ARAAudioSourceHostRef hostRef) noexcept
{
    return new PitchBoxAudioSource (document, hostRef);
}

ARAAudioModification* PitchBoxDocumentController::doCreateAudioModification (
    ARAAudioSource* audioSource,
    ARA::ARAAudioModificationHostRef hostRef,
    const ARAAudioModification* optionalModificationToClone) noexcept
{
    return new PitchBoxAudioModification (audioSource, hostRef, optionalModificationToClone);
}

ARAPlaybackRenderer* PitchBoxDocumentController::doCreatePlaybackRenderer() noexcept
{
    return new PitchBoxPlaybackRenderer (getDocumentController());
}

void PitchBoxDocumentController::didEnableAudioSourceSamplesAccess (ARAAudioSource* audioSource,
                                                                    bool enable) noexcept
{
    auto* source = static_cast<PitchBoxAudioSource*> (audioSource);

    // The host only lets us read a take while access is enabled, so this is the
    // moment the analysis can start — and the moment it must stop.
    if (enable)
        analysisQueue->request (source);
    else
        analysisQueue->cancel (source);
}

void PitchBoxDocumentController::willDestroyAudioSource (ARAAudioSource* audioSource) noexcept
{
    auto* source = static_cast<PitchBoxAudioSource*> (audioSource);
    analysisQueue->cancel (source);
}

void PitchBoxDocumentController::doUpdateAudioSourceContent (ARAAudioSource* audioSource,
                                                             ARAContentUpdateScopes scopeFlags) noexcept
{
    // The recording itself changed under us — a punch-in, say. The old analysis
    // describes audio that is no longer there, so it has to be redone. The
    // user's edits are matched onto the new notes rather than discarded.
    if (scopeFlags.affectSamples())
    {
        auto* source = static_cast<PitchBoxAudioSource*> (audioSource);
        analysisQueue->cancel (source);
        source->setAnalysis (nullptr);

        if (source->isSampleAccessEnabled())
            analysisQueue->request (source);
    }
}

//==============================================================================
bool PitchBoxDocumentController::doRestoreObjectsFromStream (ARAInputStream& input,
                                                             const ARARestoreObjectsFilter* filter) noexcept
{
    const auto numModifications = input.readInt64();

    for (int64 i = 0; i < numModifications; ++i)
    {
        const auto persistentID = input.readString();
        const auto text = input.readString();

        if (input.failed())
            return false;

        auto* modification = filter->getAudioModificationToRestoreStateWithID<PitchBoxAudioModification> (
            persistentID.toRawUTF8());

        if (modification == nullptr)
            continue;

        modification->setEdits (pb::EditState::fromString (text.toStdString()));
    }

    return ! input.failed();
}

bool PitchBoxDocumentController::doStoreObjectsToStream (ARAOutputStream& output,
                                                         const ARAStoreObjectsFilter* filter) noexcept
{
    std::vector<const PitchBoxAudioModification*> toStore;

    for (const auto& source : filter->getAudioSourcesToStore<PitchBoxAudioSource>())
        for (auto* modification : source->getAudioModifications<PitchBoxAudioModification>())
            toStore.push_back (modification);

    if (! output.writeInt64 ((int64) toStore.size()))
        return false;

    for (const auto* modification : toStore)
    {
        if (! output.writeString (String (modification->getPersistentID())))
            return false;

        if (! output.writeString (String (modification->getEdits().toString())))
            return false;
    }

    return true;
}

} // namespace onirique

//==============================================================================
const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<onirique::PitchBoxDocumentController>();
}
