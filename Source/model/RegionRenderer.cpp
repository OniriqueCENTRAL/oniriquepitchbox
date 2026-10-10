#include "RegionRenderer.h"

#include <algorithm>
#include <cmath>

namespace pb
{

namespace
{
    /** What the renderer hands PSOLA in place of a ratio curve. */
    struct RatioQuery
    {
        const Analysis*  analysis;
        const EditState* edits;
    };

    float ratioAtSample (void* context, int64_t sample) noexcept
    {
        const auto& q = *static_cast<const RatioQuery*> (context);
        return pitchRatioAt (q.analysis->track, q.edits->notes, q.edits->settings, sample);
    }
}
void RegionRenderer::prepare (int maxBlockSamples, double sampleRate, float lowestHz)
{
    maxBlock = std::max (1, maxBlockSamples);
    rate = sampleRate;

    scratch.prepare (maxBlock);
    scratch.forgetWalk();
    renderBuffer.assign ((size_t) maxBlock, 0.0f);
    gainCurve.assign ((size_t) maxBlock, 1.0f);

    (void) lowestHz;
}
void RegionRenderer::setAnalysis (Analysis::Ptr newAnalysis) noexcept
{
    analysis = std::move (newAnalysis);
    scratch.forgetWalk();
}

int64_t RegionRenderer::paddingSamples (float formantRatio) const noexcept
{
    if (analysis == nullptr)
        return 0;

    return analysis->paddingFor (formantRatio);
}

bool RegionRenderer::isPassThrough (const EditState& edits) const noexcept
{
    if (analysis == nullptr || analysis->marks.marks.empty())
        return true;

    if (! edits.settings.enabled)
        return true;

    if (std::abs (edits.settings.formantRatio - 1.0f) > 1.0e-4f)
        return false;

    for (const auto& n : edits.notes)
    {
        if (n.bypassed)
            continue;

        if (n.correctAmount > 0.0f && std::abs (n.targetCents - n.centsDetected) > 0.01f)
            return false;

        if (std::abs (n.vibratoScale - 1.0f) > 1.0e-4f)
            return false;

        if (std::abs (n.gainDb) > 1.0e-4f)
            return false;
    }

    return true;
}

void RegionRenderer::render (const float* src,
                             int64_t srcOffset,
                             int64_t srcLength,
                             const EditState& edits,
                             int64_t firstSample,
                             int64_t lastSample,
                             float* dst,
                             int64_t dstOffset) noexcept
{
    const auto count = lastSample - firstSample;

    if (count <= 0 || src == nullptr || dst == nullptr)
        return;

    auto passThrough = [&]
    {
        for (int64_t i = 0; i < count; ++i)
        {
            const auto s = firstSample + i - srcOffset;
            dst[firstSample + i - dstOffset] =
                (s >= 0 && s < srcLength) ? src[s] : 0.0f;
        }
    };

    if (analysis == nullptr || count > (int64_t) maxBlock)
    {
        // A block larger than we prepared for would mean allocating here, which
        // the audio thread must never do. Passing the take through unchanged is
        // the one safe answer.
        passThrough();
        return;
    }

    if (isPassThrough (edits))
    {
        passThrough();
        return;
    }

    // The cursor reads the correction all along its walk, from the last phase
    // anchor to the end of the block, but only at the centre of each grain. So
    // it is given a way to ask for the ratio at a sample rather than a curve
    // with every sample of that walk filled in.
    RatioQuery query { analysis.get(), &edits };
    PsolaRequest r;
    r.src          = src;
    r.srcOffset    = srcOffset;
    r.srcLength    = srcLength;
    r.marks        = &analysis->marks;
    r.anchors      = &analysis->anchors;
    r.ratioAt      = &ratioAtSample;
    r.ratioContext = &query;
    r.walkKey      = edits.version;
    r.formantRatio = edits.settings.formantRatio;
    r.firstSample  = firstSample;
    r.lastSample   = lastSample;
    r.dst          = renderBuffer.data();
    r.dstOffset    = firstSample;

    psolaRender (r, scratch);

    buildGainCurve (edits.notes, edits.settings, firstSample, count, gainCurve.data());

    for (int64_t i = 0; i < count; ++i)
        dst[firstSample + i - dstOffset] = renderBuffer[(size_t) i] * gainCurve[(size_t) i];
}

} // namespace pb
