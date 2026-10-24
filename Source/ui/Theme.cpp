#include "Theme.h"
#include "OniriqueKnob.h"

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
    // Just short of opaque: that is what makes the menu's window one that can
    // have rounded corners.
    setColour (PopupMenu::backgroundColourId, deep2.withAlpha (0.99f));
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
    // Room is kept under the dial for the two numbers at the ends of its arc.
    const auto bounds = Rectangle<int> (x, y, width, height).toFloat().reduced (5.0f).withTrimmedBottom (9.0f);
    const auto radius = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();

    auto pointAt = [centre] (float a, float r)
    {
        return Point<float> (centre.x + r * std::sin (a), centre.y - r * std::cos (a));
    };

    // Bipolar controls fill outwards from the middle, so "no change" reads as
    // an empty arc rather than as a half-full one.
    const auto bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;

    // The ONIRIQUE knob, the same in every plugin of the brand (Source/ui/OniriqueKnob.h).
    onirique::ui::drawKnob (g, bounds, sliderPos, startAngle, endAngle, ember, bipolar, slider.isMouseOverOrDragging());

    // The range, written small at the two ends of the arc.
    const auto& properties = slider.getProperties();

    if (properties.contains ("low") && properties.contains ("high"))
    {
        g.setColour (haze);
        g.setFont (monoFont (9.0f));

        auto write = [&] (const String& text, float a)
        {
            const auto p = pointAt (a, radius);
            g.drawText (text, Rectangle<float> (40.0f, 10.0f).withCentre ({ p.x, p.y + 9.0f }),
                        Justification::centred, false);
        };

        write (properties["low"].toString(), startAngle);
        write (properties["high"].toString(), endAngle);
    }
}
void OniriqueLookAndFeel::drawButtonBackground (Graphics& g, Button& button, const Colour&,
                                                bool highlighted, bool down)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const auto radius = bounds.getHeight() * 0.5f;
    const auto on = button.getToggleState();

    // An engaged control is a lit capsule: dark red inside, a bright ember
    // edge, and a halo around it.
    if (on)
    {
        g.setColour (ember.withAlpha (0.18f));
        g.drawRoundedRectangle (bounds.expanded (2.0f), radius + 2.0f, 3.0f);

        g.setGradientFill (ColourGradient (Colour (0xff6e0f1a), 0.0f, bounds.getY(),
                                           Colour (0xff3a060c), 0.0f, bounds.getBottom(), false));
    }
    else
    {
        g.setColour (down ? Colour (0x80000000) : Colour (0x59000000));
    }

    g.fillRoundedRectangle (bounds, radius);

    g.setColour (on ? emberLit : (highlighted || down ? ember.withAlpha (0.7f) : Colour (0x33ffd2d6)));
    g.drawRoundedRectangle (bounds, radius, on ? 1.6f : 1.0f);

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
    g.setColour (on ? Colours::white : haze);
    g.setFont (capsFont (10.0f));

    auto area = button.getLocalBounds();
    area.removeFromLeft (roundToInt (area.getHeight() * 0.8f));

    g.drawText (button.getButtonText(), area.reduced (4, 0), Justification::centredLeft, false);
}

void OniriqueLookAndFeel::drawComboBox (Graphics& g, int width, int height, bool,
                                        int, int, int, int, ComboBox& box)
{
    const auto bounds = Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    const auto radius = bounds.getHeight() * 0.5f;

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
    label.setBounds (14, 0, box.getWidth() - 34, box.getHeight());
    label.setFont (capsFont (10.5f));
}

//==============================================================================
void OniriqueLookAndFeel::drawPopupMenuBackgroundWithOptions (Graphics& g, int width, int height,
                                                              const PopupMenu::Options&)
{
    const auto bounds = Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);

    g.setGradientFill (ColourGradient (Colour (0xfa3d0911), 0.0f, 0.0f,
                                       Colour (0xfa1c0508), 0.0f, (float) height, false));
    g.fillRoundedRectangle (bounds, 12.0f);

    g.setColour (Colour (0x38ffd2d6));
    g.drawRoundedRectangle (bounds, 12.0f, 1.0f);
}

