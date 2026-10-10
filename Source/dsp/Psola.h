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

    Anchors are placed where a phase reset is inaudible: at unvoiced stretches
    and at note starts, where the waveform restarts anyway.
*/
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
