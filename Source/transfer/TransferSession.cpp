#include "TransferSession.h"

namespace onirique
{

namespace
{
    constexpr int kStateMagic = 0x58425450;   // "PTBX"
    constexpr int kStateVersion = 1;

    constexpr int kCodecRaw = 0;
    constexpr int kCodecFlac = 1;

    /** The capture fifo holds this much audio. The timer empties it twenty
        times a second, so this is how long the message thread may stall before
        a recording has to be cut short. */
    constexpr double kFifoSeconds = 4.0;

    constexpr double kMaxTakeSeconds = 20.0 * 60.0;
    constexpr double kMinTakeSeconds = 0.1;

    /** How far the host's reported position may stray from where the last
        block ended before it counts as a jump. Some hosts derive the sample
        position from musical time and round it, so it is not always exact; a
        loop or a locate is always far larger than this. */
    constexpr int64 kMaxTimeJitter = 4096;

    constexpr int kChunk = 1 << 16;

    /** Compresses a take for the plugin state. FLAC is fixed point, so a take
        that goes over full scale is scaled down to fit and the factor stored
        alongside, rather than clipped. */
    MemoryBlock encodeTake (const TransferTake& take)
    {
        const auto channels = (int) take.channels.size();
        const auto offset = (size_t) take.padding;

        float peak = 0.0f;

        for (const auto& channel : take.channels)
            for (int64 i = 0; i < take.numSamples; ++i)
                peak = jmax (peak, std::abs (channel[offset + (size_t) i]));

        const auto gain = peak > 1.0f ? peak : 1.0f;

        MemoryBlock flac;
        bool flacOk = false;

        {
            std::unique_ptr<OutputStream> stream = std::make_unique<MemoryOutputStream> (flac, false);

            FlacAudioFormat format;
            auto writer = format.createWriterFor (stream,
                                                  AudioFormatWriterOptions()
                                                      .withSampleRate (take.sampleRate)
                                                      .withNumChannels (channels)
                                                      .withBitsPerSample (24));

            if (writer != nullptr)
            {
                AudioBuffer<float> chunk (channels, kChunk);
                flacOk = true;

                for (int64 pos = 0; pos < take.numSamples && flacOk; pos += kChunk)
                {
                    const auto count = (int) std::min ((int64) kChunk, take.numSamples - pos);

                    for (int c = 0; c < channels; ++c)
                    {
                        chunk.copyFrom (c, 0, take.channels[(size_t) c].data() + offset + (size_t) pos, count);
                        chunk.applyGain (c, 0, count, 1.0f / gain);
                    }

                    flacOk = writer->writeFromAudioSampleBuffer (chunk, 0, count);
                }

                writer.reset();
            }
        }

        MemoryBlock result;
        MemoryOutputStream out (result, false);

        if (flacOk)
        {
            out.writeInt (kCodecFlac);
            out.writeFloat (gain);
            out.write (flac.getData(), flac.getSize());
        }
        else
        {
            // No FLAC writer for this rate or layout. Bigger, but never wrong.
            out.writeInt (kCodecRaw);
            out.writeFloat (1.0f);

            for (const auto& channel : take.channels)
                out.write (channel.data() + offset, (size_t) take.numSamples * sizeof (float));
        }

        out.flush();
        return result;
    }

