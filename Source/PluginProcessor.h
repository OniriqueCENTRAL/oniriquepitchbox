// Onirique PitchBox — plugin entry point
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "ara/PitchBoxARA.h"
#include "transfer/TransferSession.h"

namespace onirique
{

/** The plugin instance.

    There is very little here, and that is the point: in an ARA plugin the real
    work belongs to the document controller (which owns the takes and their
    analyses) and to the playback renderer (which turns them back into audio).
    This class mostly routes a block to the renderer. When the host is not an
    ARA host there is no take to be handed, so the block goes to the transfer
    session instead, which records the passage itself and then plays it back
    corrected.
*/
class PitchBoxProcessor final : public AudioProcessor,
                                public AudioProcessorARAExtension
{
public:
    PitchBoxProcessor();
    ~PitchBoxProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (AudioBuffer<float>&, MidiBuffer&) override;

    AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override     { return false; }
    bool producesMidi() const override    { return false; }
    bool isMidiEffect() const override    { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override            { return 1; }
    int getCurrentProgram() override         { return 0; }
    void setCurrentProgram (int) override    {}
    const String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const String&) override {}

    void getStateInformation (MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    AudioProcessorARAExtension* getARAClientExtensions() override { return this; }

    /** A host is supposed to bind the plugin to ARA before it prepares it to
        play. One that does it the other way round has prepared a plugin with
        no renderer yet, and the renderer it then gets would never be prepared
        at all; so it is prepared here, with what the host said earlier. */
    void didBindToARA() noexcept override;

    /** True when the host gave us ARA access. Without it there is no take to
        edit, and the editor says so instead of showing an empty canvas. */
    bool hasARAAccess() const noexcept { return isBoundToARA(); }

    /** Where the host's transport was at the last block, for the editor's
        playhead. Returns whether it was running. */
    bool getTransport (int64& positionInSamples) const noexcept
    {
        positionInSamples = transportPosition.load();
        return transportPlaying.load();
    }

    /** Where the take lives in a host without ARA. */
    TransferSession& getTransfer() noexcept { return transfer; }

private:
    TransferSession transfer;
    double preparedRate = 0.0;
    int preparedBlock = 0;
    std::atomic<int64> transportPosition { 0 };
    std::atomic<bool> transportPlaying { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PitchBoxProcessor)
};

} // namespace onirique
