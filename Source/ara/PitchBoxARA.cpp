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

    sourceWindow.setSize (jmax (1, numChannels), 0);
    regionOutput.setSize (jmax (1, numChannels), 0);
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

    // Everything about the take is in its own rate: the analysis, the edits,
    // the renderer. A block of the session's samples covers this many of its.
    state->takeRate = source->getSampleRate() > 0.0 ? source->getSampleRate() : sampleRate;

    const auto takeBlock = (int) std::ceil ((double) maxBlock * state->takeRate / sampleRate) + 8;
    state->padding = (int64) std::ceil (0.25 * state->takeRate);

    // The buffers are shared between regions, so they are as large as the
    // largest of them needs. Grown here, never on the audio thread.
    const auto windowNeeded = takeBlock + 2 * (int) state->padding;

    if (sourceWindow.getNumSamples() < windowNeeded)
        sourceWindow.setSize (sourceWindow.getNumChannels(), windowNeeded);

    if (regionOutput.getNumSamples() < takeBlock)
        regionOutput.setSize (regionOutput.getNumChannels(), takeBlock);

    state->renderer.prepare (takeBlock, state->takeRate);
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
    using Report = AraPlaybackReport;

    const auto numSamples = buffer.getNumSamples();

    // Not every host fills in the position in samples; the one in seconds says
    // the same thing.
    auto timeInSamples = (int64) 0;

    if (const auto samples = positionInfo.getTimeInSamples())
        timeInSamples = *samples;
    else if (const auto seconds = positionInfo.getTimeInSeconds())
        timeInSamples = (int64) std::llround (*seconds * sampleRate);

    // A playback renderer's output is its regions and nothing else: it starts
    // from silence and each region that is sounding is added in.
    buffer.clear();

    Report::calls.fetch_add (1);

    if (! prepared)
    {
        Report::problem.store (Report::rendererNotPrepared);
        return true;
    }

    if (! positionInfo.getIsPlaying() || numSamples <= 0)
        return true;

    Report::playingBlocks.fetch_add (1);
    Report::regions.store ((int) getPlaybackRegions().size());
    Report::hostRate.store (roundToInt (sampleRate));
    Report::playheadSeconds.store ((float) ((double) timeInSamples / sampleRate));
    Report::playheadSamples.store (timeInSamples);
    Report::lastPlayingAt.store (Time::getMillisecondCounter());

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

            auto* source = region->getAudioModification()->getAudioSource<PitchBoxAudioSource>();

            // Song time to take time. This plugin does not stretch, so the two
            // run at the same speed; but the take counts in samples of its own
            // rate, which need not be the session's. `step` is how many of its
            // samples go by for each one of the session's.
            const auto step = state->takeRate / sampleRate;
            const auto sameRate = approximatelyEqual (step, 1.0);

            const auto takeBegins = (double) region->getStartInAudioModificationSamples();
            const auto takeEnds = (double) region->getEndInAudioModificationSamples();

            // Where the first sample of this slice falls in the take.
            const auto firstPosition = takeBegins + (double) (renderRange.getStart() - regionRange.getStart()) * step;

            // No further than the part of the take the region covers.
            const auto count = jmin ((int) renderRange.getLength(),
                                     (int) std::ceil ((takeEnds - firstPosition) / step));

            if (count <= 0)
                continue;

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

            const auto startInBuffer = done + (int) (renderRange.getStart() - blockRange.getStart());

            // The stretch of the take to render: exactly the slice when the
            // rates agree, otherwise the samples around every position that
            // will be read, with one to spare before and two after.
            const auto takeFirst = sameRate ? (int64) std::llround (firstPosition)
                                            : (int64) std::floor (firstPosition) - 1;
            const auto takeLast = sameRate ? takeFirst + count
                                           : (int64) std::floor (firstPosition + (double) (count - 1) * step) + 3;
            const auto takeCount = (int) (takeLast - takeFirst);

            if (takeCount > regionOutput.getNumSamples())
            {
                success = false;
                continue;
            }

            const auto wanted = (int64) state->renderer.paddingSamples (state->edits.settings.formantRatio);
            const auto padding = jlimit ((int64) 0, state->padding, wanted);

            // Read the take around the block, so the grain machinery has the
            // context it needs on both sides.
            const auto windowStart = takeFirst - padding;
            const auto windowLength = takeCount + 2 * (int) padding;

            sourceWindow.clear (0, windowLength);

            auto* bufferedReader = state->reader.get();

            if (auto* b = dynamic_cast<BufferingAudioReader*> (bufferedReader))
                b->setReadTimeout (realtime == AudioProcessor::Realtime::no ? 100 : 0);

            // A read that comes back short has left silence where the audio
            // was not ready yet. That is played as it is: dropping the whole
            // block for it would mute what did arrive.
            if (! bufferedReader->read (&sourceWindow, 0, windowLength, windowStart, true, true))
                success = false;

            // A mono take on a stereo track reads the same samples into both
            // channels: it is rendered once and that goes to both sides.
            const auto monoTake = source->getChannelCount() == 1;

            for (int c = 0; c < outputChannels; ++c)
            {
                auto* rendered = regionOutput.getWritePointer (monoTake ? 0 : c);

                if (c == 0 || ! monoTake)
                    state->renderer.render (sourceWindow.getReadPointer (c),
                                            windowStart,
                                            windowLength,
                                            state->edits,
                                            takeFirst,
                                            takeLast,
                                            rendered,
                                            takeFirst);

                if (sameRate)
                {
                    buffer.addFrom (c, startInBuffer, rendered, count);
                    continue;
                }

                // Read it out at the session's rate, straight into the mix.
                auto* out = buffer.getWritePointer (c, startInBuffer);

                for (int i = 0; i < count; i += 256)
                {
                    float chunk[256];
                    const auto n = jmin (256, count - i);

                    pb::readAtRate (rendered, firstPosition + (double) i * step - (double) takeFirst, step, chunk, n);

                    for (int k = 0; k < n; ++k)
                        out[i + k] += chunk[k];
                }
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
