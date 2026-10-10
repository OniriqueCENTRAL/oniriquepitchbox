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
    heldVoice.gain = 0.0f;
    fadingVoice.gain = 0.0f;

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

    if (heldVoice.clip != auditionClip)
    {
        // The old clip is not cut off: it fades out underneath the new one.
        fadingVoice = heldVoice;
        heldVoice = { auditionClip, 0.0, 0.0f };
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
    heldVoice.gain = 0.0f;
    fadingVoice.gain = 0.0f;

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

void TransferSession::mixAudition (AuditionVoice& voice, AudioBuffer<float>& buffer, bool wanted) noexcept
{
    if (voice.clip == nullptr || voice.clip->length <= 0 || (! wanted && voice.gain <= 0.0f))
        return;

    const auto& clip = *voice.clip;
    const auto clipChannels = (int) clip.channels.size();
    const auto step = (wanted ? 1.0f : -1.0f) / (float) jmax (1.0, 0.012 * sampleRate);
    const auto numSamples = buffer.getNumSamples();
    const auto length = (double) clip.length;

    auto endPosition = voice.position;

    for (int c = 0; c < buffer.getNumChannels(); ++c)
    {
        const auto* source = clip.channels[(size_t) jmin (c, clipChannels - 1)].data();
        auto* out = buffer.getWritePointer (c);

        auto position = voice.position;
        auto gain = voice.gain;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto index = (int64) position;
            const auto next = index + 1 < clip.length ? index + 1 : 0;
            const auto fraction = (float) (position - (double) index);

            gain = jlimit (0.0f, 1.0f, gain + step);
            out[i] += (source[index] + fraction * (source[next] - source[index])) * gain;

            position += clip.increment;

            if (position >= length)
                position -= length;
        }

        endPosition = position;
    }

    voice.position = endPosition;
    voice.gain = jlimit (0.0f, 1.0f, voice.gain + step * (float) numSamples);
}
void TransferSession::renderAudition (AudioBuffer<float>& buffer) noexcept
{
    refreshAudioState();

    mixAudition (fadingVoice, buffer, false);
    mixAudition (heldVoice, buffer, auditionOn.load());
}