void OniriqueLookAndFeel::drawPopupMenuItemWithOptions (Graphics& g, const Rectangle<int>& area, bool highlighted,
                                                        const PopupMenu::Item& item, const PopupMenu::Options&)
{
    if (item.isSeparator)
    {
        g.setColour (Colour (0x26ffd2d6));
        g.fillRect (area.reduced (14, 0).withSizeKeepingCentre (area.getWidth() - 28, 1));
        return;
    }

    const auto bounds = area.toFloat();

    if (highlighted && item.isEnabled)
    {
        g.setColour (Colour (0x1cffd2d6));
        g.fillRoundedRectangle (bounds.reduced (6.0f, 1.0f), 7.0f);
    }

    if (item.isTicked)
    {
        const auto led = Rectangle<float> (8.0f, 8.0f).withCentre ({ bounds.getX() + 20.0f, bounds.getCentreY() });

        g.setColour (ember.withAlpha (0.35f));
        g.fillEllipse (led.expanded (3.5f));
        g.setColour (emberLit);
        g.fillEllipse (led);
    }

    g.setColour ((item.isTicked ? Colours::white : mist.withAlpha (0.88f)).withMultipliedAlpha (item.isEnabled ? 1.0f : 0.35f));
    g.setFont (displayFont (13.5f, item.isTicked));
    g.drawText (item.text, bounds.withTrimmedLeft (36.0f).withTrimmedRight (12.0f),
                Justification::centredLeft, true);
}

void OniriqueLookAndFeel::drawPopupMenuSectionHeaderWithOptions (Graphics& g, const Rectangle<int>& area,
                                                                 const String& name, const PopupMenu::Options&)
{
    const auto font = capsFont (9.5f).withExtraKerningFactor (0.3f);
    const auto textWidth = GlyphArrangement::getStringWidth (font, name);
    const auto bounds = area.toFloat().withTrimmedLeft (22.0f).withTrimmedRight (22.0f);

    g.setColour (haze);
    g.setFont (font);
    g.drawText (name, bounds, Justification::centredLeft, false);

    // The rule that runs from the heading to the edge of the card.
    g.setColour (Colour (0x26ffd2d6));
    g.fillRect (bounds.withTrimmedLeft (textWidth + 12.0f).withSizeKeepingCentre (
                    jmax (0.0f, bounds.getWidth() - textWidth - 12.0f), 1.0f)
                    .withX (bounds.getX() + textWidth + 12.0f));
}

void OniriqueLookAndFeel::getIdealPopupMenuItemSizeWithOptions (const String& text, bool isSeparator, int,
                                                                int& idealWidth, int& idealHeight,
                                                                const PopupMenu::Options&)
{
    if (isSeparator)
    {
        idealWidth = 60;
        idealHeight = 13;
        return;
    }

    idealWidth = roundToInt (GlyphArrangement::getStringWidth (displayFont (13.5f), text)) + 56;
    idealHeight = 30;
}

int OniriqueLookAndFeel::getPopupMenuBorderSizeWithOptions (const PopupMenu::Options&)
{
    return 10;
}

int OniriqueLookAndFeel::getMenuWindowFlags()
{
    // No system shadow: it is square, and the card is not.
    return 0;
}

//==============================================================================
namespace
{
    constexpr int kTooltipWidth = 250;

    TextLayout layoutTooltip (const String& text)
    {
        AttributedString s;
        s.setJustification (Justification::centredLeft);
        s.append (text, displayFont (12.5f, false), mist);

        TextLayout layout;
        layout.createLayout (s, (float) kTooltipWidth);
        return layout;
    }
}

