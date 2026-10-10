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
    prepareToPlayForARA (sampleRate, samplesPerBlock, getMainBusNumOutputChannels(), getProcessingPrecision());
}

void PitchBoxProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;

    for (int i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    // In an ARA host the renderer produces the whole output from the take, so
    // whatever arrived in the buffer is irrelevant. Outside ARA there is no
    // take at all: pass the signal through rather than silently dropping it.
    if (! processBlockForARA (buffer, isNonRealtime() ? Realtime::no : Realtime::yes, getPlayHead()))
    {
        // Nothing to do: the input is already the output.
    }
}

AudioProcessorEditor* PitchBoxProcessor::createEditor()
{
    return new PitchBoxEditor (*this);
}

void PitchBoxProcessor::getStateInformation (MemoryBlock&)
{
    // Edits live in the ARA audio modifications, which the host persists
    // through the document controller, so there is nothing instance-specific
    // worth storing here.
}

void PitchBoxProcessor::setStateInformation (const void*, int)
{
}

} // namespace onirique

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new onirique::PitchBoxProcessor();
}
