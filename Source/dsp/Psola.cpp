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

/** How many cycles before an anchor the cursor starts being steered onto it. */
constexpr double kApproachCycles = 24.0;
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
        if (marks.widestSpacing > 0.0)
            return marks.widestSpacing;

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
    // render produces.
    //
    // The spacing is in samples and derived only from the analysis, so it never
    // depends on the host's buffer size or on where the user started playback.
    const auto maxGap = (int64_t) std::lround (kMaxAnchorGapSeconds * sampleRate);

    // Where the note boundaries fall no longer matters to the anchors; see the
    // header for why.
    (void) noteStarts;

    const auto& m = marks.marks;

    anchors.push_back (m.front().position);
    auto lastAnchor = m.front().position;
    size_t pause = 0;

    for (size_t i = 1; i < m.size(); ++i)
    {
        while (pause < marks.pauses.size() && marks.pauses[pause].end < m[i].position)
            ++pause;

        // The voice comes back in here, after a real pause.
        const auto resumes = pause < marks.pauses.size() && marks.pauses[pause].end == m[i].position;

        if (resumes || m[i].position - lastAnchor > maxGap)
        {
            anchors.push_back (m[i].position);
            lastAnchor = m[i].position;
        }
    }

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
        float v = 1.0f;

        if (r.ratio != nullptr && r.ratioLength > 0)
            v = r.ratio[std::clamp<int64_t> ((int64_t) std::lround (absSample) - r.ratioOffset,
                                             0, r.ratioLength - 1)];
        else if (r.ratioAt != nullptr)
            v = r.ratioAt (r.ratioContext, (int64_t) std::lround (absSample));
        else
            return 1.0;

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

    const auto widest = maxMarkSpacing (marks);

    // No grain centred further back than this can reach the block.
    const double clearOf = widest / (double) kMinRatio + 64.0;

    PsolaScratch::WalkState state;
    state.s = anchors.empty() ? (double) marks.marks.front().position
                              : (double) psolaWalkStart (marks, anchors, first);

    // The cursor lands exactly on every anchor it reaches, rather than drifting
    // past it. That is what makes the walk restartable: a render beginning at
    // anchor k and a render that walked all the way from anchor 0 both arrive
    // at anchor k with the cursor in precisely the same place, so they lay down
    // the same grains from there on.
    while (state.nextAnchor < anchors.size() && (double) anchors[state.nextAnchor] <= state.s)
        ++state.nextAnchor;


    const auto remembers = r.walkKey != 0;

    if (remembers
        && scratch.walkKey == r.walkKey
        && scratch.walkMarks == r.marks
        && scratch.walkAnchors == r.anchors)
    {
        // Resume from the later of the two remembered states that this block
        // may use — unless the anchor is nearer still, as after a locate.
        const auto* memo = (scratch.atLast.valid && first >= scratch.atLast.validFrom) ? &scratch.atLast
                         : (scratch.atFirst.valid && first >= scratch.atFirst.validFrom) ? &scratch.atFirst
                         : nullptr;

        if (memo != nullptr && memo->state.s >= state.s)
            state = memo->state;
    }

    PsolaScratch::WalkMemo atFirst, atLast;

    // A set of marks made by hand, with no pauses worked out, falls back on
    // the voiced flag of each mark.
    const auto knowsPauses = marks.pauseEdge > 0;
    size_t walkPause = 0;

    const double endAt = (double) last + 2.0 * widest / (double) kMinRatio;
    const double hardStop = (double) marks.marks.back().position + 4.0 * widest;

    while (state.s < endAt && state.s < hardStop)
    {
        const double s = state.s;

        if (s <= (double) first - clearOf)
            atFirst = { true, first, state };

        if (s <= (double) last - clearOf)
            atLast = { true, last, state };

        const int64_t i = marks.nearest (s);
        const auto& m = marks.marks[(size_t) i];

        const auto hasAnchor = state.nextAnchor < anchors.size();
        const auto anchor = hasAnchor ? (double) anchors[state.nextAnchor] : 0.0;

        // Inside a pause the steps follow the marks. Everywhere else they
        // follow the pitch being asked for — including where the detector lost
        // the pitch for a moment but the voice carried on, or the voice would
        // be handed its own pitch back for as long as that lasted.
        while (walkPause < marks.pauses.size() && (double) marks.pauses[walkPause].end <= s)
            ++walkPause;

        const auto inPause = walkPause < marks.pauses.size() && (double) marks.pauses[walkPause].begin <= s;
        const auto shifted = knowsPauses ? ! inPause : m.voiced;

        const double ratio = shifted ? ratioAt (s) : 1.0;
        double Ts = std::max (4.0, spacingRight (i) / ratio);

        // Coming up to an anchor, the cursor is steered onto it rather than
        // dropped there. Each step it takes the distance left, the nearest
        // whole number of cycles that would cover it, and that share of the
        // distance; so what would have been one cycle half its proper length
        // is a couple of dozen cycles each a part or two in a hundred out.
        auto arrives = false;

        if (hasAnchor)
        {
            const auto left = anchor - s;

            if (left < kApproachCycles * Ts)
            {
                const auto cycles = std::max<int64_t> (1, std::min ((int64_t) std::lround (left / Ts),
                                                                    (int64_t) (left / 4.0)));
                Ts = left / (double) cycles;
                arrives = cycles == 1;
            }
        }

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

        if (hasAnchor && (arrives || next >= anchor))
        {
            next = anchor;
            ++state.nextAnchor;
        }

        state.s = next;
    }

    if (remembers)
    {
        scratch.walkKey = r.walkKey;
        scratch.walkMarks = r.marks;
        scratch.walkAnchors = r.anchors;
        scratch.atFirst = atFirst;
        scratch.atLast = atLast;
    }
    // Through a pause the recording itself goes out, and at each end of one it
    // is faded against the resynthesis: out of it once the voice has stopped,
    // back into it once the voice has started again. On the way back in the
    // two line up, because the cursor is on the marks there.
    const auto edge = (double) std::max<int64_t> (1, marks.pauseEdge);
    size_t pause = 0;

    while (pause < marks.pauses.size() && (double) marks.pauses[pause].end + edge <= (double) first)
        ++pause;

    for (int64_t i = 0; i < outLen; ++i)
    {
        const auto at = first + i;
        const auto original = sampleLinear (r.src, r.srcOffset, r.srcLength, (double) at);
        const double w = scratch.winSum[(size_t) i];
        const auto rendered = w > 1.0e-6 ? (float) (scratch.acc[(size_t) i] / w) : original;

        while (pause < marks.pauses.size() && (double) marks.pauses[pause].end + edge <= (double) at)
            ++pause;

        // How much of the resynthesis to use here: all of it away from any
        // pause, none of it inside one.
        double share = 1.0;

        for (auto k = pause; k < marks.pauses.size() && marks.pauses[k].begin <= at; ++k)
        {
            const auto& p = marks.pauses[k];
            const auto in  = ((double) at - (double) p.begin) / edge;   // 0 where the pause begins
            const auto out = ((double) at - (double) p.end) / edge;     // 0 where the voice returns

            auto here = out >= 0.0 ? std::clamp (out, 0.0, 1.0) : 1.0 - std::clamp (in, 0.0, 1.0);
            here = here * here * (3.0 - 2.0 * here);

            share = std::min (share, here);
        }

        r.dst[at - r.dstOffset] = share >= 1.0 ? rendered
                                               : original + (float) share * (rendered - original);
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
