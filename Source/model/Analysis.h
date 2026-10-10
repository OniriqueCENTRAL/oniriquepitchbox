// Onirique PitchBox — one take, analysed
#pragma once

#include "../dsp/PitchMarks.h"
#include "../dsp/Psola.h"
#include "../dsp/Scale.h"
#include "Notes.h"

#include <atomic>
#include <functional>
#include <memory>

namespace pb
{

/** Everything the renderer needs to know about an audio source, produced once
    and then treated as immutable.

    Immutability is the whole point. The analysis is expensive and happens on a
    background thread, while the renderer reads it from the audio thread; making
    the result a value that is published whole, and replaced rather than
    modified, means neither side ever needs a lock around the data itself.

    The user's edits deliberately live elsewhere, in the audio modification. An
    edit changes where notes should go, not what was sung, so it never
    invalidates this — which is why a correction can be heard on the next
    buffer instead of after a re-analysis.
*/
struct Analysis
{
    using Ptr = std::shared_ptr<const Analysis>;

    double  sampleRate = 48000.0;
    int64_t numSamples = 0;

    PitchTrack           track;
    PitchMarkSet         marks;
    std::vector<int64_t> anchors;
    std::vector<Note>    notes;
    ScaleEstimate        key;

    /** Source context the renderer must have either side of a block. */
    int64_t paddingFor (float formantRatio) const noexcept
    {
        return sourcePaddingFor (marks, formantRatio);
    }
};

struct AnalysisParams
{
    double sampleRate = 48000.0;
    float  minHz = 60.0f;
    float  maxHz = 1600.0f;
    int    hopSize = 256;
    NoteDetectorParams notes;
};

/** Runs the full analysis chain over one mono take.

    @param progress  called occasionally with 0..1; return false to abort, in
                     which case the function returns nullptr.
*/
Analysis::Ptr runAnalysis (const float* mono,
                           int64_t numSamples,
                           const AnalysisParams& params,
                           const std::function<bool (float)>& progress = {});

} // namespace pb