    /** Fills the channels of a take whose layout has already been set up. */
    bool decodeTake (const void* data, size_t size, TransferTake& take)
    {
        if (size < 8)
            return false;

        MemoryInputStream in (data, size, false);
        const auto codec = in.readInt();
        const auto gain = in.readFloat();

        const auto channels = (int) take.channels.size();
        const auto offset = (size_t) take.padding;

        if (codec == kCodecRaw)
        {
            const auto bytes = (int) ((size_t) take.numSamples * sizeof (float));

            for (auto& channel : take.channels)
                if (in.read (channel.data() + offset, bytes) != bytes)
                    return false;

            return true;
        }

        if (codec != kCodecFlac)
            return false;

        FlacAudioFormat format;
        std::unique_ptr<AudioFormatReader> reader (
            format.createReaderFor (new MemoryInputStream (static_cast<const char*> (data) + 8, size - 8, false),
                                    true));

        if (reader == nullptr
            || (int) reader->numChannels != channels
            || reader->lengthInSamples < take.numSamples)
            return false;

        AudioBuffer<float> chunk (channels, kChunk);

        for (int64 pos = 0; pos < take.numSamples; pos += kChunk)
        {
            const auto count = (int) std::min ((int64) kChunk, take.numSamples - pos);

            if (! reader->read (&chunk, 0, count, pos, true, true))
                return false;

            for (int c = 0; c < channels; ++c)
            {
                const auto* from = chunk.getReadPointer (c);
                auto* to = take.channels[(size_t) c].data() + offset + (size_t) pos;

                for (int i = 0; i < count; ++i)
                    to[i] = from[i] * gain;
            }
        }

        return true;
    }

