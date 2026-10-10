// Onirique PitchBox — the audio-thread half of the plugin
#pragma once

#include "Analysis.h"
#include "EditState.h"

#include <cstdint>
#include <vector>

namespace pb
{

/** Renders corrected audio for one playback region, one block at a time.

    Deliberately free of any plugin framework, so the whole of the audio path
    can be exercised by an offline test that renders a take block by block and
    compares it against a single offline render. The ARA layer above only
    supplies source samples and timing.

    Real-time contract: after prepare(), render() allocates nothing and its
    cost is bounded by the block length plus a fixed look-back, independent of
    how long the take is.
*/
class RegionRenderer
{
public:
    /** Call off the audio thread. maxBlockSamples is the largest block the host
        will ask for; lowestHz sets how far back a grain can reach. */
    void prepare (int maxBlockSamples, double sampleRate, float lowestHz = 60.0f);

    void setAnalysis (Analysis::Ptr newAnalysis) noexcept;
    const Analysis* getAnalysis() const noexcept { return analysis.get(); }

    /** Source context the caller must supply either side of the block. */
    int64_t paddingSamples (float formantRatio) const noexcept;

    /** Renders [firstSample, lastSample) into dst.

        @param src        source window, covering at least
                          [firstSample - padding, lastSample + padding)
        @param srcOffset  absolute source index of src[0]
        @param srcLength  length of the window
        @param dst        output, dst[0] corresponds to absolute dstOffset
    */
    void render (const float* src,
                 int64_t srcOffset,
                 int64_t srcLength,
                 const EditState& edits,
                 int64_t firstSample,
                 int64_t lastSample,
                 float* dst,
                 int64_t dstOffset) noexcept;

    /** Takes up the synthesis cursor where another renderer of the same take
        has left it. For a caller that keeps two renderers going across an edit,
        one on the old version and one on the new: the new one then carries on
        from the phase the old one was at, instead of starting again from the
        last anchor and landing somewhere else in the cycle. */
    void continueFrom (const RegionRenderer& other) noexcept;

    /** True when there is nothing to do and the caller may pass audio through
        untouched — which is also the cheapest correct thing to do while an
        analysis is still running. */
    bool isPassThrough (const EditState& edits) const noexcept;

private:
    Analysis::Ptr analysis;
    PsolaScratch  scratch;

    std::vector<float> gainCurve;
    std::vector<float> renderBuffer;

    int   maxBlock  = 0;
    double rate     = 48000.0;};

} // namespace pb
