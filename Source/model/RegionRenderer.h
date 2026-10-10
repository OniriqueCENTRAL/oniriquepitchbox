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

    /** Earliest source sample the renderer will consult for a block starting
        here. The caller does not need this for reading — paddingSamples covers
        that — but the ratio cache sizing does. */
    int64_t lookBackSamples() const noexcept;

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

    /** True when there is nothing to do and the caller may pass audio through
        untouched — which is also the cheapest correct thing to do while an
        analysis is still running. */
    bool isPassThrough (const EditState& edits) const noexcept;

private:
    void refreshCurves (const EditState& edits, int64_t from, int64_t count) noexcept;

    Analysis::Ptr analysis;
    PsolaScratch  scratch;

    std::vector<float> ratioCurve;
    std::vector<float> gainCurve;
    std::vector<float> renderBuffer;

    int64_t  curveStart   = 0;
    int64_t  curveLength  = 0;
    int64_t  curveCapacity = 0;
    uint64_t curveVersion = 0;
    bool     curveValid   = false;

    int   maxBlock  = 0;
    double rate     = 48000.0;
    int64_t maxLookBack = 0;
};

} // namespace pb
