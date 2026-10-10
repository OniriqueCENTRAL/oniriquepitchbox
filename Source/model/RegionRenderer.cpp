#include "RegionRenderer.h"

#include <algorithm>
#include <cmath>

namespace pb
{

namespace
{
    /** Extra curve built beyond what a single block needs, so the curve is
        rebuilt once every few hundred blocks instead of on every one. Each
        rebuild is a short burst of arithmetic; making it rarer matters more
        than making it smaller. */
    constexpr int64_t kCurveHeadroom = 8192;
}

void RegionRenderer::prepare (int maxBlockSamples, double sampleRate, float lowestHz)
{
    maxBlock = std::max (1, maxBlockSamples);
    rate = sampleRate;

    scratch.prepare (maxBlock);
    renderBuffer.assign ((size_t) maxBlock, 0.0f);

    // Worst case look-back: the anchor spacing used by buildPhaseAnchors plus
    // the furthest a stretched grain can reach at the lowest pitch we track.
    const auto anchorGap = (int64_t) std::lround (0.05 * sampleRate);
    const auto longestPeriod = (int64_t) std::ceil (sampleRate / (double) std::max (20.0f, lowestHz));
    maxLookBack = anchorGap + 4 * longestPeriod + 256;

    curveCapacity = (int64_t) maxBlock + maxLookBack + 4 * longestPeriod + kCurveHeadroom;
    ratioCurve.assign ((size_t) curveCapacity, 1.0f);
    gainCurve.assign ((size_t) curveCapacity, 1.0f);

    curveValid = false;
}

void RegionRenderer::setAnalysis (Analysis::Ptr newAnalysis) noexcept
{
    analysis = std::move (newAnalysis);
    curveValid = false;
}

int64_t RegionRenderer::paddingSamples (float formantRatio) const noexcept
{
    if (analysis == nullptr)
        return 0;

    return analysis->paddingFor (formantRatio);
}

int64_t RegionRenderer::lookBackSamples() const noexcept
{
    return maxLookBack;
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

void RegionRenderer::refreshCurves (const EditState& edits, int64_t from, int64_t count) noexcept
{
    if (count > curveCapacity)
        count = curveCapacity;

    buildPitchRatioCurve (analysis->track, edits.notes, edits.settings,
                          from, count, ratioCurve.data());
    buildGainCurve (edits.notes, edits.settings, from, count, gainCurve.data());

    curveStart   = from;
    curveLength  = count;
    curveVersion = edits.version;
    curveValid   = true;
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

    // The synthesis cursor starts here, and so must the correction curve: the
    // cursor consults the curve all along the walk, so a curve that began at
    // the block would change the grain spacing arriving at it.
    const auto walkStart = psolaWalkStart (analysis->marks, analysis->anchors, firstSample);
    const auto needFrom  = std::min (walkStart, firstSample);
    const auto needTo    = lastSample + analysis->paddingFor (edits.settings.formantRatio);

    const bool covered = curveValid
                         && curveVersion == edits.version
                         && needFrom >= curveStart
                         && needTo <= curveStart + curveLength;

    if (! covered)
        refreshCurves (edits, needFrom, std::min (curveCapacity, needTo - needFrom + kCurveHeadroom));

    PsolaRequest r;
    r.src          = src;
    r.srcOffset    = srcOffset;
    r.srcLength    = srcLength;
    r.marks        = &analysis->marks;
    r.anchors      = &analysis->anchors;
    r.ratio        = ratioCurve.data();
    r.ratioOffset  = curveStart;
    r.ratioLength  = curveLength;
    r.formantRatio = edits.settings.formantRatio;
    r.firstSample  = firstSample;
    r.lastSample   = lastSample;
    r.dst          = renderBuffer.data();
    r.dstOffset    = firstSample;

    psolaRender (r, scratch);

    for (int64_t i = 0; i < count; ++i)
    {
        const auto g = gainCurve[(size_t) std::clamp<int64_t> (firstSample + i - curveStart,
                                                               0, curveLength - 1)];
        dst[firstSample + i - dstOffset] = renderBuffer[(size_t) i] * g;
    }
}

} // namespace pb
