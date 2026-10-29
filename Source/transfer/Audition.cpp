#include "Audition.h"

namespace onirique
{

void AuditionBuilder::startNewNote()
{
    olderClips = std::move (clips);
    clips.clear();
}

AuditionClip::Ptr AuditionBuilder::build (const AuditionTake& take, const pb::Analysis::Ptr& analysis,
                                          const pb::EditState& edits, int64 noteStartIn, int64 noteEndIn, int64 centreIn)
{
    if (analysis == nullptr || take.channels.empty())
        return nullptr;

    const auto rate = take.rate;

    const auto fade = (int64) (0.025 * rate);
    const auto shortest = (int64) (0.070 * rate);
    const auto longest = (int64) (0.110 * rate);
    const auto needed = longest + fade;

    if (take.numSamples < needed)
        return nullptr;

    const auto noteStart = jlimit ((int64) 0, take.numSamples, noteStartIn);
    const auto noteEnd = jlimit (noteStart, take.numSamples, noteEndIn);

    // Hold the note where it was grabbed, or as near to there as it is
    // actually sung: the analysis knows which frames are voiced and at what
    // pitch, and a frame that is not voiced has no cycle to freeze.
    const pb::PitchFrame* frame = nullptr;

    for (const auto& candidate : analysis->track.frames)
    {
        if (! candidate.voiced || candidate.f0Hz <= 0.0f
            || candidate.sampleIndex < noteStart || candidate.sampleIndex >= noteEnd)
            continue;

        if (frame == nullptr
            || std::abs (candidate.sampleIndex - centreIn) < std::abs (frame->sampleIndex - centreIn))
            frame = &candidate;
    }

    const auto centre = frame != nullptr ? (int64) frame->sampleIndex : centreIn;
    const auto from = jlimit ((int64) 0, take.numSamples - needed, centre);

    // What the analysis and the edits say the pitch is there, before and after
    // correction. The period is looked for between the two and nowhere else,
    // which is what keeps it from landing on an octave below.
    double sungHz = 0.0, correctedHz = 0.0;

    if (frame != nullptr)
    {
        sungHz = frame->f0Hz;
        correctedHz = sungHz;

        const auto index = pb::noteAtSample (edits.notes, frame->sampleIndex);

        if (index >= 0 && edits.settings.enabled && ! edits.notes[(size_t) index].bypassed)
        {
            const auto& n = edits.notes[(size_t) index];
            const auto sungCents = pb::hzToCents (sungHz);
            const auto residual = sungCents - (double) n.centsDetected;

            correctedHz = pb::centsToHz ((double) n.centsDetected
                                         + (double) n.correctAmount * (double) (n.targetCents - n.centsDetected)
                                         + (double) n.vibratoScale * residual);
        }
    }
    constexpr int kBlock = 512;

    if (rendererAnalysis != analysis || ! approximatelyEqual (rendererRate, rate))
    {
        renderer.prepare (kBlock, rate);
        renderer.setAnalysis (analysis);
        rendererAnalysis = analysis;
        rendererRate = rate;
    }

    const auto channels = take.channels.size();
    std::vector<std::vector<float>> stretch (channels, std::vector<float> ((size_t) needed, 0.0f));

    for (size_t c = 0; c < channels; ++c)
    {
        const auto* source = take.channels[c];

        for (int64 pos = 0; pos < needed; pos += kBlock)
        {
            const auto count = std::min ((int64) kBlock, needed - pos);

            renderer.render (source, take.firstSample, take.length,
                             edits, from + pos, from + pos + count,
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
    const auto longestPeriod = (int64) (rate / 55.0);
    const auto window = jmin ((int64) (0.05 * rate), needed - 5 * longestPeriod - 2);

    double period = 0.0;

    if (sungHz > 0.0 && correctedHz > 0.0 && window > 0)
    {
        // Near a note boundary the correction is only part way in, so the true
        // pitch can be anywhere between the sung one and the corrected one.
        const auto expected = rate / correctedHz;
        const auto lowest = jlimit ((int64) 8, longestPeriod,
                                    (int64) std::floor (0.94 * rate / jmax (sungHz, correctedHz)));
        const auto highest = jlimit (lowest, longestPeriod,
                                     (int64) std::ceil (1.06 * rate / jmin (sungHz, correctedHz)));

        std::vector<double> score ((size_t) (highest + 2), 0.0);
        double energy = 0.0, strongest = -2.0;
        auto strongestLag = lowest;

        for (int64 i = 0; i < window; ++i)
            energy += (double) mono[(size_t) i] * mono[(size_t) i];

        for (auto lag = lowest - 1; lag <= highest + 1; ++lag)
        {
            double cross = 0.0, lagged = 0.0;

            for (int64 i = 0; i < window; ++i)
            {
                const auto later = (double) mono[(size_t) (lag + i)];
                cross += (double) mono[(size_t) i] * later;
                lagged += later * later;
            }

            score[(size_t) lag] = cross / (std::sqrt (energy * lagged) + 1.0e-12);

            if (lag >= lowest && lag <= highest && score[(size_t) lag] > strongest)
            {
                strongest = score[(size_t) lag];
                strongestLag = lag;
            }
        }

        if (strongest < 0.3)
        {
            // Too breathy to measure. The analysis still says what the pitch
            // is, and a held tone at that pitch is better than a stutter.
            period = expected;
        }
        else
        {
            // When the range is wide enough to hold two multiples of the
            // period, it is the first good peak that is wanted.
            auto chosen = strongestLag;

            for (auto lag = lowest; lag <= highest; ++lag)
            {
                const auto here = score[(size_t) lag];

                if (here >= strongest - 0.05 && here >= score[(size_t) (lag - 1)]
                    && here >= score[(size_t) (lag + 1)])
                {
                    chosen = lag;
                    break;
                }
            }

            // The true period falls between two samples; fitting a parabola
            // through the peak finds where.
            const auto before = score[(size_t) (chosen - 1)];
            const auto here = score[(size_t) chosen];
            const auto after = score[(size_t) (chosen + 1)];
            const auto curvature = before - 2.0 * here + after;
            const auto offset = curvature < -1.0e-12 ? 0.5 * (before - after) / curvature : 0.0;

            period = (double) chosen + jlimit (-0.5, 0.5, offset);
        }
    }

    const int64 origin = 0;
    if (period > 0.0)
    {
        constexpr int kTableSize = 2048;

        // A few consecutive periods averaged into one: steadier than any single
        // one of them, and free of whatever noise they do not share.
        // Only as many as the note still has left, so that a note grabbed near
        // its end is not averaged with the start of the next one.
        const auto room = (double) jmin (needed - 2, jmax ((int64) 0, noteEnd - from)) / period;
        const auto cycles = jlimit (1, 4, (int) room - 1);

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
    if (! clips.empty() && clips.back()->channels == clip->channels)
        return clips.back();

    clips.push_back (clip);
    return clip;
}

//==============================================================================
void AuditionPlayer::mix (AudioBuffer<float>& buffer, const AuditionClip::Ptr& clip, bool wanted,
                          double outputRate, double rateRatio) noexcept
{
    if (held.clip != clip)
    {
        // The old clip is not cut off: it fades out underneath the new one.
        fading = held;
        held = { clip, 0.0, 0.0f };
    }

    mixVoice (fading, buffer, false, outputRate, rateRatio);
    mixVoice (held, buffer, wanted, outputRate, rateRatio);
}

void AuditionPlayer::mixVoice (Voice& voice, AudioBuffer<float>& buffer, bool wanted,
                               double outputRate, double rateRatio) noexcept
{
    if (voice.clip == nullptr || voice.clip->length <= 0 || (! wanted && voice.gain <= 0.0f))
        return;

    const auto& clip = *voice.clip;
    const auto clipChannels = (int) clip.channels.size();
    const auto step = (wanted ? 1.0f : -1.0f) / (float) jmax (1.0, 0.012 * outputRate);
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

            position += clip.increment * rateRatio;

            if (position >= length)
                position -= length;
        }

        endPosition = position;
    }

    voice.position = endPosition;
    voice.gain = jlimit (0.0f, 1.0f, voice.gain + step * (float) numSamples);
}

} // namespace onirique
