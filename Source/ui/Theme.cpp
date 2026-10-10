#include "Theme.h"

namespace onirique::theme
{

using namespace juce;

OniriqueLookAndFeel::OniriqueLookAndFeel()
{
    setColour (ResizableWindow::backgroundColourId, night);
    setColour (Label::textColourId, mist);
    setColour (ComboBox::backgroundColourId, Colour (0x59000000));
    setColour (ComboBox::textColourId, mist);
    setColour (ComboBox::outlineColourId, line);
    setColour (ComboBox::arrowColourId, ember);
    setColour (PopupMenu::backgroundColourId, deep2);
    setColour (PopupMenu::textColourId, mist);
    setColour (PopupMenu::highlightedBackgroundColourId, ember);
    setColour (PopupMenu::highlightedTextColourId, Colours::white);
    setColour (TextButton::buttonColourId, Colour (0x4d000000));
    setColour (TextButton::buttonOnColourId, ember);
    setColour (TextButton::textColourOffId, haze);
    setColour (TextButton::textColourOnId, Colours::white);
}

void OniriqueLookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int width, int height,
                                            float sliderPos, float startAngle, float endAngle,
                                            Slider& slider)
{
    const auto bounds = Rectangle<int> (x, y, width, height).toFloat().reduced (4.0f);
    const auto radius = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto angle = startAngle + sliderPos * (endAngle - startAngle);
    const auto thickness = jmax (2.0f, radius * 0.17f);
    const auto arcRadius = radius - thickness * 0.5f;

    // Body: a dark sphere lit from the upper left, as on the rest of the range.
    g.setGradientFill (ColourGradient (Colour (0xff4a2a2e),
                                       centre.x - radius * 0.3f, centre.y - radius * 0.4f,
                                       Colour (0xff0b0506),
                                       centre.x + radius * 0.6f, centre.y + radius * 0.7f,
                                       true));
    g.fillEllipse (bounds.reduced (thickness));

    Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
    g.setColour (line);
    g.strokePath (track, PathStrokeType (thickness, PathStrokeType::curved, PathStrokeType::rounded));

    // Bipolar controls fill outwards from the middle, so "no change" reads as
    // an empty arc rather than as a half-full one.
    const auto bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
    const auto originAngle = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;

    Path arc;
    arc.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                       jmin (originAngle, angle), jmax (originAngle, angle), true);

    g.setColour (slider.isMouseOverOrDragging() ? emberLit : ember);
    g.strokePath (arc, PathStrokeType (thickness, PathStrokeType::curved, PathStrokeType::rounded));

    Path pointer;
    const auto pointerLength = radius * 0.52f;
    pointer.startNewSubPath (centre.x, centre.y - radius * 0.14f);
    pointer.lineTo (centre.x, centre.y - pointerLength);
    pointer.applyTransform (AffineTransform::rotation (angle, centre.x, centre.y));

    g.setColour (bone);
    g.strokePath (pointer, PathStrokeType (jmax (1.5f, radius * 0.1f), PathStrokeType::curved,
                                           PathStrokeType::rounded));
}

void OniriqueLookAndFeel::drawButtonBackground (Graphics& g, Button& button, const Colour&,
                                                bool highlighted, bool down)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const auto radius = bounds.getHeight() * 0.5f;
    const auto on = button.getToggleState();

    g.setColour (on ? ember.withAlpha (0.22f) : Colour (0x4d000000));
    g.fillRoundedRectangle (bounds, radius);

    if (on)
    {
        g.setColour (ember.withAlpha (0.5f));
        g.drawRoundedRectangle (bounds.expanded (1.5f), radius + 1.5f, 2.0f);
    }

    g.setColour (on ? ember : (highlighted || down ? ember.withAlpha (0.6f) : line));
    g.drawRoundedRectangle (bounds, radius, 1.0f);

    // The little LED that marks an engaged control on every Onirique plugin.
    const auto led = Rectangle<float> (7.0f, 7.0f)
                         .withCentre ({ bounds.getX() + radius * 0.9f, bounds.getCentreY() });

    g.setColour (on ? ember : Colour (0x33ffd2d6));
    g.fillEllipse (led);

    if (on)
    {
        g.setColour (ember.withAlpha (0.35f));
        g.fillEllipse (led.expanded (3.0f));
    }
}

void OniriqueLookAndFeel::drawButtonText (Graphics& g, TextButton& button, bool, bool)
{
    const auto on = button.getToggleState();
    g.setColour (on ? mist : haze);
    g.setFont (monoFont (11.0f));

    auto area = button.getLocalBounds();
    area.removeFromLeft (roundToInt (area.getHeight() * 0.8f));

    g.drawText (button.getButtonText(), area.reduced (4, 0), Justification::centredLeft, false);
}

void OniriqueLookAndFeel::drawComboBox (Graphics& g, int width, int height, bool,
                                        int, int, int, int, ComboBox& box)
{
    const auto bounds = Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    const auto radius = 6.0f;

    g.setColour (Colour (0x59000000));
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (box.isMouseOver() ? ember.withAlpha (0.6f) : line);
    g.drawRoundedRectangle (bounds, radius, 1.0f);

    Path arrow;
    const auto cx = bounds.getRight() - 14.0f;
    const auto cy = bounds.getCentreY();
    arrow.startNewSubPath (cx - 4.0f, cy - 2.0f);
    arrow.lineTo (cx, cy + 2.5f);
    arrow.lineTo (cx + 4.0f, cy - 2.0f);

    g.setColour (ember);
    g.strokePath (arrow, PathStrokeType (1.6f, PathStrokeType::curved, PathStrokeType::rounded));
}

void OniriqueLookAndFeel::positionComboBoxText (ComboBox& box, Label& label)
{
    label.setBounds (10, 0, box.getWidth() - 28, box.getHeight());
    label.setFont (monoFont (12.0f));
}

Font OniriqueLookAndFeel::getLabelFont (Label&)
{
    return monoFont (11.0f);
}

} // namespace onirique::theme