//==============================================================================
bool TransferSession::rebuildAuditionClip()
{
    const ScopedLock auditionScope (auditionLock);

    TransferTake::Ptr currentTake;
    pb::Analysis::Ptr currentAnalysis;
    pb::EditState currentEdits;

    {
        const SpinLock::ScopedLockType lock (sharedLock);
        currentTake = take;
        currentAnalysis = analysis;
        currentEdits = edits;
    }

    if (currentTake == nullptr || currentAnalysis == nullptr
        || ! approximatelyEqual (currentTake->sampleRate, sampleRate))
        return false;

    const auto rate = currentTake->sampleRate;

    const auto fade = (int64) (0.025 * rate);
    const auto shortest = (int64) (0.070 * rate);
    const auto longest = (int64) (0.110 * rate);
    const auto needed = longest + fade;

    const auto noteStart = jlimit ((int64) 0, currentTake->numSamples, auditionNoteStart);
    const auto noteEnd = jlimit (noteStart, currentTake->numSamples, auditionNoteEnd);

    // From where the note was grabbed, but kept clear of its two ends when
    // there is room: that is where the attack and the release are.
    const auto margin = jmin ((int64) (0.03 * rate), jmax ((int64) 0, (noteEnd - noteStart - needed) / 2));
    const auto from = jlimit (noteStart + margin,
                              jmax (noteStart + margin, noteEnd - margin - needed),
                              auditionCentre);

    if (from + needed > currentTake->numSamples)
        return false;

    constexpr int kBlock = 512;

    if (auditionRendererAnalysis != currentAnalysis)
    {
        auditionRenderer.prepare (kBlock, rate);
        auditionRenderer.setAnalysis (currentAnalysis);
        auditionRendererAnalysis = currentAnalysis;
    }

    const auto channels = currentTake->channels.size();
    std::vector<std::vector<float>> stretch (channels, std::vector<float> ((size_t) needed, 0.0f));

    for (size_t c = 0; c < channels; ++c)
    {
        const auto& source = currentTake->channels[c];

        for (int64 pos = 0; pos < needed; pos += kBlock)
        {
            const auto count = std::min ((int64) kBlock, needed - pos);

            auditionRenderer.render (source.data(), -currentTake->padding, (int64) source.size(),
                                     currentEdits, from + pos, from + pos + count,
                                     stretch[c].data() + pos, from + pos);
        }
    }

    std::vector<float> mono ((size_t) needed, 0.0f);

    for (const auto& channel : stretch)
        for (size_t i = 0; i < mono.size(); ++i)
            mono[i] += channel[i];

    double headEnergy = 0.0;

    for (int64 i = 0; i < fade; ++i)
        headEnergy += (double) mono[(size_t) i] * mono[(size_t) i];

    auto length = shortest;
    double best = -2.0;

    for (auto lag = shortest; lag <= longest; ++lag)
    {
        double cross = 0.0, tailEnergy = 0.0;

        for (int64 i = 0; i < fade; ++i)
        {
            const auto tail = (double) mono[(size_t) (lag + i)];
            cross += (double) mono[(size_t) i] * tail;
            tailEnergy += tail * tail;
        }

        const auto score = cross / (std::sqrt (headEnergy * tailEnergy) + 1.0e-12);

        if (score > best)
        {
            best = score;
            length = lag;
        }
    }

    auto clip = std::make_shared<AuditionClip>();

    // The pitch period of what was just rendered: the shortest lag at which it
    // matches itself. Measured on the corrected audio rather than taken from
    // the analysis, so it is the pitch that will actually be heard.
    const auto shortestPeriod = (int64) (rate / 1200.0);
    const auto longestPeriod = (int64) (rate / 55.0);
    const auto window = jmin ((int64) (0.05 * rate), needed - 5 * longestPeriod - 2);
    const auto origin = (needed - window - 5 * longestPeriod - 2) / 2;

    double period = 0.0;

    if (window > 2 * shortestPeriod && origin >= 0)
    {
        std::vector<double> score ((size_t) (longestPeriod + 2), 0.0);
        double energy = 0.0, strongest = 0.0;

        for (int64 i = 0; i < window; ++i)
            energy += (double) mono[(size_t) (origin + i)] * mono[(size_t) (origin + i)];

        for (auto lag = shortestPeriod - 1; lag <= longestPeriod + 1; ++lag)
        {
            double cross = 0.0, lagged = 0.0;

            for (int64 i = 0; i < window; ++i)
            {
                const auto later = (double) mono[(size_t) (origin + lag + i)];
                cross += (double) mono[(size_t) (origin + i)] * later;
                lagged += later * later;
            }

            score[(size_t) lag] = cross / (std::sqrt (energy * lagged) + 1.0e-12);
            strongest = jmax (strongest, score[(size_t) lag]);
        }

        // Every multiple of the period matches as well as the period does, so
        // it is the first good peak that is wanted, not the best one.
        if (strongest > 0.6)
        {
            for (auto lag = shortestPeriod; lag <= longestPeriod; ++lag)
            {
                const auto here = score[(size_t) lag];
                const auto before = score[(size_t) (lag - 1)];
                const auto after = score[(size_t) (lag + 1)];

                if (here >= strongest - 0.05 && here >= before && here >= after)
                {
                    // The true period falls between two samples; fitting a
                    // parabola through the peak finds where.
                    const auto curvature = before - 2.0 * here + after;
                    const auto offset = std::abs (curvature) > 1.0e-12 ? 0.5 * (before - after) / curvature : 0.0;

                    period = (double) lag + jlimit (-0.5, 0.5, offset);
                    break;
                }
            }
        }
    }

    if (period > 0.0)
    {
        constexpr int kTableSize = 2048;

        // A few consecutive periods averaged into one: steadier than any single
        // one of them, and free of whatever noise they do not share.
        const auto cycles = jlimit (1, 4, (int) ((double) (needed - origin - 2) / period) - 1);

        clip->length = kTableSize;
        clip->increment = (double) kTableSize / period;
        clip->channels.assign (channels, std::vector<float> ((size_t) kTableSize, 0.0f));

        for (size_t c = 0; c < channels; ++c)
        {
            const auto& source = stretch[c];

            auto read = [&] (double position)
            {
                const auto index = (size_t) position;
                const auto fraction = (float) (position - (double) index);
                return source[index] + fraction * (source[index + 1] - source[index]);
            };

            auto averaged = [&] (int tableIndex)
            {
                float sum = 0.0f;

                for (int k = 0; k < cycles; ++k)
                    sum += read ((double) origin + ((double) k + (double) tableIndex / kTableSize) * period);

                return sum / (float) cycles;
            };

            // A voice never repeats exactly, so the cycle ends a little away
            // from where it began. Taking that difference out as a slope makes
            // the table close on itself instead of stepping once per period.
            const auto mismatch = averaged (kTableSize) - averaged (0);

            for (int i = 0; i < kTableSize; ++i)
                clip->channels[c][(size_t) i] = averaged (i) - mismatch * (float) i / (float) kTableSize;
        }
    }
    else
    {
        // Unvoiced: a breath or a consonant has no period to freeze. All that
        // can be done is to go round a stretch of it, its end faded into its
        // start, at the length where the two match best.
        clip->length = length;
        clip->increment = 1.0;
        clip->channels.assign (channels, std::vector<float> ((size_t) length, 0.0f));

        for (size_t c = 0; c < channels; ++c)
        {
            for (int64 i = 0; i < length; ++i)
            {
                auto sample = stretch[c][(size_t) i];

                if (i < fade)
                {
                    const auto in = (float) i / (float) fade;
                    sample = sample * in + stretch[c][(size_t) (length + i)] * (1.0f - in);
                }

                clip->channels[c][(size_t) i] = sample;
            }
        }
    }
    // A drag that has not changed what the note sounds like gives the same
    // clip again; swapping to it would only be heard as a flutter.
    if (! auditionClips.empty() && auditionClips.back()->channels == clip->channels)
        return true;

    auditionClips.push_back (clip);

    const SpinLock::ScopedLockType lock (sharedLock);
    auditionClip = std::move (clip);
    ++revision;
    return true;
}

void TransferSession::startAudition (int64 noteStart, int64 noteEnd, int64 centre)
{
    {
        const ScopedLock auditionScope (auditionLock);

        auditionNoteStart = noteStart;
        auditionNoteEnd = noteEnd;
        auditionCentre = centre;

        // By now the audio thread has long let go of the clips before last.
        olderAuditionClips = std::move (auditionClips);
        auditionClips.clear();
    }

    if (! rebuildAuditionClip())
    {
        // Nothing to play for this note: make sure the last one is not heard
        // in its place.
        const SpinLock::ScopedLockType lock (sharedLock);
        auditionClip = nullptr;
        ++revision;
    }

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

void TransferSession::applyEdits (pb::EditState newEdits)
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

void TransferSession::setEdits (pb::EditState newEdits)
{
    applyEdits (std::move (newEdits));

    // A note being held while it is edited has to be heard changing.
    if (auditionOn.load())
        rebuildAuditionClip();
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
