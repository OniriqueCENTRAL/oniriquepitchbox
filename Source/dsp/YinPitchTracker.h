// Onirique PitchBox — YIN fundamental-frequency tracker (FFT-accelerated)
#pragma once

#include "Fft.h"
#include "PitchTrack.h"

#include <functional>
#include <memory>

namespace pb
{

/** YIN pitch tracker.

    Implements the cumulative mean normalised difference function of
    de Cheveigne & Kawahara (2002), with the squared-difference term evaluated
    through an FFT cross-correlation so that cost does not grow with the
    lag range.
*/
class YinPitchTracker
{
public:
    struct Params
    {
        double sampleRate = 48000.0;
        int    windowSize = 2048;  // analysis window, >= 2 * (sampleRate / minHz)
        int    hopSize    = 256;
        float  minHz      = 60.0f;
        float  maxHz      = 1600.0f;
        float  threshold  = 0.15f; // YIN absolute threshold
        float  voicedConfidence = 0.55f;
        float  silenceDb  = -60.0f; // frames quieter than this are never voiced
    };

    explicit YinPitchTracker (const Params& p);

    /** Analyses the whole signal. Calls progress(0..1) occasionally if supplied;
        returning false from progress aborts and yields a partial track. */
    PitchTrack process (const float* samples,
                        int64_t numSamples,
                        const std::function<bool (float)>& progress = {}) const;

private:
    Params params;
    int tauMin = 0, tauMax = 0, fftSize = 0;
    std::shared_ptr<Fft> fft;
};

/** Removes isolated octave jumps and single-frame dropouts from a track. */
void medianSmoothPitchTrack (PitchTrack& track, int halfWindowFrames = 2);

} // namespace pb