    std::shared_ptr<TransferTake> makeEmptyTake (double sampleRate, int channels, int64 numSamples)
    {
        auto take = std::make_shared<TransferTake>();
        take->sampleRate = sampleRate;
        take->numSamples = numSamples;
        take->padding = (int64) std::ceil (0.5 * sampleRate);
        take->channels.assign ((size_t) channels,
                               std::vector<float> ((size_t) (numSamples + 2 * take->padding), 0.0f));
        return take;
    }
}

//==============================================================================
TransferSession::TransferSession() : Thread ("PitchBox transfer analysis")
{
    startTimerHz (20);
}

TransferSession::~TransferSession()
{
    stopTimer();
    stopThread (10000);
}

void TransferSession::prepare (double newSampleRate, int maxBlockSamples, int channels)
{
    const ScopedLock lock (captureLock);

    channels = jmax (1, channels);
    maxBlock = jmax (1, maxBlockSamples);

    const auto layoutChanged = ! approximatelyEqual (newSampleRate, sampleRate)
                               || channels != numChannels
                               || fifoBuffer.getNumSamples() == 0;

    sampleRate = newSampleRate;
    numChannels = channels;

    renderer.prepare (maxBlock, sampleRate);
    auditionScratch.assign ((size_t) maxBlock, 0.0f);
    auditionGain = 0.0f;

    // Hosts call this freely, some of them on every transport start, so an
    // armed or running transfer must survive it whenever nothing that matters
    // to the recording has changed.
    if (! layoutChanged)
        return;

    if (state.load() == State::recording)
    {
        drainFifo();
        state.store (State::finishing);
    }

    const auto fifoSize = (int) std::ceil (kFifoSeconds * sampleRate) + maxBlock;
    fifoBuffer.setSize (numChannels, fifoSize);
    fifo.setTotalSize (fifoSize);

    if (state.load() == State::armed)
        resetCapture();
}

//==============================================================================
bool TransferSession::pushToFifo (const AudioBuffer<float>& buffer) noexcept
{
    const auto numSamples = buffer.getNumSamples();

    if (fifo.getFreeSpace() < numSamples)
        return false;

    const auto scope = fifo.write (numSamples);

    for (int c = 0; c < fifoBuffer.getNumChannels(); ++c)
    {
        const auto* from = buffer.getReadPointer (jmin (c, buffer.getNumChannels() - 1));

        if (scope.blockSize1 > 0)
            fifoBuffer.copyFrom (c, scope.startIndex1, from, scope.blockSize1);

        if (scope.blockSize2 > 0)
            fifoBuffer.copyFrom (c, scope.startIndex2, from + scope.blockSize1, scope.blockSize2);
    }

    return true;
}

void TransferSession::refreshAudioState() noexcept
{
    const auto latest = revision.load();

    if (latest == audioRevision)
        return;

    // Only ever try: if the message thread is in the middle of publishing, this
    // block plays what the last one did and the next one catches up.
    const SpinLock::ScopedTryLockType lock (sharedLock);

    if (! lock.isLocked())
        return;

    audioTake = take;

    if (audioAnalysis != analysis)
    {
        audioAnalysis = analysis;
        renderer.setAnalysis (analysis);
    }

    audioEdits = edits;
    audioRevision = latest;
}

void TransferSession::process (AudioBuffer<float>& buffer,
                               const Optional<AudioPlayHead::PositionInfo>& position) noexcept
{
    const auto numSamples = buffer.getNumSamples();

    if (numSamples == 0 || buffer.getNumChannels() == 0)
        return;

    const auto playing = position.hasValue() && position->getIsPlaying();
    const auto time = position.hasValue() ? position->getTimeInSamples() : Optional<int64_t>();

    auto current = state.load();

    if (current == State::armed && playing && time.hasValue())
    {
        captureStart.store (*time);
        expectedTime = *time;

        // The user may be cancelling at this very moment; whoever gets there
        // first wins.
        if (state.compare_exchange_strong (current, State::recording))
            current = State::recording;
    }

    if (current == State::recording)
    {
        // The take ends when the transport stops, when the user says so, or
        // when the playhead jumps — after a loop or a locate the audio going
        // past no longer follows on from what was recorded.
        const auto keepGoing = playing
                               && time.hasValue()
                               && ! stopRequested.load()
                               && std::abs (*time - expectedTime) <= kMaxTimeJitter
                               && pushToFifo (buffer);

        if (keepGoing)
            expectedTime += numSamples;
        else
            state.store (State::finishing);

        // What is being recorded is also what should be heard.
        return;
    }

    if (! playing || ! time.hasValue())
    {
        renderAudition (buffer);
        return;
    }

    // Once the transport runs, the take is already sounding where it belongs.
    auditionGain = 0.0f;
    auditionSeenSerial = auditionSerial.load();

    refreshAudioState();

    if (audioTake == nullptr || ! approximatelyEqual (audioTake->sampleRate, sampleRate))
        return;

    const auto takeRange = Range<int64>::withStartAndLength (audioTake->startInSong, audioTake->numSamples);
    const auto takeChannels = (int) audioTake->channels.size();

    // The renderer was prepared for maxBlock; a host that sends more than it
    // announced gets the block in slices rather than an uncorrected one.
    for (int done = 0; done < numSamples;)
    {
        const auto count = jmin (maxBlock, numSamples - done);
        const auto blockRange = Range<int64>::withStartAndLength (*time + done, count);
        const auto range = blockRange.getIntersectionWith (takeRange);

        if (! range.isEmpty())
        {
            const auto first = range.getStart() - audioTake->startInSong;
            const auto last = range.getEnd() - audioTake->startInSong;
            const auto startInBuffer = done + (int) (range.getStart() - blockRange.getStart());

            for (int c = 0; c < buffer.getNumChannels(); ++c)
            {
                if (c >= takeChannels)
                {
                    // A mono take in a wider layout: same signal everywhere.
                    buffer.copyFrom (c, startInBuffer, buffer, takeChannels - 1, startInBuffer,
                                     (int) range.getLength());
                    continue;
                }

                const auto& source = audioTake->channels[(size_t) c];

                renderer.render (source.data(),
                                 -audioTake->padding,
                                 (int64) source.size(),
                                 audioEdits,
                                 first,
                                 last,
                                 buffer.getWritePointer (c) + startInBuffer,
                                 first);
            }
        }

        done += count;
    }
}

void TransferSession::renderAudition (AudioBuffer<float>& buffer) noexcept
{
    const auto wanted = auditionOn.load();
    const auto serial = auditionSerial.load();

    if (! wanted && auditionGain <= 0.0f)
    {
        auditionSeenSerial = serial;
        return;
    }

    refreshAudioState();

    if (audioTake == nullptr || ! approximatelyEqual (audioTake->sampleRate, sampleRate))
    {
        auditionGain = 0.0f;
        return;
    }

    const auto first = jlimit ((int64) 0, audioTake->numSamples, auditionFirst.load());
    const auto last = jlimit (first, audioTake->numSamples, auditionLast.load());

    // Short fades at both ends of the stretch, so going round does not click,
    // and a ramp on the whole thing for the press and the release.
    const auto edge = (int64) jmax (1.0, 0.01 * sampleRate);
    const auto step = 1.0f / (float) jmax (1.0, 0.008 * sampleRate);

    if (last - first < 4 * edge)
    {
        auditionGain = 0.0f;
        return;
    }

    if (serial != auditionSeenSerial)
    {
        auditionSeenSerial = serial;
        auditionPosition = first;
        auditionGain = 0.0f;
    }

    const auto numSamples = buffer.getNumSamples();
    const auto takeChannels = (int) audioTake->channels.size();
    const auto direction = wanted ? step : -step;

    for (int done = 0; done < numSamples;)
    {
        if (auditionPosition < first || auditionPosition >= last)
            auditionPosition = first;

        const auto count = (int) std::min ((int64) jmin (maxBlock, numSamples - done),
                                           last - auditionPosition);

        for (int c = 0; c < buffer.getNumChannels(); ++c)
        {
            const auto& source = audioTake->channels[(size_t) jmin (c, takeChannels - 1)];

            renderer.render (source.data(),
                             -audioTake->padding,
                             (int64) source.size(),
                             audioEdits,
                             auditionPosition,
                             auditionPosition + count,
                             auditionScratch.data(),
                             auditionPosition);

            auto* out = buffer.getWritePointer (c) + done;
            auto gain = auditionGain;

            for (int i = 0; i < count; ++i)
            {
                const auto position = auditionPosition + i;
                const auto fromEdge = std::min (position - first, last - 1 - position);
                const auto window = jmin (1.0f, (float) fromEdge / (float) edge);

                gain = jlimit (0.0f, 1.0f, gain + direction);
                out[i] += auditionScratch[(size_t) i] * gain * window;
            }
        }

        auditionGain = jlimit (0.0f, 1.0f, auditionGain + direction * (float) count);
        auditionPosition += count;
        done += count;

        if (! wanted && auditionGain <= 0.0f)
            break;
    }
}

void TransferSession::startAudition (int64 firstSample, int64 lastSample)
{
    auditionFirst.store (firstSample);
    auditionLast.store (lastSample);
    ++auditionSerial;
    auditionOn.store (true);
}

void TransferSession::stopAudition()
{
    auditionOn.store (false);
}

//==============================================================================
void TransferSession::resetCapture()
{
    fifo.reset();

    captured.assign ((size_t) numChannels, {});

    for (auto& channel : captured)
        channel.reserve ((size_t) (60.0 * sampleRate));

    captureRate = sampleRate;
    recordedSamples.store (0);
    stopRequested.store (false);
    stopTicks = 0;
}

void TransferSession::toggleTransfer()
{
    {
        const ScopedLock lock (captureLock);

        if (fifoBuffer.getNumSamples() == 0)
            return;   // not prepared yet, so there is nothing to record into

        switch (state.load())
        {
            case State::idle:
                resetCapture();
                state.store (State::armed);
                break;

            case State::armed:
            {
                auto expected = State::armed;

                if (! state.compare_exchange_strong (expected, State::idle))
                    stopRequested.store (true);   // it started under our feet

                break;
            }

            case State::recording:
                stopRequested.store (true);
                break;

            case State::finishing:
                break;
        }
    }

    sendChangeMessage();
}

double TransferSession::getRecordedSeconds() const noexcept
{
    return captureRate > 0.0 ? (double) recordedSamples.load() / captureRate : 0.0;
}

void TransferSession::drainFifo()
{
    const auto ready = fifo.getNumReady();

    if (ready <= 0 || captured.empty())
        return;

    const auto scope = fifo.read (ready);

    for (size_t c = 0; c < captured.size(); ++c)
    {
        const auto* from = fifoBuffer.getReadPointer (jmin ((int) c, fifoBuffer.getNumChannels() - 1));

        captured[c].insert (captured[c].end(), from + scope.startIndex1,
                            from + scope.startIndex1 + scope.blockSize1);
        captured[c].insert (captured[c].end(), from + scope.startIndex2,
                            from + scope.startIndex2 + scope.blockSize2);
    }

    recordedSamples.store ((int64) captured.front().size());
}

void TransferSession::timerCallback()
{
    const auto current = state.load();

    if (current != State::recording && current != State::finishing)
        return;

    TransferTake::Ptr finished;
    bool changed = false;

    {
        const ScopedLock lock (captureLock);
        drainFifo();

        if (current == State::recording)
        {
            if (getRecordedSeconds() >= kMaxTakeSeconds)
                stopRequested.store (true);

            // A stop is normally acknowledged by the audio thread on its next
            // block. If the host is not running the plugin at all, nobody
            // will, so after half a second take it as read.
            if (stopRequested.load() && ++stopTicks > 10)
                state.store (State::finishing);

            return;
        }

        const auto length = captured.empty() ? (int64) 0 : (int64) captured.front().size();

        if ((double) length >= kMinTakeSeconds * captureRate)
        {
            auto newTake = makeEmptyTake (captureRate, (int) captured.size(), length);
            newTake->startInSong = captureStart.load();

            for (size_t c = 0; c < captured.size(); ++c)
                std::copy (captured[c].begin(), captured[c].end(),
                           newTake->channels[c].begin() + (std::ptrdiff_t) newTake->padding);

            finished = std::move (newTake);
        }

        captured.clear();
        stopRequested.store (false);
        state.store (State::idle);
        changed = true;
    }

    if (finished != nullptr)
        install (std::move (finished), nullptr, nullptr);
    else if (changed)
        sendChangeMessage();
}

//==============================================================================
void TransferSession::install (TransferTake::Ptr newTake,
                               const pb::EditState* replacementEdits,
                               std::shared_ptr<const MemoryBlock> encodedAudio)
{
    stopThread (10000);

    {
        const SpinLock::ScopedLockType lock (sharedLock);

        // The audio thread may still be holding the old take. Parking it here
        // until the next replacement keeps the last reference, and so the
        // free, off the audio thread.
        retiredTake = std::move (take);
        retiredAnalysis = std::move (analysis);

        take = newTake;
        analysis = nullptr;

        if (replacementEdits != nullptr)
        {
            edits = *replacementEdits;
        }
        else
        {
            // A new recording: the settings still apply, the notes do not.
            edits.notes.clear();
            edits.touched.clear();
        }

        edits.version = ++editCounter;
        ++revision;
    }

    {
        const ScopedLock lock (encodeLock);
        encoded = std::move (encodedAudio);
        encodedFor = newTake;
    }

    analysisProgress.store (0.0f);

    if (newTake != nullptr)
    {
        analysing.store (true);
        startThread (Priority::low);
    }

    sendChangeMessage();
}

void TransferSession::run()
{
    // Declared first so it runs last, after the result has been published.
    const ScopeGuard finished { [this]
    {
        analysing.store (false);
        sendChangeMessage();
    } };

    TransferTake::Ptr job;

    {
        const SpinLock::ScopedLockType lock (sharedLock);
        job = take;
    }

    if (job == nullptr || job->channels.empty())
        return;

    // Analysis is monophonic, as it is in the ARA path: pitch is a property of
    // the voice, not of the stereo image.
    std::vector<float> mono ((size_t) job->numSamples, 0.0f);
    const auto scale = 1.0f / (float) job->channels.size();

    for (const auto& channel : job->channels)
    {
        const auto* in = channel.data() + (size_t) job->padding;

        for (size_t i = 0; i < mono.size(); ++i)
            mono[i] += in[i] * scale;
    }

    pb::AnalysisParams params;
    params.sampleRate = job->sampleRate;

    auto result = pb::runAnalysis (mono.data(), job->numSamples, params,
                                   [this] (float fraction)
                                   {
                                       analysisProgress.store (fraction);
                                       return ! threadShouldExit();
                                   });

    if (result == nullptr)
        return;

    const SpinLock::ScopedLockType lock (sharedLock);

    if (take != job)
        return;

    analysis = std::move (result);

    // Done here rather than left to the editor, so a reloaded project plays
    // corrected without its window ever being opened.
    edits.adoptAnalysis (*analysis);
    edits.version = ++editCounter;
    ++revision;
}

//==============================================================================
double TransferSession::getTakeSampleRate() const
{
    const SpinLock::ScopedLockType lock (sharedLock);
    return take != nullptr ? take->sampleRate : 0.0;
}

pb::Analysis::Ptr TransferSession::getAnalysis() const
{
    const SpinLock::ScopedLockType lock (sharedLock);
    return analysis;
}

pb::EditState TransferSession::getEdits() const
{
    const SpinLock::ScopedLockType lock (sharedLock);
    return edits;
}

uint64_t TransferSession::getEditVersion() const
{
    const SpinLock::ScopedLockType lock (sharedLock);
    return edits.version;
}

void TransferSession::setEdits (pb::EditState newEdits)
{
    const SpinLock::ScopedLockType lock (sharedLock);

    // An editor that copied the state just before the analysis landed would
    // otherwise write the notes back out of existence.
    if (newEdits.notes.empty() && analysis != nullptr)
        newEdits.adoptAnalysis (*analysis);

    edits = std::move (newEdits);

    // Stamped here rather than taken from the caller: the renderer keys its
    // curve cache on the version, so no two different states may share one.
    edits.version = ++editCounter;
    ++revision;
}

//==============================================================================
std::shared_ptr<const MemoryBlock> TransferSession::encodedAudioFor (const TransferTake::Ptr& wanted)
{
    const ScopedLock lock (encodeLock);

    if (encoded == nullptr || encodedFor != wanted)
    {
        encoded = std::make_shared<const MemoryBlock> (encodeTake (*wanted));
        encodedFor = wanted;
    }

    return encoded;
}

void TransferSession::saveState (MemoryBlock& destination)
{
    TransferTake::Ptr currentTake;
    pb::EditState currentEdits;

    {
        const SpinLock::ScopedLockType lock (sharedLock);
        currentTake = take;
        currentEdits = edits;
    }

    MemoryOutputStream out (destination, false);

    out.writeInt (kStateMagic);
    out.writeInt (kStateVersion);
    out.writeString (String (currentEdits.toString()));
    out.writeBool (currentTake != nullptr);

    if (currentTake == nullptr)
        return;

    const auto audio = encodedAudioFor (currentTake);

    out.writeInt64 (currentTake->startInSong);
    out.writeDouble (currentTake->sampleRate);
    out.writeInt ((int) currentTake->channels.size());
    out.writeInt64 (currentTake->numSamples);
    out.writeInt64 ((int64) audio->getSize());
    out.write (audio->getData(), audio->getSize());
}

void TransferSession::restoreState (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes < 8)
        return;

