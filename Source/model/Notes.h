// Onirique PitchBox — editable note model (the "blobs")
#pragma once

#include "../dsp/PitchTrack.h"
#include "../dsp/Scale.h"

#include <cstdint>
#include <vector>

namespace pb
{

/** One detected note, plus whatever the user has done to it.

    The analysis fields (start/end/centsDetected) are owned by the analysis and
    are never written by the editor. The edit fields below them are the only
    state the UI changes, and they are what gets serialised into the host
    session, so a note survives a reload even if the audio is re-analysed.
*/
struct Note
{
    // ---- analysis, read-only once set
    int64_t startSample   = 0;
    int64_t endSample     = 0;
    float   centsDetected = 0.0f;  // median pitch of the note, cents above MIDI 0
    float   peakRms       = 0.0f;
    int     firstFrame    = 0;     // index into PitchTrack::frames
    int     lastFrame     = 0;     // exclusive

    // ---- edit state
    /** Where the note should sit. Initialised to the detected pitch, so a fresh
        analysis is a no-op until the user moves something. */
    float targetCents = 0.0f;
    /** How far the note is pulled from where it was sung to targetCents.
        0 leaves the take alone, 1 puts it exactly on target. */
    float correctAmount = 0.0f;
    /** Scales the pitch movement *within* the note — vibrato, scoops, drift.
        1 keeps the performance, 0 flattens it to a dead straight line,
        >1 exaggerates it. */
    float vibratoScale = 1.0f;
    /** Per-note gain in dB, applied to the rendered grains. */
    float gainDb = 0.0f;
    /** Excludes the note from correction entirely (for a deliberate bend). */
    bool bypassed = false;

    int64_t lengthSamples() const noexcept { return endSample - startSample; }
    double  midi()          const noexcept { return centsDetected / 100.0; }
    /** Where the note ends up once correction is applied. */
    float   resultCents()   const noexcept
    {
        return bypassed ? centsDetected
                        : centsDetected + correctAmount * (targetCents - centsDetected);
    }
};

//==============================================================================

struct NoteDetectorParams
{
    /** A pitch change larger than this, sustained, starts a new note. */
    float   splitCents = 65.0f;
    /** Shortest note we are willing to emit. Anything briefer is absorbed into
        its neighbour — these are consonants and pitch-tracker noise. */
    double  minNoteSeconds = 0.045;
    /** A voiced gap shorter than this does not break a note in two. */
    double  maxGapSeconds = 0.030;
    /** Frames below this fraction of the note's peak RMS are treated as decay
        and do not contribute to the median pitch. */
    float   quietFraction = 0.12f;
};

/** Segments a pitch track into notes. Pure function of the analysis: running it
    twice on the same track gives the same notes, which is what lets us
    re-analyse without losing the user's edits. */
std::vector<Note> detectNotes (const PitchTrack& track,
                               const NoteDetectorParams& params = {});

/** Weighted pitch-class histogram over the whole track, for key detection. */
std::array<float, 12> pitchClassHistogram (const std::vector<Note>& notes);

//==============================================================================

/** Global controls that sit above the per-note edits. */
struct CorrectionSettings
{
    Scale scale;
    /** Applied to every note that the user has not touched. This is the
        "Scale Assistant" behaviour: snap to the scale by this much. */
    float globalAmount = 0.0f;
    /** Seconds for the correction to take effect at a note boundary. 0 is the
        hard, audible snap; 40-80 ms sounds like a singer finding the note. */
    double transitionSeconds = 0.035;
    /** Moves the spectral envelope independently of pitch. 1 leaves it alone. */
    float formantRatio = 1.0f;
    bool  enabled = true;
};

/** Sets every note's target to the nearest pitch allowed by the scale, and its
    correction amount to the global amount. Notes the user has already edited
    (touched) are left alone. */
void applyScaleToNotes (std::vector<Note>& notes,
                        const CorrectionSettings& settings,
                        const std::vector<bool>& touched);

/** Builds the per-sample frequency ratio that PSOLA consumes.

    This is the one place where notes, the original pitch track and the global
    settings are combined, and it is called both by the offline preview and by
    the real-time renderer — so what you hear on playback is by construction the
    same thing the editor drew.

    @param track      the analysis
    @param notes      current note state
    @param settings   global correction settings
    @param first      first output sample to fill (in audio-source samples)
    @param count      number of samples to fill
    @param ratioOut   buffer of length count, receives target/source ratios
*/
void buildPitchRatioCurve (const PitchTrack& track,
                           const std::vector<Note>& notes,
                           const CorrectionSettings& settings,
                           int64_t first,
                           int64_t count,
                           float* ratioOut);

/** Per-sample linear gain from the per-note gainDb, same contract as above. */
void buildGainCurve (const std::vector<Note>& notes,
                     const CorrectionSettings& settings,
                     int64_t first,
                     int64_t count,
                     float* gainOut);

/** Index of the note containing the sample, or -1. Notes are sorted and
    non-overlapping, so this is a binary search. */
int noteAtSample (const std::vector<Note>& notes, int64_t sample) noexcept;

} // namespace pb
