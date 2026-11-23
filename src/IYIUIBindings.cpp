// Registers IYIUI subsystem types, widgets, styles, themes, and UICanvas actor with pybind11.
// Defines the 'engine.ui' submodule and binds all UI classes and functions. A world actor asks
// engine.ui.pointer_over_ui() before treating a click, drag or wheel as a world action.

#include <string>
#include <vector>
#include <memory>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "raylib.h"
#include "IYI/Application.h"
#include "IYI/Log.h"
#include "IYI/Video.h"
#include "IYI/Scene.h"
#include "IYI/ActorRegistry.h"
#include "IYI/IYIUI/IYIUI.h"
#include "IYI/IYIUI/IYIUITween.h"

namespace py = pybind11;

// The one place an alignment name falls back: an unknown one ("middle" vs "center") warns instead of
// silently snapping to the default.
static void WarnUnknownAlign(const char* property, const std::string& value, const char* fallback)
{
    if (value != fallback)
        LogWrite(LogLevel::Warn, "ui", std::string(property) + ": unknown value '" + value + "', using '" + fallback + "'");
}

namespace IYI {

using namespace IYI::UI;

// True while a Python wrapper is registered for this address. Retired elements are reclaimed only when this
// is false for every node of the subtree (UICanvas::ReclaimRetired). Runs on the main thread holding the GIL.
static bool HasPythonWrapper(const void* e)
{
    return py::detail::with_instance_map(e, [&](py::detail::instance_map& m) { return m.find(e) != m.end(); });
}

// A removed actor is freed only once this is false (ActorRegistry::RemoveDestroyed); a canvas also waits for
// every wrapped element of its tree. Lives here because it needs both the actor and the UI types.
static bool ActorHasPythonReference(const GameActor* a)
{
    const auto* canvas = dynamic_cast<const UICanvas*>(a);
    return HasPythonWrapper(a) || (canvas && canvas->AnyElementReferenced());
}

void GuardDeadActorAccess(py::handle type); // EngineBindings.cpp

void RegisterIYIUIBindings(py::module_& m)
{
    SetUIElementReferencedHook([](const UIElement* e) { return HasPythonWrapper(e); });
    SetActorReferencedHook(&ActorHasPythonReference);
    py::module_ ui = m.def_submodule("ui", "IYI UI subsystem");

    // UIStyleState
    py::class_<UIStyleState>(ui, "UIStyleState")
        .def_property("background_color",
            [](const UIStyleState& s) { return s.Background; },
            [](UIStyleState& s, Color c) { s.Background = c; s.HasBackground = true; })
        .def_property("text_color",
            [](const UIStyleState& s) { return s.Text; },
            [](UIStyleState& s, Color c) { s.Text = c; s.HasText = true; })
        .def_property("border_color",
            [](const UIStyleState& s) { return s.Border; },
            [](UIStyleState& s, Color c) { s.Border = c; s.HasBorder = true; })
        .def_readwrite("scale_mul", &UIStyleState::ScaleMul)
        .def("__repr__", [](const UIStyleState& s) {
            return "<UIStyleState scale_mul=" + std::to_string(s.ScaleMul) + ">";
        });

    // UIStyle
    py::class_<UIStyle>(ui, "UIStyle")
        .def(py::init<>())
        .def_readwrite("background_color", &UIStyle::BackgroundColor)
        .def_readwrite("text_color", &UIStyle::TextColor)
        .def_readwrite("border_color", &UIStyle::BorderColor)
        .def_readwrite("border_width", &UIStyle::BorderWidth)
        .def_readwrite("corner_radius", &UIStyle::CornerRadius)
        .def_readwrite("shadow_color", &UIStyle::ShadowColor)
        .def_readwrite("shadow_offset", &UIStyle::ShadowOffset)
        .def_readwrite("shadow_size", &UIStyle::ShadowSize)
        .def_property("padding",
            [](const UIStyle& s) { return s.PaddingLeft; },
            [](UIStyle& s, float p) { s.PaddingLeft = s.PaddingRight = s.PaddingTop = s.PaddingBottom = p; })
        .def_readwrite("padding_left", &UIStyle::PaddingLeft)
        .def_readwrite("padding_right", &UIStyle::PaddingRight)
        .def_readwrite("padding_top", &UIStyle::PaddingTop)
        .def_readwrite("padding_bottom", &UIStyle::PaddingBottom)
        .def_readwrite("font_size", &UIStyle::FontSize)
        .def_readwrite("font_role", &UIStyle::FontRole, "a theme.type_scale name: font_size = that size x canvas.text_scale")
        .def_readwrite("use_npatch", &UIStyle::UseNPatch)
        .def_property("npatch_texture", [](UIStyle& s) { return s.NPatchTexture; }, [](UIStyle& s, const Texture2D* t) { s.NPatchTexture = t; }, py::return_value_policy::reference)
        .def_readwrite("npatch_left", &UIStyle::NPatchLeft)
        .def_readwrite("npatch_top", &UIStyle::NPatchTop)
        .def_readwrite("npatch_right", &UIStyle::NPatchRight)
        .def_readwrite("npatch_bottom", &UIStyle::NPatchBottom)
        .def_readwrite("beveled", &UIStyle::Beveled)
        .def_readwrite("bevel_sunken", &UIStyle::BevelSunken)
        .def_readwrite("bevel_light", &UIStyle::BevelLight)
        .def_readwrite("bevel_dark", &UIStyle::BevelDark)
        .def_readwrite("hover", &UIStyle::Hover)
        .def_readwrite("pressed", &UIStyle::Pressed)
        .def_readwrite("disabled", &UIStyle::Disabled)
        .def_readwrite("transition_duration", &UIStyle::TransitionDuration)
        .def("__repr__", [](const UIStyle&) {
            return "<UIStyle>";
        });

    // UITheme
    py::class_<UITheme>(ui, "UITheme")
        .def(py::init<>())
        .def("get", &UITheme::Get, py::arg("style_class"), py::return_value_policy::reference_internal)
        .def("extend", &UITheme::Extend, py::arg("style_class"), py::arg("base"), py::return_value_policy::reference_internal,
             "a new style class copied from base (a copy, not a live link)")
        .def_readwrite("type_scale", &UITheme::TypeScale, "{role: size}; assign the whole dict")
        .def_property("default_font",
            [](const UITheme& t) -> const Font* { return t.HasDefaultFont ? &t.DefaultFont : nullptr; },
            [](UITheme& t, const Font& f) { t.DefaultFont = f; t.HasDefaultFont = true; },
            py::return_value_policy::reference)
        .def_property_readonly("style_classes", [](const UITheme& t) {
            std::vector<std::string> classes;
            for (const auto& [k, _] : t.Styles) {
                classes.push_back(k);
            }
            return classes;
        })
        .def("__repr__", [](const UITheme& t) {
            return "<UITheme styles=" + std::to_string(t.Styles.size()) + ">";
        });

    // UITweenHandle (Phase 2)
    py::class_<UITweenHandle>(ui, "UITweenHandle")
        .def_property_readonly("active", &UITweenHandle::Active)
        .def_property_readonly("completed", &UITweenHandle::Completed)
        .def_property_readonly("progress", &UITweenHandle::Progress)
        .def("delay", [](UITweenHandle& self, std::optional<float> d) -> py::object {
            if (d.has_value()) {
                self.Delay(d.value());
                return py::cast(self);
            }
            return py::cast(self.GetDelay());
        }, py::arg("seconds") = py::none())
        .def("loops", [](UITweenHandle& self, std::optional<int> count) -> py::object {
            if (count.has_value()) {
                self.Loops(count.value());
                return py::cast(self);
            }
            return py::cast(self.GetLoops());
        }, py::arg("count") = py::none())
        .def("yoyo", [](UITweenHandle& self, std::optional<bool> enable) -> py::object {
            if (enable.has_value()) {
                self.Yoyo(enable.value());
                return py::cast(self);
            }
            return py::cast(self.GetYoyo());
        }, py::arg("enable") = py::none())
        .def("kill", &UITweenHandle::Kill)
        .def("pause", &UITweenHandle::Pause)
        .def("resume", &UITweenHandle::Resume)
        .def("__repr__", [](const UITweenHandle& h) {
            return "<UITweenHandle id=" + std::to_string(h.TweenId) + ">";
        });

    // UISequenceHandle (Phase 2)
    py::class_<UISequenceHandle>(ui, "UISequenceHandle")
        .def("append_move", &UISequenceHandle::AppendMove, py::arg("element"), py::arg("x"), py::arg("y"), py::arg("duration"), py::arg("ease") = "linear")
        .def("join_move", &UISequenceHandle::JoinMove, py::arg("element"), py::arg("x"), py::arg("y"), py::arg("duration"), py::arg("ease") = "linear")
        .def("append_size", &UISequenceHandle::AppendSize, py::arg("element"), py::arg("w"), py::arg("h"), py::arg("duration"), py::arg("ease") = "linear")
        .def("join_size", &UISequenceHandle::JoinSize, py::arg("element"), py::arg("w"), py::arg("h"), py::arg("duration"), py::arg("ease") = "linear")
        .def("append_scale", &UISequenceHandle::AppendScale, py::arg("element"), py::arg("sx"), py::arg("sy"), py::arg("duration"), py::arg("ease") = "linear")
        .def("join_scale", &UISequenceHandle::JoinScale, py::arg("element"), py::arg("sx"), py::arg("sy"), py::arg("duration"), py::arg("ease") = "linear")
        .def("append_rotate", &UISequenceHandle::AppendRotate, py::arg("element"), py::arg("degrees"), py::arg("duration"), py::arg("ease") = "linear")
        .def("join_rotate", &UISequenceHandle::JoinRotate, py::arg("element"), py::arg("degrees"), py::arg("duration"), py::arg("ease") = "linear")
        .def("append_fade", &UISequenceHandle::AppendFade, py::arg("element"), py::arg("opacity"), py::arg("duration"), py::arg("ease") = "linear")
        .def("join_fade", &UISequenceHandle::JoinFade, py::arg("element"), py::arg("opacity"), py::arg("duration"), py::arg("ease") = "linear")
        .def("append_color", &UISequenceHandle::AppendColor, py::arg("element"), py::arg("color"), py::arg("duration"), py::arg("ease") = "linear")
        .def("join_color", &UISequenceHandle::JoinColor, py::arg("element"), py::arg("color"), py::arg("duration"), py::arg("ease") = "linear")
        .def("append_text_color", &UISequenceHandle::AppendTextColor, py::arg("element"), py::arg("color"), py::arg("duration"), py::arg("ease") = "linear")
        .def("join_text_color", &UISequenceHandle::JoinTextColor, py::arg("element"), py::arg("color"), py::arg("duration"), py::arg("ease") = "linear")
        .def("append_interval", &UISequenceHandle::AppendInterval, py::arg("seconds"))
        .def("play", &UISequenceHandle::Play)
        .def("kill", &UISequenceHandle::Kill)
        .def_property_readonly("active", &UISequenceHandle::Active)
        .def_property_readonly("completed", &UISequenceHandle::Completed)
        .def("__repr__", [](const UISequenceHandle& h) {
            return "<UISequenceHandle id=" + std::to_string(h.SequenceId) + ">";
        });

    // UIElement
    py::class_<UIElement, std::unique_ptr<UIElement, py::nodelete>>(ui, "UIElement")
        .def_readonly("id", &UIElement::Id)
        .def_readonly("name", &UIElement::Name)
        .def_property_readonly("type_name", &UIElement::TypeName)
        .def_property_readonly("parent", [](UIElement& self) -> UIElement* {
            return self.Parent;
        }, py::return_value_policy::reference)
        .def_property_readonly("children", [](UIElement& self) {
            std::vector<UIElement*> list;
            for (const auto& c : self.Children) list.push_back(c.get());
            return list;
        }, py::return_value_policy::reference)
        .def_readwrite("visible", &UIElement::Visible)
        .def_readwrite("enabled", &UIElement::Enabled)
        .def_readwrite("opacity", &UIElement::Opacity)
        .def_readwrite("focusable", &UIElement::Focusable)
        .def_readwrite("style_class", &UIElement::StyleClass)
        .def_readwrite("font_group", &UIElement::FontGroup, "texts of one group draw at one size (the smallest fit)")
        .def_readwrite("clip_children", &UIElement::ClipChildren)
        // RectTransform
        .def_property("anchor_min", [](UIElement& self) { return self.Rect.AnchorMin; }, [](UIElement& self, Vector2 v) { self.Rect.AnchorMin = v; })
        .def_property("anchor_max", [](UIElement& self) { return self.Rect.AnchorMax; }, [](UIElement& self, Vector2 v) { self.Rect.AnchorMax = v; })
        .def_property("pivot", [](UIElement& self) { return self.Rect.Pivot; }, [](UIElement& self, Vector2 v) { self.Rect.Pivot = v; })
        .def_property("anchored_position", [](UIElement& self) { return self.Rect.AnchoredPosition; }, [](UIElement& self, Vector2 v) { self.Rect.AnchoredPosition = v; })
        .def_property("size_delta", [](UIElement& self) { return self.Rect.SizeDelta; }, [](UIElement& self, Vector2 v) { self.Rect.SizeDelta = v; })
        .def_property("min_size", [](UIElement& self) { return self.Rect.MinSize; }, [](UIElement& self, Vector2 v) { self.Rect.MinSize = v; })
        .def_property("max_size", [](UIElement& self) { return self.Rect.MaxSize; }, [](UIElement& self, Vector2 v) { self.Rect.MaxSize = v; })
        .def_property("scale", [](UIElement& self) { return self.Rect.Scale; }, [](UIElement& self, Vector2 v) { self.Rect.Scale = v; })
        .def_property("rotation", [](UIElement& self) { return self.Rect.Rotation; }, [](UIElement& self, float r) { self.Rect.Rotation = r; })
        .def_property("translation", [](UIElement& self) { return self.Rect.Translation; }, [](UIElement& self, Vector2 v) { self.Rect.Translation = v; })
        .def_readwrite("flex_weight", &UIElement::FlexWeight)
        .def_readwrite("ignore_layout", &UIElement::IgnoreLayout)
        .def_readwrite("fit_height", &UIElement::FitHeight, "height follows the text or the children")
        .def_readwrite("wrap_children", &UIElement::WrapChildren, "horizontal layout: children flow onto new rows")
        // Layout container
        .def_property("layout_mode",
            [](const UIElement& self) -> std::string {
                switch (self.Layout) {
                    case UILayoutMode::Horizontal: return "horizontal";
                    case UILayoutMode::Vertical: return "vertical";
                    default: return "none";
                }
            },
            [](UIElement& self, const std::string& mode) {
                if (mode == "horizontal") self.Layout = UILayoutMode::Horizontal;
                else if (mode == "vertical") self.Layout = UILayoutMode::Vertical;
                else self.Layout = UILayoutMode::None;
            })
        .def_property("padding",
            [](const UIElement& self) { return self.PaddingLeft; },
            [](UIElement& self, float p) { self.PaddingLeft = self.PaddingRight = self.PaddingTop = self.PaddingBottom = p; })
        .def_readwrite("padding_left", &UIElement::PaddingLeft)
        .def_readwrite("padding_right", &UIElement::PaddingRight)
        .def_readwrite("padding_top", &UIElement::PaddingTop)
        .def_readwrite("padding_bottom", &UIElement::PaddingBottom)
        .def_readwrite("spacing", &UIElement::Spacing)
        .def_property("child_alignment",
            [](const UIElement& self) -> std::string {
                switch (self.ChildAlignment) {
                    case UIAlign::Center: return "center";
                    case UIAlign::End: return "end";
                    default: return "start";
                }
            },
            [](UIElement& self, const std::string& align) {
                if (align == "center") self.ChildAlignment = UIAlign::Center;
                else if (align == "end") self.ChildAlignment = UIAlign::End;
                else { WarnUnknownAlign("child_alignment", align, "start"); self.ChildAlignment = UIAlign::Start; }
            })
        .def_readwrite("expand_children_width", &UIElement::ExpandChildrenWidth)
        .def_readwrite("expand_children_height", &UIElement::ExpandChildrenHeight)
        .def_readwrite("stretch_children_cross_axis", &UIElement::StretchChildrenCrossAxis)
        // Resolved
        .def_readonly("resolved_rect", &UIElement::ResolvedRect)
        .def_readonly("resolved_opacity", &UIElement::ResolvedOpacity)
        // Tree Operations
        .def_readonly("alive", &UIElement::Alive)
        .def("remove", &UIElement::Remove)
        .def("clear_children", &UIElement::ClearChildren)
        .def("bring_to_front", &UIElement::BringToFront)
        .def("send_to_back", &UIElement::SendToBack)
        .def_property_readonly("style", &UIElement::GetOrCreateCustomStyle, py::return_value_policy::reference_internal)
        // Tween helpers (Phase 2)
        .def("move_to", &UIElement::MoveTo, py::arg("x"), py::arg("y"), py::arg("duration"), py::arg("ease") = "linear")
        .def("translate_to", &UIElement::TranslateTo, py::arg("x"), py::arg("y"), py::arg("duration"), py::arg("ease") = "linear")
        .def("size_to", &UIElement::SizeTo, py::arg("w"), py::arg("h"), py::arg("duration"), py::arg("ease") = "linear")
        .def("scale_to", &UIElement::ScaleTo, py::arg("sx"), py::arg("sy"), py::arg("duration"), py::arg("ease") = "linear")
        .def("rotate_to", &UIElement::RotateTo, py::arg("degrees"), py::arg("duration"), py::arg("ease") = "linear")
        .def("fade_to", &UIElement::FadeTo, py::arg("opacity"), py::arg("duration"), py::arg("ease") = "linear")
        .def("color_to", &UIElement::ColorTo, py::arg("color"), py::arg("duration"), py::arg("ease") = "linear")
        .def("text_color_to", &UIElement::TextColorTo, py::arg("color"), py::arg("duration"), py::arg("ease") = "linear")
        .def("__repr__", [](const UIElement& self) {
            return "<" + self.TypeName() + " " + std::to_string(self.Id) + " '" + self.Name + "'>";
        });

    // Widgets
    py::class_<UIPanel, UIElement, std::unique_ptr<UIPanel, py::nodelete>>(ui, "UIPanel")
        .def_readwrite("blocks_input_behind", &UIPanel::BlocksInputBehind);

    py::class_<UIText, UIElement, std::unique_ptr<UIText, py::nodelete>>(ui, "UIText")
        .def_readwrite("text", &UIText::Text)
        .def_readwrite("loc_key", &UIText::LocKey, "empty = use text normally; set = resolve from the current language")
        .def_readwrite("loc_args", &UIText::LocArgs, "assign a whole dict; in-place item assignment does not persist")
        .def_property("font", [](UIText& self) -> Font* { return self.FontOverride; }, [](UIText& self, Font* f) { self.FontOverride = f; }, py::return_value_policy::reference)
        .def_readwrite("font_size", &UIText::FontSize)
        .def_readwrite("color", &UIText::ColorOverride)
        .def_readwrite("wrap", &UIText::Wrap)
        .def_readwrite("auto_shrink", &UIText::AutoShrink)
        .def_readwrite("min_font_size", &UIText::MinFontSize)
        .def_property_readonly("fitted_font_size", &UIText::FittedFontSize)
        .def_property_readonly("measured_width", &UIText::MeasuredWidth)
        .def_property_readonly("measured_height", &UIText::MeasuredHeight)
        .def_property("horizontal_alignment",
            [](const UIText& self) -> std::string {
                switch (self.HAlign) {
                    case UIHAlign::Center: return "center";
                    case UIHAlign::Right: return "right";
                    default: return "left";
                }
            },
            [](UIText& self, const std::string& a) {
                if (a == "center") self.HAlign = UIHAlign::Center;
                else if (a == "right") self.HAlign = UIHAlign::Right;
                else { WarnUnknownAlign("horizontal_alignment", a, "left"); self.HAlign = UIHAlign::Left; }
            })
        .def_property("vertical_alignment",
            [](const UIText& self) -> std::string {
                switch (self.VAlign) {
                    case UIVAlign::Middle: return "middle";
                    case UIVAlign::Bottom: return "bottom";
                    default: return "top";
                }
            },
            [](UIText& self, const std::string& a) {
                if (a == "middle") self.VAlign = UIVAlign::Middle;
                else if (a == "bottom") self.VAlign = UIVAlign::Bottom;
                else { WarnUnknownAlign("vertical_alignment", a, "top"); self.VAlign = UIVAlign::Top; }
            });

    py::class_<UIImage, UIElement, std::unique_ptr<UIImage, py::nodelete>>(ui, "UIImage")
        .def_property("texture", [](UIImage& s) { return s.Texture; }, [](UIImage& s, const Texture2D* t) { s.Texture = t; }, py::return_value_policy::reference)
        .def_readwrite("tint", &UIImage::Tint)
        .def_readwrite("source_rect", &UIImage::SourceRect)
        .def_readwrite("preserve_aspect", &UIImage::PreserveAspect)
        .def_readwrite("video", &UIImage::VideoSource, "an engine.video Video shown instead of texture (None: off)")
        .def_readwrite("integer_scale", &UIImage::IntegerScale);

    py::class_<UIButton, UIElement, std::unique_ptr<UIButton, py::nodelete>>(ui, "UIButton")
        .def_readwrite("text", &UIButton::Text)
        .def_readwrite("loc_key", &UIButton::LocKey, "empty = use text normally; set = resolve from the current language")
        .def_readwrite("loc_args", &UIButton::LocArgs, "assign a whole dict; in-place item assignment does not persist")
        .def_readwrite("icon", &UIButton::Icon)
        .def_property("icon_texture", [](UIButton& s) { return s.IconTexture; }, [](UIButton& s, const Texture2D* t) { s.IconTexture = t; }, py::return_value_policy::reference)
        .def_readwrite("wrap", &UIButton::Wrap)
        .def_readwrite("auto_shrink", &UIButton::AutoShrink)
        .def_readwrite("min_font_size", &UIButton::MinFontSize)
        .def_readwrite("hit_scale", &UIButton::HitScale)
        .def_property_readonly("fitted_font_size", &UIButton::FittedFontSize)
        .def_property_readonly("measured_width", &UIButton::MeasuredWidth)
        .def_property_readonly("measured_height", &UIButton::MeasuredHeight)
        .def_readonly("hovered", &UIButton::Hovered)
        .def_readonly("pressed", &UIButton::Pressed)
        .def_readonly("clicked", &UIButton::Clicked);

    py::class_<UIProgressBar, UIElement, std::unique_ptr<UIProgressBar, py::nodelete>>(ui, "UIProgressBar")
        .def_readwrite("value", &UIProgressBar::Value)
        .def_property("fill_direction",
            [](const UIProgressBar& self) -> std::string {
                switch (self.FillDirection) {
                    case UIFillDirection::RightToLeft: return "right_to_left";
                    case UIFillDirection::TopToBottom: return "top_to_bottom";
                    case UIFillDirection::BottomToTop: return "bottom_to_top";
                    default: return "left_to_right";
                }
            },
            [](UIProgressBar& self, const std::string& dir) {
                if (dir == "right_to_left") self.FillDirection = UIFillDirection::RightToLeft;
                else if (dir == "top_to_bottom") self.FillDirection = UIFillDirection::TopToBottom;
                else if (dir == "bottom_to_top") self.FillDirection = UIFillDirection::BottomToTop;
                else self.FillDirection = UIFillDirection::LeftToRight;
            });

    py::class_<UISlider, UIElement, std::unique_ptr<UISlider, py::nodelete>>(ui, "UISlider")
        .def_readwrite("value", &UISlider::Value)
        .def_readwrite("min_value", &UISlider::MinValue)
        .def_readwrite("max_value", &UISlider::MaxValue)
        .def_readwrite("step", &UISlider::Step)
        .def_readwrite("vertical", &UISlider::Vertical)
        .def_readonly("hovered", &UISlider::Hovered)
        .def_readonly("pressed", &UISlider::Pressed)
        .def_readonly("changed", &UISlider::Changed);

    py::class_<UIScrollView, UIElement, std::unique_ptr<UIScrollView, py::nodelete>>(ui, "UIScrollView")
        .def_readwrite("scroll_offset", &UIScrollView::ScrollOffset)
        .def_readonly("content_height", &UIScrollView::ContentHeight);

    py::class_<UITextInput, UIElement, std::unique_ptr<UITextInput, py::nodelete>>(ui, "UITextInput")
        .def_readwrite("text", &UITextInput::Text)
        .def_readwrite("placeholder", &UITextInput::Placeholder)
        .def_readwrite("allow_letters", &UITextInput::AllowLetters)
        .def_readwrite("allow_digits", &UITextInput::AllowDigits)
        .def_readwrite("allow_symbols", &UITextInput::AllowSymbols)
        .def_readonly("hovered", &UITextInput::Hovered)
        .def_readonly("pressed", &UITextInput::Pressed)
        .def_readonly("submitted", &UITextInput::Submitted);

    // UICanvas
    py::class_<UICanvas, GameActor, std::unique_ptr<UICanvas, py::nodelete>>(ui, "UICanvas")
        .def_property_readonly("root", &UICanvas::Root, py::return_value_policy::reference)
        .def("add_panel", &UICanvas::AddPanel, py::arg("name") = "Panel", py::arg("parent") = nullptr, py::return_value_policy::reference)
        .def("add_text", &UICanvas::AddText, py::arg("name") = "Text", py::arg("parent") = nullptr, py::return_value_policy::reference)
        .def("add_image", &UICanvas::AddImage, py::arg("name") = "Image", py::arg("parent") = nullptr, py::return_value_policy::reference)
        .def("add_button", &UICanvas::AddButton, py::arg("name") = "Button", py::arg("parent") = nullptr, py::return_value_policy::reference)
        .def("add_progress_bar", &UICanvas::AddProgressBar, py::arg("name") = "ProgressBar", py::arg("parent") = nullptr, py::return_value_policy::reference)
        .def("add_slider", &UICanvas::AddSlider, py::arg("name") = "Slider", py::arg("parent") = nullptr, py::return_value_policy::reference)
        .def("add_scroll_view", &UICanvas::AddScrollView, py::arg("name") = "ScrollView", py::arg("parent") = nullptr, py::return_value_policy::reference)
        .def("add_text_input", &UICanvas::AddTextInput,
             py::arg("name") = "TextInput",
             py::arg("parent") = nullptr,
             py::arg("placeholder") = "",
             py::arg("allow_letters") = true,
             py::arg("allow_digits") = true,
             py::arg("allow_symbols") = true,
             py::return_value_policy::reference)
        .def("find", &UICanvas::Find, py::arg("name"), py::return_value_policy::reference)
        .def("find_by_id", &UICanvas::FindById, py::arg("id"), py::return_value_policy::reference)
        .def("clear_ui", &UICanvas::ClearUI)
        .def("sequence", &UICanvas::Sequence)
        .def("set_theme", &UICanvas::SetTheme, py::arg("theme"))
        .def_property("theme", py::overload_cast<>(&UICanvas::Theme), &UICanvas::SetTheme, py::return_value_policy::reference_internal)
        .def_property("reference_size",
            [](const UICanvas& self) { return Vector2{ self.ReferenceWidth, self.ReferenceHeight }; },
            [](UICanvas& self, Vector2 v) { self.ReferenceWidth = v.x; self.ReferenceHeight = v.y; })
        .def_readwrite("reference_width", &UICanvas::ReferenceWidth)
        .def_readwrite("reference_height", &UICanvas::ReferenceHeight)
        .def_readwrite("match", &UICanvas::Match)
        .def_property_readonly("scale_factor", &UICanvas::ScaleFactor)
        .def_property_readonly("logical_width", &UICanvas::LogicalWidth)
        .def_property_readonly("logical_height", &UICanvas::LogicalHeight)
        .def_readwrite("debug_draw", &UICanvas::DebugDraw)
        .def_property("text_scale", &UICanvas::TextScale, &UICanvas::SetTextScale)
        .def("problems", [](UICanvas& self) -> py::list {
            py::list out;
            for (const auto& p : self.Problems()) {
                py::dict d;
                d["kind"] = p.Kind;
                d["element"] = p.Element->Name;
                d["detail"] = p.Detail;
                out.append(d);
            }
            return out;
        })
        .def_property("focused", &UICanvas::GetFocused, &UICanvas::SetFocused, py::return_value_policy::reference)
        .def_property_readonly("hovered", &UICanvas::GetHovered, py::return_value_policy::reference)
        .def("__repr__", [](const UICanvas& self) {
            return "<UICanvas " + std::to_string(self.Id) + " '" + self.Name + "'>";
        });
    GuardDeadActorAccess(ui.attr("UICanvas"));

    // Submodule functions
    ui.def("spawn_canvas", &Api::UISpawnCanvas,
       py::arg("name") = "Canvas",
       py::arg("script") = "",
       py::arg("reference_width") = 1920.0f,
       py::arg("reference_height") = 1080.0f,
       py::arg("match") = 0.5f,
       py::arg("persistent") = false,
       py::return_value_policy::reference);

    // Whether any live canvas of the scene has the mouse: a world actor checks this before turning a click, drag or wheel into a world action.
    ui.def("pointer_over_ui", &Api::UIPointerOverUI);

    ui.def("modern_theme", &CreateModernTheme);
    ui.def("classic_theme", &CreateClassicTheme);
    ui.def("classic_dark_theme", &CreateClassicDarkTheme);
    ui.def("theme", []() { return UITheme{}; });
    ui.def("icon_names", &GetIconNames);
}

} // namespace IYI
