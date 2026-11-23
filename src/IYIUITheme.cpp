// Builds the stock IYIUI themes (modern, classic, classic dark) and resolves style classes on a UITheme.
// Moved verbatim out of IYIUI.cpp; no drawing or input lives here.

#include "IYI/IYIUI/IYIUI.h"
#include <stdexcept>

namespace IYI::UI {

// HR's Modern UI: dark surfaces in the classic themes' navy (title 10,36,106; dark primary 30,58,128), thin
// lines, small corners and solid offset shadows instead of blurred ones. game_kit.theme restyles every class
// its games use on top of this.
UITheme CreateModernTheme()
{
    UITheme t;
    const Color ink{ 225, 225, 228, 255 }, line{ 70, 76, 92, 255 }, navy{ 30, 58, 128, 255 };
    const Color inset{ 22, 24, 30, 255 }, shadow{ 0, 0, 0, 140 };
    auto box = [&](Color bg, float radius, float pad, bool shadowed) {
        UIStyle s;
        s.BackgroundColor = bg;
        s.BorderColor = line;
        s.BorderWidth = 1.0f;
        s.CornerRadius = radius;
        if (shadowed) { s.ShadowColor = shadow; s.ShadowOffset = Vector2{ 3.0f, 3.0f }; s.ShadowSize = 0.0f; }
        s.PaddingLeft = s.PaddingRight = s.PaddingTop = s.PaddingBottom = pad;
        return s;
    };
    auto label = [&](Color c, float size) {
        UIStyle s;
        s.BackgroundColor = Color{ 0, 0, 0, 0 };
        s.TextColor = c;
        s.FontSize = size;
        return s;
    };
    t.Styles["panel"] = box(Color{ 34, 37, 45, 245 }, 2.0f, 12.0f, true);
    t.Styles["card"] = box(Color{ 42, 46, 56, 250 }, 2.0f, 16.0f, true);
    t.Styles["text"] = label(ink, 18.0f);
    t.Styles["title"] = label(Color{ 255, 255, 255, 255 }, 24.0f);
    t.Styles["image"] = label(ink, 0.0f);

    UIStyle button = box(Color{ 52, 56, 68, 255 }, 2.0f, 0.0f, false);
    button.TextColor = ink;
    button.FontSize = 18.0f;
    button.TransitionDuration = 0.1f;
    button.Hover.HasBackground = true; button.Hover.Background = Color{ 64, 70, 86, 255 };
    button.Hover.HasBorder = true; button.Hover.Border = Color{ 110, 130, 190, 255 };
    button.Pressed.HasBackground = true; button.Pressed.Background = Color{ 38, 41, 50, 255 };
    button.Pressed.HasBorder = true; button.Pressed.Border = Color{ 110, 130, 190, 255 };
    button.Pressed.ScaleMul = 0.98f;
    button.Disabled.HasBackground = true; button.Disabled.Background = Color{ 40, 43, 52, 255 };
    button.Disabled.HasText = true; button.Disabled.Text = Color{ 150, 152, 160, 255 };
    button.Disabled.HasBorder = true; button.Disabled.Border = Color{ 56, 60, 72, 255 };
    t.Styles["button"] = button;

    UIStyle primary = button;
    primary.BackgroundColor = navy;
    primary.TextColor = Color{ 255, 255, 255, 255 };
    primary.BorderColor = Color{ 70, 100, 180, 255 };
    primary.Hover.Background = Color{ 48, 80, 160, 255 };
    primary.Pressed.Background = Color{ 20, 40, 96, 255 };
    t.Styles["button_primary"] = primary;

    t.Styles["progress"] = box(inset, 2.0f, 0.0f, false);
    UIStyle fill = box(Color{ 60, 100, 200, 255 }, 2.0f, 0.0f, false);
    fill.BorderWidth = 0.0f;
    t.Styles["progress_fill"] = fill;
    t.Styles["slider"] = box(inset, 2.0f, 0.0f, false);
    t.Styles["slider_fill"] = fill;
    UIStyle knob = box(Color{ 200, 204, 214, 255 }, 2.0f, 0.0f, false);
    knob.Hover.HasBackground = true; knob.Hover.Background = Color{ 230, 232, 238, 255 };
    knob.Pressed.HasBackground = true; knob.Pressed.Background = Color{ 170, 175, 190, 255 };
    t.Styles["slider_knob"] = knob;
    t.Styles["scroll_view"] = box(Color{ 28, 30, 37, 230 }, 0.0f, 0.0f, false);

    UIStyle input = box(inset, 2.0f, 0.0f, false);
    input.TextColor = ink;
    input.FontSize = 18.0f;
    input.PaddingLeft = input.PaddingRight = 8.0f;
    input.PaddingTop = input.PaddingBottom = 6.0f;
    input.TransitionDuration = 0.1f;
    input.Hover.HasBorder = true; input.Hover.Border = Color{ 110, 130, 190, 255 };
    input.Disabled = button.Disabled;
    t.Styles["text_input"] = input;

    return t;
}

UITheme CreateClassicTheme()
{
    UITheme t;

    // panel
    UIStyle panel;
    panel.BackgroundColor = Color{ 192, 192, 192, 255 };
    panel.Beveled         = true;
    panel.BevelSunken     = false;
    panel.CornerRadius    = 0.0f;
    panel.PaddingLeft = panel.PaddingRight = panel.PaddingTop = panel.PaddingBottom = 6.0f;
    t.Styles["panel"] = panel;

    // card
    UIStyle card = panel;
    card.BackgroundColor = Color{ 212, 208, 200, 255 };
    t.Styles["card"] = card;

    // text
    UIStyle text;
    text.BackgroundColor = Color{ 0, 0, 0, 0 };
    text.TextColor       = Color{ 0, 0, 0, 255 };
    text.FontSize        = 16.0f;
    t.Styles["text"] = text;

    // title
    UIStyle title;
    title.BackgroundColor = Color{ 10, 36, 106, 255 };
    title.TextColor       = Color{ 255, 255, 255, 255 };
    title.FontSize        = 18.0f;
    title.PaddingLeft = title.PaddingRight = 6.0f;
    title.PaddingTop = title.PaddingBottom = 3.0f;
    t.Styles["title"] = title;

    // image
    UIStyle img;
    img.BackgroundColor = Color{ 0, 0, 0, 0 };
    t.Styles["image"] = img;

    // button
    UIStyle button;
    button.BackgroundColor    = Color{ 192, 192, 192, 255 };
    button.TextColor          = Color{ 0, 0, 0, 255 };
    button.Beveled            = true;
    button.BevelSunken        = false;
    button.CornerRadius       = 0.0f;
    button.FontSize           = 16.0f;
    button.TransitionDuration = 0.0f;
    button.Hover.HasBackground = true; button.Hover.Background = Color{ 205, 205, 205, 255 };
    button.Pressed.HasBackground = true; button.Pressed.Background = Color{ 180, 180, 180, 255 };
    button.Disabled.HasBackground = true; button.Disabled.Background = Color{ 192, 192, 192, 255 };
    button.Disabled.HasText       = true; button.Disabled.Text       = Color{ 128, 128, 128, 255 };
    t.Styles["button"] = button;

    // button_primary
    UIStyle buttonPrimary = button;
    buttonPrimary.TextColor = Color{ 0, 0, 128, 255 };
    buttonPrimary.Disabled.HasText = true; buttonPrimary.Disabled.Text = Color{ 128, 128, 128, 255 };
    t.Styles["button_primary"] = buttonPrimary;

    // progress
    UIStyle progress;
    progress.BackgroundColor = Color{ 180, 180, 180, 255 };
    progress.Beveled         = true;
    progress.BevelSunken     = true;
    progress.CornerRadius    = 0.0f;
    t.Styles["progress"] = progress;

    // progress_fill
    UIStyle progressFill;
    progressFill.BackgroundColor = Color{ 10, 36, 106, 255 };
    progressFill.CornerRadius    = 0.0f;
    t.Styles["progress_fill"] = progressFill;

    // slider
    UIStyle slider;
    slider.BackgroundColor = Color{ 180, 180, 180, 255 };
    slider.Beveled         = true;
    slider.BevelSunken     = true;
    slider.CornerRadius    = 0.0f;
    t.Styles["slider"] = slider;

    // slider_fill
    UIStyle sliderFill;
    sliderFill.BackgroundColor = Color{ 10, 36, 106, 255 };
    sliderFill.CornerRadius    = 0.0f;
    t.Styles["slider_fill"] = sliderFill;

    // slider_knob
    UIStyle sliderKnob;
    sliderKnob.BackgroundColor = Color{ 192, 192, 192, 255 };
    sliderKnob.Beveled         = true;
    sliderKnob.BevelSunken     = false;
    sliderKnob.CornerRadius    = 0.0f;
    t.Styles["slider_knob"] = sliderKnob;

    // scroll_view
    UIStyle sv;
    sv.BackgroundColor = Color{ 255, 255, 255, 255 };
    sv.Beveled         = true;
    sv.BevelSunken     = true;
    sv.CornerRadius    = 0.0f;
    t.Styles["scroll_view"] = sv;

    // text_input
    UIStyle textInput;
    textInput.BackgroundColor = Color{ 255, 255, 255, 255 };
    textInput.TextColor       = Color{ 0, 0, 0, 255 };
    textInput.Beveled         = true;
    textInput.BevelSunken     = true;
    textInput.CornerRadius    = 0.0f;
    textInput.FontSize        = 16.0f;
    textInput.PaddingLeft = textInput.PaddingRight = 4.0f;
    textInput.PaddingTop = textInput.PaddingBottom = 2.0f;
    textInput.Disabled.HasBackground = true; textInput.Disabled.Background = Color{ 212, 208, 200, 255 };
    textInput.Disabled.HasText       = true; textInput.Disabled.Text       = Color{ 128, 128, 128, 255 };
    t.Styles["text_input"] = textInput;

    return t;
}

// Tradeoff: unmapped colors pass through unchanged and will look wrong,
// which is the intended loud failure mode if Classic gains a new color.
static Color MapSurfaceColor(Color c)
{
    if (c.a == 0) return c;
    if (c.r == 192 && c.g == 192 && c.b == 192) return Color{ 58, 58, 62, c.a };
    if (c.r == 212 && c.g == 208 && c.b == 200) return Color{ 48, 48, 52, c.a };
    if (c.r == 205 && c.g == 205 && c.b == 205) return Color{ 72, 72, 76, c.a };
    if (c.r == 180 && c.g == 180 && c.b == 180) return Color{ 38, 38, 42, c.a };
    if (c.r == 255 && c.g == 255 && c.b == 255) return Color{ 30, 30, 34, c.a };
    if (c.r == 10  && c.g == 36  && c.b == 106) return Color{ 30, 58, 128, c.a };
    return c;
}

static Color MapTextColor(Color c)
{
    if (c.a == 0) return c;
    if (c.r == 0   && c.g == 0   && c.b == 0)   return Color{ 225, 225, 228, c.a };
    if (c.r == 128 && c.g == 128 && c.b == 128) return Color{ 124, 124, 130, c.a };
    if (c.r == 255 && c.g == 255 && c.b == 255) return Color{ 255, 255, 255, c.a };
    if (c.r == 0   && c.g == 0   && c.b == 128) return Color{ 150, 180, 255, c.a };
    if (c.r == 10  && c.g == 36  && c.b == 106) return Color{ 30, 58, 128, c.a };
    return c;
}

static Color MapBorderColor(Color c)
{
    if (c.a == 0) return c;
    if (c.r == 10 && c.g == 36 && c.b == 106) return Color{ 30, 58, 128, c.a };
    return Color{ 24, 24, 28, c.a };
}

static Color MapShadowColor(Color c)
{
    if (c.a == 0) return c;
    float newAlpha = static_cast<float>(c.a) * 1.4f;
    unsigned char a = (newAlpha > 255.0f) ? 255 : static_cast<unsigned char>(newAlpha);
    return Color{ c.r, c.g, c.b, a };
}

static void RemapStyleState(UIStyleState& state)
{
    state.Background = MapSurfaceColor(state.Background);
    state.Border     = MapBorderColor(state.Border);
    state.Text       = MapTextColor(state.Text);
}

UITheme CreateClassicDarkTheme()
{
    UITheme t = CreateClassicTheme();
    for (auto& [name, style] : t.Styles)
    {
        style.BackgroundColor = MapSurfaceColor(style.BackgroundColor);
        style.TextColor       = MapTextColor(style.TextColor);
        style.BorderColor     = MapBorderColor(style.BorderColor);
        style.ShadowColor     = MapShadowColor(style.ShadowColor);

        style.BevelLight      = Color{ 255, 255, 255, 60 };
        style.BevelDark       = Color{ 0, 0, 0, 150 };

        RemapStyleState(style.Hover);
        RemapStyleState(style.Pressed);
        RemapStyleState(style.Disabled);
    }
    return t;
}


UIStyle& UITheme::Get(const std::string& styleClass)
{
    return Styles[styleClass];
}

const UIStyle& UITheme::Resolve(const std::string& styleClass) const
{
    static const UIStyle s_defaultStyle;
    if (styleClass.empty()) return s_defaultStyle;
    auto it = Styles.find(styleClass);
    if (it != Styles.end()) return it->second;
    return s_defaultStyle;
}

UIStyle& UITheme::Extend(const std::string& styleClass, const std::string& base)
{
    auto it = Styles.find(base);
    if (it == Styles.end()) throw std::invalid_argument("extend: no style class '" + base + "'");
    UIStyle copy = it->second;
    return Styles[styleClass] = std::move(copy);
}

void UITheme::ApplyTypeScale(float textScale)
{
    for (auto& [_, style] : Styles) {
        if (style.FontRole.empty()) continue;
        auto size = TypeScale.find(style.FontRole);
        if (size != TypeScale.end()) style.FontSize = size->second * textScale;
    }
}

} // namespace IYI::UI
