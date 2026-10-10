#include "Psola.h"

#include <algorithm>
#include <cmath>

namespace pb
{

namespace
{

/** Pitch ratios outside this range stop sounding like the same voice, and the
    grain stretching they imply would blow up the source context we need. */
constexpr float kMinRatio = 0.25f;
constexpr float kMaxRatio = 4.0f;
constexpr float kMinFormant = 0.5f;
constexpr float kMaxFormant = 2.0f;

inline float sampleLinear (const float* src, int64_t srcOffset, int64_t srcLength, double absPos) noexcept
{
    const double rel = absPos - (double) srcOffset;

    if (rel <= 0.0)
        return src[0];

    if (rel >= (double) (srcLength - 1))
        return src[srcLength - 1];

    const int64_t i = (int64_t) rel;
    const float frac = (float) (rel - (double) i);
    return src[i] + frac * (src[i + 1] - src[i]);
}

} // namespace

//==============================================================================
void PsolaScratch::prepare (int64_t maxOutputSamples)
{
    acc.assign ((size_t) std::max<int64_t> (0, maxOutputSamples), 0.0);
    winSum.assign ((size_t) std::max<int64_t> (0, maxOutputSamples), 0.0);
}

//==============================================================================
namespace
{
    /** The widest gap the renderer will ever step across. Grain half-widths are
        derived from the measured distance between marks, not from the estimated
        period, and in unvoiced stretches marks can sit much further apart than
        one period — so bounding the reach by period alone leaves a block short
        of source context and its edges come out wrong. */
    double maxMarkSpacing (const PitchMarkSet& marks) noexcept
    {
        double widest = 16.0;

        for (const auto& m : marks.marks)
            widest = std::max (widest, (double) m.period);

        for (size_t i = 1; i < marks.marks.size(); ++i)
            widest = std::max (widest, (double) (marks.marks[i].position
                                                 - marks.marks[i - 1].position));

        return widest;
    }
}

int64_t sourcePaddingFor (const PitchMarkSet& marks, float formantRatio) noexcept
{
    const auto widest = maxMarkSpacing (marks);

    // A grain half-width is at most spacing / kMinRatio, and moving formants
    // scales how far into the source each sample of that grain reaches.
    const double reach = widest / (double) kMinRatio
                         * (double) std::clamp (formantRatio, kMinFormant, kMaxFormant);

    return (int64_t) std::lround (2.0 * reach) + 64;
}

//==============================================================================
std::vector<int64_t> buildPhaseAnchors (const PitchMarkSet& marks,
                                        const std::vector<int64_t>& noteStarts,
                                        double sampleRate)
{
    std::vector<int64_t> anchors;

    if (marks.marks.empty())
        return anchors;

    // Anchors always sit on a pitch mark. That matters: the renderer writes a
    // grain centred on the cursor while reading it from the nearest mark, so a
    // cursor sitting exactly on a mark is the same alignment an untouched
    // render produces. Snapping to an arbitrary sample instead would put a
    // fraction-of-a-period phase step into the middle of a held note, which is
    // audible as roughness.
    //
    // A long note needs interior anchors anyway, or the walk back from a block
    // to its anchor would grow without bound. The spacing is in samples and
    // derived only from the analysis, so it never depends on the host's buffer
    // size or on where the user started playback.
    const int64_t maxGap = (int64_t) std::lround (0.05 * sampleRate);

    std::vector<bool> wanted (marks.marks.size(), false);
    wanted[0] = true;

    // Every transition into voiced audio is a natural restart.
    for (size_t i = 1; i < marks.marks.size(); ++i)
        if (marks.marks[i].voiced && ! marks.marks[i - 1].voiced)
            wanted[i] = true;

    // So is the start of each detected note.
    for (auto s : noteStarts)
    {
        const auto idx = marks.nearest ((double) s);

        if (idx >= 0)
            wanted[(size_t) idx] = true;
    }

    int64_t lastAnchor = marks.marks.front().position;

    for (size_t i = 0; i < marks.marks.size(); ++i)
    {
        const auto pos = marks.marks[i].position;

        if (! wanted[i] && pos - lastAnchor <= maxGap)
            continue;

        anchors.push_back (pos);
        lastAnchor = pos;
    }

    anchors.erase (std::unique (anchors.begin(), anchors.end()), anchors.end());
    return anchors;
}

int64_t psolaAnchorBefore (const std::vector<int64_t>& anchors, int64_t sample) noexcept
{
    if (anchors.empty())
        return sample;

    auto it = std::upper_bound (anchors.begin(), anchors.end(), sample);

    if (it == anchors.begin())
        return anchors.front();

    return *std::prev (it);
}

int64_t psolaWalkStart (const PitchMarkSet& marks,
                        const std::vector<int64_t>& anchors,
                        int64_t firstSample) noexcept
{
    // It is not enough to start at the anchor immediately before the block. A
    // grain stretches as the pitch is pulled down — up to spacing / kMinRatio —
    // so a grain whose centre lies well before that anchor can still write into
    // the first samples of the block. Start far enough back that every grain
    // reaching the block is laid down, then let the anchor rule snap us to a
    // reproducible phase.
    const auto reach = maxMarkSpacing (marks) / (double) kMinRatio;
    const auto back = firstSample - (int64_t) std::lround (reach) - 64;

    return psolaAnchorBefore (anchors, back);
}

//==============================================================================
void psolaRender (const PsolaRequest& r, PsolaScratch& scratch)
{
    if (r.src == nullptr || r.dst == nullptr || r.marks == nullptr)
        return;

    const int64_t first = r.firstSample;
    const int64_t last  = r.lastSample;
    const int64_t outLen = last - first;

    if (outLen <= 0)
        return;

    auto passThrough = [&]
    {
        for (int64_t i = 0; i < outLen; ++i)
            r.dst[first + i - r.dstOffset] =
                sampleLinear (r.src, r.srcOffset, r.srcLength, (double) (first + i));
    };

    const auto& marks = *r.marks;

    if (marks.marks.empty())
    {
        passThrough();
        return;
    }

    if ((int64_t) scratch.acc.size() < outLen)
        scratch.prepare (outLen);   // offline path only; the renderer pre-sizes

    std::fill (scratch.acc.begin(), scratch.acc.begin() + (long) outLen, 0.0);
    std::fill (scratch.winSum.begin(), scratch.winSum.begin() + (long) outLen, 0.0);

    const float formantRatio = std::clamp (r.formantRatio, kMinFormant, kMaxFormant);

    auto ratioAt = [&] (double absSample) -> double
    {
        if (r.ratio == nullptr || r.ratioLength <= 0)
            return 1.0;

        const int64_t i = std::clamp<int64_t> ((int64_t) std::lround (absSample) - r.ratioOffset,
                                               0, r.ratioLength - 1);
        const float v = r.ratio[i];
        return (v > 0.0f && std::isfinite (v)) ? (double) std::clamp (v, kMinRatio, kMaxRatio) : 1.0;
    };

    const int64_t markCount = (int64_t) marks.marks.size();

    // Spacing to the neighbouring marks. Using the measured spacing rather than
    // the estimated period is what makes a unity render bit-transparent: the
    // synthesis cursor then lands exactly on the analysis marks.
    auto spacingLeft = [&] (int64_t i) -> double
    {
        if (i > 0)
            return std::max (4.0, (double) (marks.marks[(size_t) i].position
                                            - marks.marks[(size_t) i - 1].position));
        return std::max (4.0, (double) marks.marks[(size_t) i].period);
    };

    auto spacingRight = [&] (int64_t i) -> double
    {
        if (i + 1 < markCount)
            return std::max (4.0, (double) (marks.marks[(size_t) i + 1].position
                                            - marks.marks[(size_t) i].position));
        return std::max (4.0, (double) marks.marks[(size_t) i].period);
    };

    // Start the cursor at the last phase anchor at or before the range. Grains
    // centred before `first` still get walked over (cheaply, since they write
    // nothing) so that the phase arriving at `first` is the same one a render
    // of the whole take would have produced.
    static const std::vector<int64_t> noAnchors;
    const auto& anchors = (r.anchors != nullptr) ? *r.anchors : noAnchors;

    double s = anchors.empty() ? (double) marks.marks.front().position
                               : (double) psolaWalkStart (marks, anchors, first);

    // The cursor lands exactly on every anchor it reaches, rather than drifting
    // past it. That is what makes the walk restartable: a render beginning at
    // anchor k and a render that walked all the way from anchor 0 both arrive
    // at anchor k with the cursor in precisely the same place, so they lay down
    // the same grains from there on.
    size_t nextAnchor = 0;

    while (nextAnchor < anchors.size() && (double) anchors[nextAnchor] <= s)
        ++nextAnchor;

    const auto widest = maxMarkSpacing (marks);

    const double endAt = (double) last + 2.0 * widest / (double) kMinRatio;
    const double hardStop = (double) marks.marks.back().position + 4.0 * widest;

    while (s < endAt && s < hardStop)
    {
        const int64_t i = marks.nearest (s);
        const auto& m = marks.marks[(size_t) i];

        const double ratio = m.voiced ? ratioAt (s) : 1.0;
        const double Ts = std::max (4.0, spacingRight (i) / ratio);

        // Asymmetric Hann halves: each half spans the gap to its neighbour, so
        // consecutive grains sum to exactly one however irregular the marks are.
        // When pitching down the grains must stretch to keep the overlap closed.
        const double halfL = std::max (spacingLeft (i), Ts);
        const double halfR = std::max (spacingRight (i), Ts);

        const int64_t centre = (int64_t) std::lround (s);
        const int64_t uLo = std::max<int64_t> (-(int64_t) std::lround (halfL), first - centre);
        const int64_t uHi = std::min<int64_t> ((int64_t) std::lround (halfR), last - centre);

        for (int64_t u = uLo; u < uHi; ++u)
        {
            const double w = u < 0
                ? 0.5 - 0.5 * std::cos (M_PI * ((double) u + halfL) / halfL)
                : 0.5 + 0.5 * std::cos (M_PI * (double) u / halfR);

            if (w <= 0.0)
                continue;

            const double srcPos = (double) m.position + (double) u * (double) formantRatio;
            const auto idx = (size_t) (centre + u - first);

            scratch.acc[idx]    += w * (double) sampleLinear (r.src, r.srcOffset, r.srcLength, srcPos);
            scratch.winSum[idx] += w;
        }

        double next = s + Ts;

        if (nextAnchor < anchors.size() && next > (double) anchors[nextAnchor])
        {
            next = (double) anchors[nextAnchor];
            ++nextAnchor;
        }

        s = next;
    }

    for (int64_t i = 0; i < outLen; ++i)
    {
        const double w = scratch.winSum[(size_t) i];
        r.dst[first + i - r.dstOffset] =
            w > 1.0e-6 ? (float) (scratch.acc[(size_t) i] / w)
                       : sampleLinear (r.src, r.srcOffset, r.srcLength, (double) (first + i));
    }
}

//==============================================================================
void psolaRenderRange (const float* src,
                       int64_t numSamples,
                       const PitchMarkSet& marks,
                       const float* pitchRatio,
                       float formantRatio,
                       int64_t firstSample,
                       int64_t lastSample,
                       float* dst,
                       const std::vector<int64_t>* anchors)
{
    firstSample = std::clamp<int64_t> (firstSample, 0, numSamples);
    lastSample  = std::clamp<int64_t> (lastSample, firstSample, numSamples);

    PsolaRequest r;
    r.src          = src;
    r.srcOffset    = 0;
    r.srcLength    = numSamples;
    r.marks        = &marks;
    r.anchors      = anchors;
    r.ratio        = pitchRatio;
    r.ratioOffset  = 0;
    r.ratioLength  = numSamples;
    r.formantRatio = formantRatio;
    r.firstSample  = firstSample;
    r.lastSample   = lastSample;
    r.dst          = dst;
    r.dstOffset    = 0;

    PsolaScratch scratch;
    scratch.prepare (lastSample - firstSample);
    psolaRender (r, scratch);
}

void psolaRender (const float* src,
                  int64_t numSamples,
                  const PitchMarkSet& marks,
                  const float* pitchRatio,
                  float formantRatio,
                  float* dst,
                  const std::vector<int64_t>* anchors)
{
    psolaRenderRange (src, numSamples, marks, pitchRatio, formantRatio,
                      0, numSamples, dst, anchors);
}

} // namespace pb