Rectangle<int> OniriqueLookAndFeel::getTooltipBounds (const String& text, Point<int> screenPos,
                                                      Rectangle<int> parentArea)
{
    const auto layout = layoutTooltip (text);
    const auto width = roundToInt (layout.getWidth()) + 22;
    const auto height = roundToInt (layout.getHeight()) + 14;

    // Below and to the right of the pointer, or on the other side when that
    // would run off the edge.
    return Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - width - 12 : screenPos.x + 18,
                           screenPos.y > parentArea.getCentreY() ? screenPos.y - height - 10 : screenPos.y + 18,
                           width, height)
               .constrainedWithin (parentArea);
}

void OniriqueLookAndFeel::drawTooltip (Graphics& g, const String& text, int width, int height)
{
    const auto bounds = Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);

    g.setGradientFill (ColourGradient (Colour (0xff3d0911), 0.0f, 0.0f, Colour (0xff1c0508), 0.0f, (float) height, false));
    g.fillRoundedRectangle (bounds, 7.0f);
    g.setColour (ember.withAlpha (0.6f));
    g.drawRoundedRectangle (bounds, 7.0f, 1.0f);

    layoutTooltip (text).draw (g, bounds.reduced (11.0f, 7.0f));
}

void OniriqueLookAndFeel::drawLinearSlider (Graphics& g, int x, int y, int width, int height, float sliderPos,
                                            float, float, Slider::SliderStyle, Slider& slider)
{
    // A thin lit bar with a round handle: the flat cousin of the knobs.
    const auto track = Rectangle<float> ((float) x, (float) y + (float) height * 0.5f - 2.0f, (float) width, 4.0f);

    g.setColour (Colour (0x26ffd2d6));
    g.fillRoundedRectangle (track, 2.0f);

    // Filled from where "no change" is, so the middle of a bipolar control
    // reads as empty.
    const auto zero = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0
                          ? (float) x + (float) width * (float) slider.valueToProportionOfLength (0.0)
                          : (float) x;

    g.setColour (slider.isMouseOverOrDragging() ? emberLit : ember);
    g.fillRoundedRectangle (track.withLeft (jmin (zero, sliderPos)).withRight (jmax (zero, sliderPos)), 2.0f);

    g.setColour (ember.withAlpha (0.3f));
    g.fillEllipse (Rectangle<float> (16.0f, 16.0f).withCentre ({ sliderPos, track.getCentreY() }));
    g.setColour (Colours::white);
    g.fillEllipse (Rectangle<float> (10.0f, 10.0f).withCentre ({ sliderPos, track.getCentreY() }));
}

void OniriqueLookAndFeel::drawScrollbar (Graphics& g, ScrollBar&, int x, int y, int width, int height,
                                         bool vertical, int thumbStart, int thumbSize, bool isMouseOver, bool isMouseDown)
{
    const auto area = Rectangle<float> ((float) x, (float) y, (float) width, (float) height);
    const auto thickness = 4.0f;
    const auto track = vertical ? area.withSizeKeepingCentre (thickness, area.getHeight() - 4.0f)
                                : area.withSizeKeepingCentre (area.getWidth() - 4.0f, thickness);

    g.setColour (Colour (0x26ffd2d6));
    g.fillRoundedRectangle (track, thickness * 0.5f);

    if (thumbSize <= 0)
        return;

    // Wider and brighter under the pointer, so it reads as something to take hold of.
    const auto lit = isMouseOver || isMouseDown;
    const auto thumbThickness = lit ? 6.0f : thickness;
    const auto thumb = vertical
        ? Rectangle<float> (area.getCentreX() - thumbThickness * 0.5f, (float) thumbStart + 1.0f, thumbThickness, (float) thumbSize - 2.0f)
        : Rectangle<float> ((float) thumbStart + 1.0f, area.getCentreY() - thumbThickness * 0.5f, (float) thumbSize - 2.0f, thumbThickness);

    g.setColour (lit ? emberLit : ember.withAlpha (0.85f));
    g.fillRoundedRectangle (thumb, thumbThickness * 0.5f);
}

