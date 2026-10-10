// Renders the editor to a PNG without a host, with a take transferred into it,
// so a change to the interface can be looked at straight from the build.
//
//   pitchbox_snapshot out.png [width height]
//   pitchbox_snapshot --menu out.png      the drop-down menu on its own
#include "../Source/PluginProcessor.h"
#include "../Source/dsp/Scale.h"
#include "../Source/ui/Theme.h"
#include "../tests/TestSignal.h"

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;
constexpr juce::int64 kSongStart = 96000;

struct FixedPlayHead final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override { return info; }
    PositionInfo info;
};

std::vector<float> makeTake()
{
    std::vector<float> src;
    const std::vector<double> midi { 57, 60, 62, 64, 62, 60, 65, 64, 62, 60, 59, 57 };

    for (size_t i = 0; i < midi.size(); ++i)
    {
        const double detune = ((double) ((i * 37) % 71) - 35.0);
        auto note = pbtest::makeVoice (kSr, 0.26, pb::midiToHz (midi[i] + detune / 100.0), 30.0, 5.4);
        const int fade = (int) (0.012 * kSr);

        for (int j = 0; j < fade; ++j)
        {
            const float g = (float) j / (float) fade;
            note[(size_t) j] *= g;
            note[note.size() - 1 - (size_t) j] *= g;
        }

        src.insert (src.end(), note.begin(), note.end());
        src.insert (src.end(), (size_t) (0.03 * kSr), 0.0f);
    }

    return src;
}

/** Walks the plugin through a transfer on the message thread, one timer tick
    at a time, then takes the picture and ends the program. */
}

namespace
{
struct Director final : juce::Timer
{
    Director (juce::File destination, int width, int height) : file (std::move (destination))
    {
        processor.reset (static_cast<onirique::PitchBoxProcessor*> (createPluginFilter()));
        processor->setPlayHead (&head);
        processor->setPlayConfigDetails (2, 2, kSr, kBlock);
        processor->prepareToPlay (kSr, kBlock);

        editor.reset (processor->createEditor());
        editor->setSize (width, height);

        startTimer (50);
    }

    void play (const std::vector<float>& audio)
    {
        juce::AudioBuffer<float> buffer (2, kBlock);
        juce::MidiBuffer midi;

        for (size_t pos = 0; pos < audio.size(); pos += kBlock)
        {
            const auto count = (int) std::min ((size_t) kBlock, audio.size() - pos);
            juce::AudioBuffer<float> block (buffer.getArrayOfWritePointers(), 2, count);

            block.copyFrom (0, 0, audio.data() + pos, count);
            block.copyFrom (1, 0, audio.data() + pos, count);

            head.info.setIsPlaying (true);
            head.info.setTimeInSamples (kSongStart + (juce::int64) pos);
            processor->processBlock (block, midi);
        }

        stoppedAt (kSongStart + (juce::int64) audio.size());
    }

    void stoppedAt (juce::int64 position)
    {
        juce::AudioBuffer<float> block (2, kBlock);
        juce::MidiBuffer midi;
        block.clear();

        head.info.setIsPlaying (false);
        head.info.setTimeInSamples (position);
        processor->processBlock (block, midi);
    }

    void timerCallback() override
    {
        auto& session = processor->getTransfer();

        switch (step)
        {
            case 0:
                take = makeTake();
                session.toggleTransfer();
                play (take);
                ++step;
                break;

            case 1:
                if (session.getState() == onirique::TransferSession::State::idle
                    && ! session.isAnalysing() && session.getAnalysis() != nullptr)
                {
                    auto edits = session.getEdits();
                    edits.settings.scale.root = 9;
                    edits.settings.scale.type = pb::ScaleType::naturalMinor;
                    edits.settings.globalAmount = 0.8f;
                    edits.refreshUntouched();
                    session.setEdits (edits);

                    // Leave the playhead a third of the way in.
                    stoppedAt (kSongStart + (juce::int64) take.size() / 3);
                    ++step;
                }
                else if (++waited > 400)
                {
                    finish();
                }
                break;

            default:
                // A few ticks for the editor's own timer to catch up.
                if (++step > 12)
                    finish();
                break;
        }
    }

    void finish()
    {
        stopTimer();

        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 1.0f);

        file.deleteFile();

        if (auto stream = file.createOutputStream())
            juce::PNGImageFormat().writeImageToStream (image, *stream);

        juce::MessageManager::getInstance()->stopDispatchLoop();
    }

    juce::File file;
    FixedPlayHead head;
    std::unique_ptr<onirique::PitchBoxProcessor> processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::vector<float> take;
    int step = 0, waited = 0;
};
}

/** Lays a menu out the way JUCE would and draws it with the plugin's look, a
    real pop-up being a window of its own that a snapshot cannot reach. */
bool writeMenu (const juce::File& file)
{
    onirique::theme::OniriqueLookAndFeel look;
    const juce::PopupMenu::Options options;

    std::vector<juce::PopupMenu::Item> items;

    {
        juce::PopupMenu::Item heading ("KEY");
        heading.isSectionHeader = true;
        items.push_back (heading);
    }

    for (int i = 0; i < 12; ++i)
    {
        juce::PopupMenu::Item item (pb::noteName (i));
        item.itemID = i + 1;
        item.isTicked = (i == 9);
        items.push_back (item);
    }

    const auto border = look.getPopupMenuBorderSizeWithOptions (options);
    int width = 150, height = 2 * border;
    std::vector<int> heights;

    for (const auto& item : items)
    {
        int w = 0, h = 0;
        look.getIdealPopupMenuItemSizeWithOptions (item.text, item.isSeparator, 20, w, h, options);
        width = std::max (width, w);
        heights.push_back (h);
        height += h;
    }

    // Over the dark of the editor, as it will be seen.
    juce::Image image (juce::Image::RGB, width + 40, height + 40, true);
    // The drawing only reaches the image once its Graphics has gone.
    {
        juce::Graphics g (image);
        g.fillAll (onirique::theme::night);
        g.setOrigin (20, 20);
        look.drawPopupMenuBackgroundWithOptions (g, width, height, options);

        int y = border;

        for (size_t i = 0; i < items.size(); ++i)
        {
            const juce::Rectangle<int> area (0, y, width, heights[i]);

            if (items[i].isSectionHeader)
                look.drawPopupMenuSectionHeaderWithOptions (g, area, items[i].text, options);
            else
                look.drawPopupMenuItemWithOptions (g, area, i == 4, items[i], options);

            y += heights[i];
        }
    }

    file.deleteFile();

    if (auto stream = file.createOutputStream())
        return juce::PNGImageFormat().writeImageToStream (image, *stream);

    return false;
}

int main (int argc, char** argv)
{
    if (argc < 2)
        return 2;

    const juce::ScopedJuceInitialiser_GUI gui;

    if (juce::String (argv[1]) == "--menu")
        return argc > 2 && writeMenu (juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (argv[2]))) ? 0 : 1;

    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (argv[1]));
    const auto width = argc > 3 ? juce::String (argv[2]).getIntValue() : 1160;
    const auto height = argc > 3 ? juce::String (argv[3]).getIntValue() : 760;

    {
        Director director (file, width, height);
        juce::MessageManager::getInstance()->runDispatchLoop();
    }

    return file.existsAsFile() ? 0 : 1;
}
