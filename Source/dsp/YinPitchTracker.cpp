#include "YinPitchTracker.h"

#include <algorithm>
#include <cmath>

namespace pb
{

YinPitchTracker::YinPitchTracker (const Params& p)
    : params (p)
{
    tauMin = std::max (2, (int) std::floor (params.sampleRate / params.maxHz));
    tauMax = (int) std::ceil (params.sampleRate / params.minHz);

    // The window must hold at least two of the longest periods we search for.
    params.windowSize = std::max (params.windowSize, 2 * tauMax);
    tauMax = std::min (tauMax, params.windowSize / 2);

    fftSize = nextPowerOfTwo (params.windowSize + tauMax + 1);
    fft = std::make_shared<Fft> (fftSize);
}

PitchTrack YinPitchTracker::process (const float* x,
                                     int64_t numSamples,
                                     const std::function<bool (float)>& progress) const
{
    PitchTrack track;
    track.sampleRate = params.sampleRate;
    track.hopSize    = params.hopSize;

    const int W = params.windowSize;
    if (numSamples < (int64_t) (W + tauMax))
        return track;

    // Prefix sums of x^2 so the two energy terms of the difference function are O(1).
    std::vector<double> cumPow ((size_t) numSamples + 1, 0.0);
    for (int64_t i = 0; i < numSamples; ++i)
        cumPow[(size_t) i + 1] = cumPow[(size_t) i] + (double) x[i] * (double) x[i];

    const int64_t lastStart = numSamples - (W + tauMax);
    const int64_t numFrames = lastStart / params.hopSize + 1;
    track.frames.reserve ((size_t) numFrames);

    std::vector<std::complex<float>> bufA ((size_t) fftSize), bufB ((size_t) fftSize);
    std::vector<double> diff ((size_t) tauMax + 1), cmnd ((size_t) tauMax + 1);

    const double silenceRms = std::pow (10.0, params.silenceDb / 20.0);

    for (int64_t frameIdx = 0; frameIdx < numFrames; ++frameIdx)
    {
        const int64_t start = frameIdx * params.hopSize;

        if (progress && (frameIdx % 256) == 0)
            if (! progress ((float) ((double) frameIdx / (double) numFrames)))
                break;

        // --- cross-correlation r(tau) = sum_j x[start+j] * x[start+tau+j], j < W
        std::fill (bufA.begin(), bufA.end(), std::complex<float> (0.0f, 0.0f));
        std::fill (bufB.begin(), bufB.end(), std::complex<float> (0.0f, 0.0f));

        for (int j = 0; j < W; ++j)
            bufA[(size_t) j] = std::complex<float> (x[start + j], 0.0f);

        const int longLen = W + tauMax;
        for (int j = 0; j < longLen; ++j)
            bufB[(size_t) j] = std::complex<float> (x[start + j], 0.0f);

        fft->forward (bufA);
        fft->forward (bufB);

        for (size_t i = 0; i < bufA.size(); ++i)
            bufA[i] = std::conj (bufA[i]) * bufB[i];

        fft->inverse (bufA);

        // --- difference function
        const double powFrame = cumPow[(size_t) (start + W)] - cumPow[(size_t) start];

        for (int tau = 0; tau <= tauMax; ++tau)
        {
            const double powLag = cumPow[(size_t) (start + tau + W)] - cumPow[(size_t) (start + tau)];
            const double corr   = (double) bufA[(size_t) tau].real();
            diff[(size_t) tau] = std::max (0.0, powFrame + powLag - 2.0 * corr);
        }

        // --- cumulative mean normalised difference
        cmnd[0] = 1.0;
        double running = 0.0;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            running += diff[(size_t) tau];
            cmnd[(size_t) tau] = running > 0.0 ? diff[(size_t) tau] * (double) tau / running : 1.0;
        }

        // --- absolute threshold: first local minimum below the threshold
        int bestTau = -1;
        for (int tau = tauMin; tau < tauMax; ++tau)
        {
            if (cmnd[(size_t) tau] < (double) params.threshold)
            {
                int t = tau;
                while (t + 1 < tauMax && cmnd[(size_t) (t + 1)] < cmnd[(size_t) t])
                    ++t;
                bestTau = t;
                break;
            }
        }

        if (bestTau < 0)
        {
            // Nothing crossed the threshold: fall back to the global minimum so we
            // still report a (low-confidence) candidate.
            double best = 1e30;
            for (int tau = tauMin; tau < tauMax; ++tau)
            {
                if (cmnd[(size_t) tau] < best)
                {
                    best = cmnd[(size_t) tau];
                    bestTau = tau;
                }
            }
        }

        PitchFrame frame;
        frame.sampleIndex = start + W / 2;
        frame.rms = (float) std::sqrt (powFrame / (double) W);

        if (bestTau > 0)
        {
            // Parabolic refinement of the period around the chosen lag.
            double refined = (double) bestTau;

            if (bestTau > 0 && bestTau < tauMax)
            {
                const double a = cmnd[(size_t) bestTau - 1];
                const double b = cmnd[(size_t) bestTau];
                const double c = cmnd[(size_t) bestTau + 1];
                const double denom = a - 2.0 * b + c;
                if (std::abs (denom) > 1e-12)
                    refined = (double) bestTau + 0.5 * (a - c) / denom;
            }

            refined = std::clamp (refined, (double) tauMin, (double) tauMax);

            frame.f0Hz = (float) (params.sampleRate / refined);
            frame.confidence = (float) std::clamp (1.0 - cmnd[(size_t) bestTau], 0.0, 1.0);
            frame.voiced = frame.confidence >= params.voicedConfidence
                           && frame.rms > (float) silenceRms
                           && frame.f0Hz >= params.minHz
                           && frame.f0Hz <= params.maxHz;
        }

        if (! frame.voiced)
            frame.f0Hz = 0.0f;

        track.frames.push_back (frame);
    }

    return track;
}

//==============================================================================
void medianSmoothPitchTrack (PitchTrack& track, int halfWindowFrames)
{
    if (track.frames.size() < 3 || halfWindowFrames < 1)
        return;

    const auto original = track.frames;
    std::vector<float> window;

    for (size_t i = 0; i < track.frames.size(); ++i)
    {
        if (! original[i].voiced)
            continue;

        window.clear();
        const int64_t lo = (int64_t) i - halfWindowFrames;
        const int64_t hi = (int64_t) i + halfWindowFrames;

        for (int64_t j = std::max<int64_t> (0, lo);
             j <= std::min<int64_t> ((int64_t) original.size() - 1, hi);
             ++j)
        {
            if (original[(size_t) j].voiced)
                window.push_back (original[(size_t) j].f0Hz);
        }

        if (window.size() < 2)
            continue;

        std::sort (window.begin(), window.end());
        const float median = window[window.size() / 2];

        // Only correct gross errors (octave jumps); leave genuine detail alone.
        const float ratio = track.frames[i].f0Hz / median;
        if (ratio > 1.4f || ratio < 0.72f)
            track.frames[i].f0Hz = median;
    }
}

} // namespace pb