Font OniriqueLookAndFeel::getLabelFont (Label& label)
{
    return label.getProperties().contains ("pill") ? monoFont (11.0f).boldened() : capsFont (10.0f);
}

void OniriqueLookAndFeel::drawLabel (Graphics& g, Label& label)
{
    if (! label.getProperties().contains ("pill"))
    {
        LookAndFeel_V4::drawLabel (g, label);
        return;
    }

    const auto bounds = label.getLocalBounds().toFloat().reduced (0.5f);
    const auto radius = bounds.getHeight() * 0.5f;

    g.setColour (Colour (0x73000000));
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (Colour (0x33ffd2d6));
    g.drawRoundedRectangle (bounds, radius, 1.0f);

    const auto led = Rectangle<float> (5.0f, 5.0f).withCentre ({ bounds.getX() + radius + 1.0f, bounds.getCentreY() });
    g.setColour (ember.withAlpha (0.35f));
    g.fillEllipse (led.expanded (2.0f));
    g.setColour (ember);
    g.fillEllipse (led);

    g.setColour (mist);
    g.setFont (getLabelFont (label));
    g.drawText (label.getText(), bounds, Justification::centred, false);
}

//==============================================================================
void TransportButton::paintButton (Graphics& g, bool highlighted, bool down)
{
    const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
    const auto side = jmin (bounds.getWidth(), bounds.getHeight());
    const auto disc = Rectangle<float> (side, side).withCentre (bounds.getCentre());
    const auto on = getToggleState();

    if (on)
    {
        g.setColour (ember.withAlpha (0.2f));
        g.drawEllipse (disc.expanded (1.5f), 3.0f);
        g.setGradientFill (ColourGradient (Colour (0xff6e0f1a), 0.0f, disc.getY(),
                                           Colour (0xff3a060c), 0.0f, disc.getBottom(), false));
    }
    else
    {
        g.setColour (down ? Colour (0x80000000) : Colour (0x59000000));
    }

    g.fillEllipse (disc);
    g.setColour (on ? emberLit : (highlighted || down ? ember.withAlpha (0.7f) : Colour (0x33ffd2d6)));
    g.drawEllipse (disc, on ? 1.6f : 1.0f);

    const auto centre = disc.getCentre();
    const auto r = side * 0.2f;

    g.setColour ((on || highlighted ? Colours::white : mist).withAlpha (isEnabled() ? 1.0f : 0.4f));

    if (kind == Kind::stop)
    {
        g.fillRoundedRectangle (Rectangle<float> (2.0f * r, 2.0f * r).withCentre (centre), 1.5f);
    }
    else if (kind == Kind::play)
    {
        Path p;
        p.addTriangle (centre.x - r * 0.8f, centre.y - r * 1.15f,
                       centre.x - r * 0.8f, centre.y + r * 1.15f,
                       centre.x + r * 1.25f, centre.y);
        g.fillPath (p);
    }
    else
    {
        // Most of a circle, with an arrowhead where it would close.
        Path arc;
        arc.addCentredArc (centre.x, centre.y, r * 1.15f, r * 1.15f, 0.0f,
                           MathConstants<float>::pi * 0.35f, MathConstants<float>::pi * 2.1f, true);
        g.strokePath (arc, PathStrokeType (1.7f, PathStrokeType::curved, PathStrokeType::rounded));

        const auto tip = Point<float> (centre.x + r * 1.15f * std::sin (MathConstants<float>::pi * 0.35f),
                                       centre.y - r * 1.15f * std::cos (MathConstants<float>::pi * 0.35f));
        Path head;
        head.addTriangle (tip.x - r * 0.75f, tip.y - r * 0.15f,
                          tip.x + r * 0.25f, tip.y - r * 0.85f,
                          tip.x + r * 0.35f, tip.y + r * 0.45f);
        g.fillPath (head);
    }
}

} // namespace onirique::theme