    MemoryInputStream in (data, (size_t) sizeInBytes, false);

    if (in.readInt() != kStateMagic || in.readInt() != kStateVersion)
        return;

    const auto restoredEdits = pb::EditState::fromString (in.readString().toStdString());

    std::shared_ptr<TransferTake> newTake;
    std::shared_ptr<const MemoryBlock> audio;

    if (in.readBool())
    {
        const auto startInSong = in.readInt64();
        const auto rate = in.readDouble();
        const auto channels = in.readInt();
        const auto numSamples = in.readInt64();
        const auto audioSize = in.readInt64();

        const auto plausible = rate > 0.0 && rate <= 768000.0
                               && channels >= 1 && channels <= 8
                               && numSamples > 0
                               && (double) numSamples <= 2.0 * kMaxTakeSeconds * rate
                               && audioSize > 0 && audioSize <= in.getNumBytesRemaining();

        if (plausible)
        {
            const auto* bytes = static_cast<const char*> (data) + in.getPosition();

            newTake = makeEmptyTake (rate, channels, numSamples);
            newTake->startInSong = startInSong;

            if (decodeTake (bytes, (size_t) audioSize, *newTake))
                audio = std::make_shared<const MemoryBlock> (bytes, (size_t) audioSize);
            else
                newTake = nullptr;
        }
    }

    install (std::move (newTake), &restoredEdits, std::move (audio));
}

} // namespace onirique
