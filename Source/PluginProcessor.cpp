#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace onirique
{

namespace
{
    // Set from the editor and by the presets, not by automation: they are the
    // settings of the take on screen, and a lane in the host would fight it.
    AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        const auto fixed = AudioParameterFloatAttributes().withAutomatable (false);
        const auto percent = fixed.withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100.0f)) + " %"; });
        const auto ms = fixed.withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 1000.0f)) + " ms"; });

        AudioProcessorValueTreeState::ParameterLayout layout;
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "amount", 1 }, "Amount", NormalisableRange<float> (0.0f, 1.0f), 0.0f, percent));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "speed", 1 }, "Speed", NormalisableRange<float> (0.0f, 0.25f), 0.035f, ms));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "formant", 1 }, "Formant", NormalisableRange<float> (0.7f, 1.4f), 1.0f, fixed));
        layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false, AudioParameterBoolAttributes().withAutomatable (false)));
        return layout;
    }

    enum Tags { natural = 1, pop = 2, effect = 4, colour = 8 };

    // Ways of correcting a voice, as starting points. The key and the notes
    // themselves belong to the take: a preset leaves them alone.
    std::vector<PresetBank::Stock> stockPresets()
    {
        return {
            { "Default", 0, {} },

            { "Gentle Pull", natural, { { "amount", 0.35f }, { "speed", 0.09f } } },
            { "Natural Lead", natural, { { "amount", 0.6f }, { "speed", 0.06f } } },
            { "Ballad", natural, { { "amount", 0.7f }, { "speed", 0.11f } } },
            { "Backing Stack", natural | pop, { { "amount", 0.9f }, { "speed", 0.04f } } },

            { "Studio Polish", pop, { { "amount", 0.8f }, { "speed", 0.045f } } },
            { "Pop Tight", pop, { { "amount", 0.92f }, { "speed", 0.025f } } },
            { "Slow Glide", pop | effect, { { "amount", 0.95f }, { "speed", 0.18f } } },

            { "Hard Tune", effect, { { "amount", 1.0f }, { "speed", 0.0f } } },
            { "Hard Tune Dark", effect | colour, { { "amount", 1.0f }, { "speed", 0.0f }, { "formant", 0.88f } } },

            { "Darker Voice", colour, { { "formant", 0.9f } } },
            { "Brighter Voice", colour, { { "formant", 1.1f } } },
            { "Deep Character", colour | effect, { { "amount", 0.8f }, { "speed", 0.02f }, { "formant", 0.8f } } },
            { "Small Voice", colour | effect, { { "formant", 1.28f } } },
        };
    }

    // The saved state since the bar: this, the parameters as XML, then the
    // transferred take as it was always saved. Older sessions hold the take alone.
    constexpr int kStateMagic = 0x32534250; // "PBS2"
}

PitchBoxProcessor::PitchBoxProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", AudioChannelSet::stereo(), true)
                          .withOutput ("Output", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PitchBox", createLayout()),
      presets (apvts, "PitchBox", stockPresets(), { "Natural", "Pop", "Effect", "Colour" })
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
    // Many hosts call this just before a bounce: the licence is read again then.
    license.refresh();

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
    }
    else
    {
        // Outside ARA the buffer is the only audio there is: the transfer session
        // records from it, and replaces it only where it has a take to play.
        transfer.process (buffer, position);
    }

    // Without a licence the output is replaced by noise: see License/DemoNoise.h.
    license.process (buffer, getSampleRate(), isNonRealtime());
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
    auto state = apvts.copyState();
    presets.writeState (state);

    MemoryOutputStream out (destination, false);
    out.writeInt (kStateMagic);
    out.writeString (state.toXmlString());

    // Under ARA, edits live in the audio modifications, which the host persists
    // through the document controller, so there is nothing instance-specific
    // worth storing here. A transferred take has no such home.
    if (! isBoundToARA())
    {
        MemoryBlock take;
        transfer.saveState (take);
        out.write (take.getData(), take.getSize());
    }
}

void PitchBoxProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes < 4)
        return;

    MemoryInputStream in (data, (size_t) sizeInBytes, false);

    if (in.readInt() != kStateMagic)
    {
        transfer.restoreState (data, sizeInBytes);
        return;
    }

    if (const auto xml = parseXML (in.readString()); xml != nullptr && xml->hasTagName (apvts.state.getType()))
    {
        auto state = ValueTree::fromXml (*xml);
        presets.readState (state);
        apvts.replaceState (state);
    }

    const auto at = (int) in.getPosition();

    if (at < sizeInBytes)
        transfer.restoreState (static_cast<const char*> (data) + at, sizeInBytes - at);
}

} // namespace onirique

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new onirique::PitchBoxProcessor();
}
