#include "Analysis.h"

#include "../dsp/YinPitchTracker.h"

namespace pb
{

Analysis::Ptr runAnalysis (const float* mono,
                           int64_t numSamples,
                           const AnalysisParams& params,
                           const std::function<bool (float)>& progress)
{
    if (mono == nullptr || numSamples <= 0)
        return nullptr;

    auto result = std::make_shared<Analysis>();
    result->sampleRate = params.sampleRate;
    result->numSamples = numSamples;

    auto report = [&progress] (float fraction) -> bool
    {
        return progress ? progress (fraction) : true;
    };

    // 1. Fundamental frequency. This is the expensive stage, so it carries the
    //    progress reporting and the abort check.
    YinPitchTracker::Params yin;
    yin.sampleRate = params.sampleRate;
    yin.hopSize    = params.hopSize;
    yin.minHz      = params.minHz;
    yin.maxHz      = params.maxHz;
    yin.windowSize = 2048;

    // The window has to hold two periods of the lowest pitch we claim to track,
    // or the difference function has nothing to find.
    const auto needed = (int) std::ceil (2.5 * params.sampleRate / (double) params.minHz);

    while (yin.windowSize < needed)
        yin.windowSize *= 2;

    YinPitchTracker tracker (yin);
    result->track = tracker.process (mono, numSamples,
                                     [&report] (float f) { return report (0.70f * f); });

    if (result->track.empty())
        return nullptr;

    if (! report (0.72f))
        return nullptr;

    medianSmoothPitchTrack (result->track);

    // 2. Grain anchors for PSOLA.
    const auto periods = buildPeriodCurve (result->track, numSamples);
    result->marks = detectPitchMarks (mono, numSamples, result->track, periods);

    if (! report (0.90f))
        return nullptr;

    // 3. Notes, then the key they imply.
    result->notes = detectNotes (result->track, params.notes);
    result->key   = estimateScale (pitchClassHistogram (result->notes));

    std::vector<int64_t> noteStarts;
    noteStarts.reserve (result->notes.size());

    for (const auto& n : result->notes)
        noteStarts.push_back (n.startSample);

    result->anchors = buildPhaseAnchors (result->marks, noteStarts, params.sampleRate);

    if (! report (1.0f))
        return nullptr;

    return result;
}

} // namespace pb
