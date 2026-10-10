// Onirique PitchBox — time-domain PSOLA pitch shifting
#pragma once

#include "PitchMarks.h"

#include <cstdint>
#include <vector>

namespace pb
{

/** Deterministic phase-reset points for the synthesis cursor.

    PSOLA lays grains down by walking a cursor forward from some starting point,
    and where that walk begins decides the grain phase everywhere after it. A
    naive renderer starts at the first mark in the file, which makes every block
    cost as much as the whole take and is unusable on the audio thread.

    Anchors fix that. The cursor restarts at the nearest anchor at or before the
    block, so the walk is bounded by the distance between anchors rather than by
    the length of the take — and because an anchor's position depends only on
    the analysis and not on where the host happens to split its buffers, the
    same sample renders identically whether the host asks for it in one block,
    in forty, or during an offline bounce.

    A reset is not free, though. Once the pitch is being changed the cursor no
    longer falls on the marks, and putting it back on one means a cycle of the
    wrong length. Cut short, that cycle is two glottal pulses almost on top of
    one another, which is heard as a pop; and one every few hundredths of a
    second is a flutter over the whole voice.

    So an anchor sits where the voice comes back in after a pause, and the
    cursor is not put there but brought there: across the pause leading up to
    it, where there is no pitch to disturb, every step is lengthened or
    shortened by the same small fraction so that the last one arrives exactly.
    Note starts are deliberately not anchors: in legato singing they come
    several to the second, in the middle of the sound.

    The one case with no pause to land in is a stretch voiced for longer than
    kMaxAnchorGapSeconds. There an anchor has to fall in the sound itself, and
    the cursor takes whichever is nearer of stretching the cycle it is in or
    ending it early, so that it is never off by more than half.
*/constexpr double kMaxAnchorGapSeconds = 2.0;
std::vector<int64_t> buildPhaseAnchors (const PitchMarkSet& marks,
                                        const std::vector<int64_t>& noteStarts,
                                        double sampleRate);

/** Greatest anchor at or before `sample`. A renderer asked for a block must
    supply its pitch-ratio curve from this sample onwards, not merely from the
    start of the block: the cursor consults the curve all the way along the
    walk, so a curve that began at the block would change the grain spacing
    arriving at it and the block would no longer match an offline render. */
int64_t psolaAnchorBefore (const std::vector<int64_t>& anchors, int64_t sample) noexcept;

/** The sample the synthesis cursor must start walking from in order to render
    a block beginning at `firstSample`, which is also where that block's
    pitch-ratio curve has to begin. This sits further back than the nearest
    anchor: grains stretch when the pitch is pulled down, so a grain centred
    before the preceding anchor can still reach into the block. */
int64_t psolaWalkStart (const PitchMarkSet& marks,
                        const std::vector<int64_t>& anchors,
                        int64_t firstSample) noexcept;

/** Pre-allocated working buffers, so a render does no heap allocation.
    One of these per renderer; call prepare() off the audio thread. */
struct PsolaScratch
{
    void prepare (int64_t maxOutputSamples);

    std::vector<double> acc;
    std::vector<double> winSum;

    //==============================================================================
    /** Where the cursor was, so the next block need not walk there again.

        Anchors can be seconds apart, and a walk from the last one on every
        block would cost far more than the block itself. But playback asks for
        blocks in order, and the cursor's path through the take is the same
        whichever block is being rendered — so a state the last walk passed
        through, far enough back to be clear of the next block, is exactly the
        state a fresh walk from the anchor would reach there. Resuming from it
        changes the cost and nothing else.

        Two are kept: one good for a block starting where the last one started
        (the second channel of a stereo pair is rendered that way), one for a
        block starting where the last one ended.
    */
    struct WalkState
    {
        double s = 0.0;
        size_t nextAnchor = 0;

        // The approach to that anchor: where the pause before it begins, and
        // once the cursor is in it, the factor on every step and how many are
        // left before it arrives.
        double  landFrom  = 0.0;
        double  scale     = 1.0;
        int64_t stepsLeft = 0;
        bool    landing   = false;
    };

    struct WalkMemo
    {
        bool valid = false;
        int64_t validFrom = 0;   // good for any block starting at or after this
        WalkState state;
    };

    void forgetWalk() noexcept
    {
        walkKey = 0;
        atFirst.valid = false;
        atLast.valid = false;
    }

    uint64_t walkKey = 0;
    const PitchMarkSet* walkMarks = nullptr;
    const std::vector<int64_t>* walkAnchors = nullptr;
    WalkMemo atFirst, atLast;
};

/** A render request in absolute audio-source sample coordinates.

    `src` need not be the whole take: it is a window covering
    [srcOffset, srcOffset + srcLength), and the renderer reads nothing outside
    it. sourcePaddingFor() says how much context the window must carry on each
    side of the range being rendered.
*/
struct PsolaRequest
{
    const float* src        = nullptr;
    int64_t      srcOffset  = 0;   // absolute index of src[0]
    int64_t      srcLength  = 0;

    const PitchMarkSet*         marks   = nullptr;
    const std::vector<int64_t>* anchors = nullptr;

    /** Per-sample target/source frequency ratio, covering at least the rendered
        range, indexed in absolute samples via ratioOffset. */
    const float* ratio       = nullptr;
    int64_t      ratioOffset = 0;
    int64_t      ratioLength = 0;

    /** Used instead when `ratio` is null: the ratio at one absolute sample.
        The cursor only reads the ratio at grain centres, so a caller that can
        answer for a single sample need not fill in a curve at all. */
    float (*ratioAt) (void* context, int64_t sample) noexcept = nullptr;
    void* ratioContext = nullptr;

    /** Identifies what the cursor's path depends on other than the marks and
        anchors: in practice, the version of the edits. A non-zero key lets the
        walk resume from where the scratch last left it; 0 walks from the anchor
        every time. */
    uint64_t walkKey = 0;

    float formantRatio = 1.0f;

    int64_t firstSample = 0;   // absolute, inclusive
    int64_t lastSample  = 0;   // absolute, exclusive

    float*  dst       = nullptr;
    int64_t dstOffset = 0;     // absolute index of dst[0]
};

/** Source context, in samples, that a window must carry either side of the
    rendered range for the result to be exact. Grains stretch when pitching
    down and when formants are moved, so this scales with both limits. */
int64_t sourcePaddingFor (const PitchMarkSet& marks, float formantRatio) noexcept;

/** Renders [firstSample, lastSample). Real-time safe: no allocation, and the
    work is bounded by the range plus the distance back to the last anchor. */
void psolaRender (const PsolaRequest& request, PsolaScratch& scratch);

//==============================================================================
// Offline convenience wrappers: whole-buffer coordinates, allocate internally.

void psolaRender (const float* src,
                  int64_t numSamples,
                  const PitchMarkSet& marks,
                  const float* pitchRatio,
                  float formantRatio,
                  float* dst,
                  const std::vector<int64_t>* anchors = nullptr);

void psolaRenderRange (const float* src,
                       int64_t numSamples,
                       const PitchMarkSet& marks,
                       const float* pitchRatio,
                       float formantRatio,
                       int64_t firstSample,
                       int64_t lastSample,
                       float* dst,
                       const std::vector<int64_t>* anchors = nullptr);

} // namespace pb
