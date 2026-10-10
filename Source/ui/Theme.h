// Onirique PitchBox — shared look, matching the Onirique identity
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace onirique::theme
{

using juce::Colour;

// The palette the rest of the Onirique range uses: a near-black ground, a
// blood-dark panel, and a single ember accent that carries every active state.
inline const Colour night   { 0xff0d0809 };
inline const Colour deep    { 0xff1a080a };
inline const Colour deep2   { 0xff2a0a0e };
inline const Colour blood   { 0xff5c0008 };
inline const Colour ember   { 0xffe23444 };
inline const Colour emberLit{ 0xffff5a68 };
inline const Colour mist    { 0xfff6eeee };
inline const Colour bone    { 0xffffecee };
inline const Colour haze    { 0xffa88f92 };
inline const Colour line    { 0x21ffd2d6 };

inline juce::Font displayFont (float height, bool heavy = true)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultSansSerifFontName(), height,
                                          heavy ? juce::Font::bold : juce::Font::plain));
}

inline juce::Font monoFont (float height)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height,
                                          juce::Font::plain));
}

/** The ember-arc knob and segmented controls used across the range. */
class OniriqueLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    OniriqueLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                           juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool highlighted, bool down) override;

    void drawButtonText (juce::Graphics&, juce::TextButton&, bool, bool) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool,
                       int, int, int, int, juce::ComboBox&) override;

    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    juce::Font getLabelFont (juce::Label&) override;
};

} // namespace onirique::theme
