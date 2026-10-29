// Onirique PitchBox — a held note, heard while it is dragged
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "../model/Analysis.h"
#include "../model/EditState.h"
#include "../model/RegionRenderer.h"

#include <memory>
#include <vector>

namespace onirique
{

using namespace juce;

//==============================================================================
/** What a held note sounds like: one cycle of the corrected voice, frozen.

    A voiced note is held the way a wavetable holds a tone. One pitch period is
    lifted out of the corrected audio, resampled into a table, and read round
    and round at exactly the rate that gives the period back, so the pitch
    heard is the pitch the note has. Anything longer than one period would be
    heard for what it is, a loop.

    Built off the audio thread, so that all the audio thread does is read it.
*/
struct AuditionClip
{
    using Ptr = std::shared_ptr<const AuditionClip>;

    int64 length = 0;

    /** Table samples per output sample, at the take's own rate. */
    double increment = 1.0;

    std::vector<std::vector<float>> channels;
};

/** The samples a clip is cut from: some stretch of the take, which must reach
    from a little before the note to a little after it. */
struct AuditionTake
{
    double rate = 48000.0;
    int64 numSamples = 0;                  // the whole take
    int64 firstSample = 0;                 // where channels[c][0] is in the take
    int64 length = 0;                      // samples in each channel
    std::vector<const float*> channels;
};

//==============================================================================
/** Makes the clips, on the message thread. Shared by the transfer session and
    the ARA editor. */
class AuditionBuilder
{
public:
    /** How much of the take a clip needs beyond the note, either side, on top
        of the renderer's own padding. */
    static int64 marginSamples (double rate) noexcept   { return (int64) (0.135 * rate) + 1; }

    /** The clip for the note held at `centre`, or nullptr when there is none.
        A build that sounds exactly like the last one gives that one back, so
        the player has nothing to cross-fade. */
    AuditionClip::Ptr build (const AuditionTake&, const pb::Analysis::Ptr&, const pb::EditState&,
                             int64 noteStartIn, int64 noteEndIn, int64 centreIn);

    /** A new note is grabbed. By now the audio thread has long let go of the
        clips before last. */
    void startNewNote();

    int64 paddingSamples (const pb::EditState& edits) const noexcept   { return renderer.paddingSamples (edits.settings.formantRatio); }

private:
    pb::RegionRenderer renderer;
    pb::Analysis::Ptr rendererAnalysis;
    double rendererRate = 0.0;

    std::vector<AuditionClip::Ptr> clips, olderClips;
};

//==============================================================================
/** Plays the held clip, on the audio thread. A new clip fades in while the one
    it replaces fades out, and letting go fades the note out. */
class AuditionPlayer
{
public:
    /** Adds the note to the buffer. `rateRatio` is take samples per output
        sample. Allocates nothing and takes no lock. */
    void mix (AudioBuffer<float>&, const AuditionClip::Ptr& clip, bool wanted,
              double outputRate, double rateRatio) noexcept;

    bool isSounding() const noexcept   { return held.gain > 0.0f || fading.gain > 0.0f; }

    /** Cuts the note off at once: playback has taken over. */
    void silence() noexcept   { held.gain = 0.0f; fading.gain = 0.0f; }

private:
    struct Voice
    {
        AuditionClip::Ptr clip;
        double position = 0.0;
        float gain = 0.0f;
    };

    static void mixVoice (Voice&, AudioBuffer<float>&, bool wanted, double outputRate, double rateRatio) noexcept;

    Voice held, fading;
};

} // namespace onirique
