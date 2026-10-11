#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace onirique
{

PitchBoxProcessor::PitchBoxProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", AudioChannelSet::stereo(), true)
                          .withOutput ("Output", AudioChannelSet::stereo(), true))
{
}

bool PitchBoxProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();

    if (out != AudioChannelSet::mono() && out != AudioChannelSet::stereo())
        return false;

    return layouts.getMainInputChannelSet() == out;
}

void PitchBoxProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    preparedRate = sampleRate;
    preparedBlock = samplesPerBlock;

    prepareToPlayForARA (sampleRate, samplesPerBlock, getMainBusNumOutputChannels(), getProcessingPrecision());
    transfer.prepare (sampleRate, samplesPerBlock, getMainBusNumOutputChannels());
}

void PitchBoxProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;

    for (int i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    Optional<AudioPlayHead::PositionInfo> position;

    if (auto* head = getPlayHead())
        position = head->getPosition();

    if (position.hasValue())
    {
        if (const auto time = position->getTimeInSamples())
            transportPosition.store (*time);

        transportPlaying.store (position->getIsPlaying());
    }

    // In an ARA host the renderer produces the whole output from the take, so
    // whatever arrived in the buffer is irrelevant.
    if (isBoundToARA())
    {
        processBlockForARA (buffer, isNonRealtime() ? Realtime::no : Realtime::yes, getPlayHead());
        return;
    }

    // Outside ARA the buffer is the only audio there is: the transfer session
    // records from it, and replaces it only where it has a take to play.
    transfer.process (buffer, position);
}

void PitchBoxProcessor::didBindToARA() noexcept
{
    AudioProcessorARAExtension::didBindToARA();

    if (preparedRate > 0.0 && preparedBlock > 0)
        prepareToPlayForARA (preparedRate, preparedBlock, getMainBusNumOutputChannels(), getProcessingPrecision());
}

AudioProcessorEditor* PitchBoxProcessor::createEditor()
{
    return new PitchBoxEditor (*this);
}

void PitchBoxProcessor::getStateInformation (MemoryBlock& destination)
{
    // Under ARA, edits live in the audio modifications, which the host persists
    // through the document controller, so there is nothing instance-specific
    // worth storing here. A transferred take has no such home.
    if (! isBoundToARA())
        transfer.saveState (destination);
}

void PitchBoxProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    transfer.restoreState (data, sizeInBytes);
}

} // namespace onirique

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new onirique::PitchBoxProcessor();
}
