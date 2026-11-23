// Implements IYIUI canvas ownership, input handling, focus, widget rendering and clipping. Themes live in
// IYIUITheme.cpp, layout in IYIUILayout.cpp, canvas tweens and sequences in IYIUITween.cpp.
// Isolates rlgl.h dependencies and manages scissor stack clipping and debug visualization.

#include "IYI/IYIUI/IYIUI.h"
#include "IYI/Application.h"
#include "IYI/ActorRegistry.h"
#include "IYI/Scene.h"
#include "IYI/Profiler.h"
#include "IYI/FramePacket.h"
#include "IYI/Ime.h"
#include "IYI/FrameInput.h"
#include "IYI/Log.h"
#include "IYI/Video.h"
#include "rlgl.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cassert>
#include <bit>
#include <utility>

namespace IYI::UI {

// UIElement implementation
UIElement::UIElement(UIElementType type, uint64_t id, const std::string& name, UICanvas* canvas)
    : Id(id), Name(name), Type(type), Canvas(canvas)
{
}

std::string UIElement::TypeName() const
{
    switch (Type) {
        case UIElementType::Root: return "Root";
        case UIElementType::Panel: return "Panel";
        case UIElementType::Text: return "Text";
        case UIElementType::Image: return "Image";
        case UIElementType::Button: return "Button";
        case UIElementType::ProgressBar: return "ProgressBar";
        case UIElementType::Slider: return "Slider";
        case UIElementType::ScrollView: return "ScrollView";
        case UIElementType::TextInput: return "TextInput";
    }
    return "Element";
}

UIStyle& UIElement::GetOrCreateCustomStyle()
{
    if (!CustomStyle) {
        CustomStyle = std::make_unique<UIStyle>();
    }
    return *CustomStyle;
}

void UIElement::Remove()
{
    if (!Alive || !Parent || !Canvas) return;
    Canvas->NotifySubtreeRemoving(this);

    auto& sibs = Parent->Children;
    for (size_t i = 0; i < sibs.size(); ++i) {
        if (sibs[i].get() == this) {
            std::unique_ptr<UIElement> self = std::move(sibs[i]);
            sibs.erase(sibs.begin() + i);
            Canvas->RetireSubtree(std::move(self)); // never free: Python may still hold this pointer
            break;
        }
    }
}

void UIElement::ClearChildren()
{
    if (!Alive || !Canvas) return;
    for (auto& child : Children) {
        Canvas->NotifySubtreeRemoving(child.get());
    }
    std::vector<std::unique_ptr<UIElement>> detached = std::move(Children);
    Children.clear();
    for (auto& child : detached) {
        Canvas->RetireSubtree(std::move(child));
    }
}

void UIElement::BringToFront()
{
    if (!Alive || !Parent) return;
    auto& sibs = Parent->Children;
    for (size_t i = 0; i < sibs.size(); ++i) {
        if (sibs[i].get() == this) {
            if (i + 1 < sibs.size()) {
                auto ptr = std::move(sibs[i]);
                sibs.erase(sibs.begin() + i);
                sibs.push_back(std::move(ptr));
            }
            break;
        }
    }
}

void UIElement::SendToBack()
{
    if (!Alive || !Parent) return;
    auto& sibs = Parent->Children;
    for (size_t i = 0; i < sibs.size(); ++i) {
        if (sibs[i].get() == this) {
            if (i > 0) {
                auto ptr = std::move(sibs[i]);
                sibs.erase(sibs.begin() + i);
                sibs.insert(sibs.begin(), std::move(ptr));
            }
            break;
        }
    }
}

void UIElement::DrawContent(const UIStyle& /*resolvedStyle*/, float /*opacity*/)
{
}

// UIElement tween helpers (Phase 2)
UITweenHandle UIElement::MoveTo(float x, float y, float duration, const std::string& ease)
{
    if (!Canvas) return UITweenHandle{};
    float toVals[2] = { x, y };
    uint64_t id = Canvas->StartTween(Id, UITweenProperty::AnchoredPosition, toVals, 2, duration, ParseEase(ease));
    return UITweenHandle{ Canvas, id };
}

UITweenHandle UIElement::TranslateTo(float x, float y, float duration, const std::string& ease)
{
    if (!Canvas) return UITweenHandle{};
    float toVals[2] = { x, y };
    uint64_t id = Canvas->StartTween(Id, UITweenProperty::Translation, toVals, 2, duration, ParseEase(ease));
    return UITweenHandle{ Canvas, id };
}

UITweenHandle UIElement::SizeTo(float w, float h, float duration, const std::string& ease)
{
    if (!Canvas) return UITweenHandle{};
    float toVals[2] = { w, h };
    uint64_t id = Canvas->StartTween(Id, UITweenProperty::SizeDelta, toVals, 2, duration, ParseEase(ease));
    return UITweenHandle{ Canvas, id };
}

UITweenHandle UIElement::ScaleTo(float sx, float sy, float duration, const std::string& ease)
{
    if (!Canvas) return UITweenHandle{};
    float toVals[2] = { sx, sy };
    uint64_t id = Canvas->StartTween(Id, UITweenProperty::Scale, toVals, 2, duration, ParseEase(ease));
    return UITweenHandle{ Canvas, id };
}

UITweenHandle UIElement::RotateTo(float degrees, float duration, const std::string& ease)
{
    if (!Canvas) return UITweenHandle{};
    float toVals[1] = { degrees };
    uint64_t id = Canvas->StartTween(Id, UITweenProperty::Rotation, toVals, 1, duration, ParseEase(ease));
    return UITweenHandle{ Canvas, id };
}

UITweenHandle UIElement::FadeTo(float opacity, float duration, const std::string& ease)
{
    if (!Canvas) return UITweenHandle{};
    float toVals[1] = { opacity };
    uint64_t id = Canvas->StartTween(Id, UITweenProperty::Opacity, toVals, 1, duration, ParseEase(ease));
    return UITweenHandle{ Canvas, id };
}

UITweenHandle UIElement::ColorTo(Color color, float duration, const std::string& ease)
{
    if (!Canvas) return UITweenHandle{};
    float toVals[4] = { static_cast<float>(color.r), static_cast<float>(color.g), static_cast<float>(color.b), static_cast<float>(color.a) };
    uint64_t id = Canvas->StartTween(Id, UITweenProperty::BackgroundColor, toVals, 4, duration, ParseEase(ease));
    return UITweenHandle{ Canvas, id };
}

UITweenHandle UIElement::TextColorTo(Color color, float duration, const std::string& ease)
{
    if (!Canvas) return UITweenHandle{};
    float toVals[4] = { static_cast<float>(color.r), static_cast<float>(color.g), static_cast<float>(color.b), static_cast<float>(color.a) };
    uint64_t id = Canvas->StartTween(Id, UITweenProperty::TextColor, toVals, 4, duration, ParseEase(ease));
    return UITweenHandle{ Canvas, id };
}

// Where a screen point drawn by the UI shows after the postprocess radial warp: the inverse of the
// shader's output-to-source lookup (LogicalMousePosition applies the lookup itself).
Vector2 WarpedScreenPoint(Vector2 point)
{
    const float width = static_cast<float>(GameInput().ScreenWidth);
    const float height = static_cast<float>(GameInput().ScreenHeight);
    const float warp = CurrentApp().GetPostprocessRadialWarp();
    if (warp <= 0.0f || width <= 0.0f || height <= 0.0f) return point;
    float x = point.x * 2.0f / width - 1.0f;
    float y = point.y * 2.0f / height - 1.0f;
    float radius = std::sqrt(x * x + y * y);
    if (radius == 0.0f) return point;
    float low = 0.0f, high = radius;
    for (int i = 0; i < 16; ++i) {
        float middle = (low + high) * 0.5f;
        if (middle * (1.0f + warp * middle * middle) < radius) low = middle;
        else high = middle;
    }
    float scale = (low + high) * 0.5f / radius;
    return Vector2{(x * scale + 1.0f) * width * 0.5f, (y * scale + 1.0f) * height * 0.5f};
}

// UICanvas
UICanvas::UICanvas()
{
    ScreenSpace = true;
    m_theme = CreateModernTheme();
    m_root = std::make_unique<UIElement>(UIElementType::Root, 0, "Root", this);
}

UICanvas::~UICanvas() { ImeCancelOwner(m_focusedElement); }

float UICanvas::ScaleFactor() const
{
    float screenW = static_cast<float>(GameInput().ScreenWidth);
    float screenH = static_cast<float>(GameInput().ScreenHeight);

    if (ReferenceWidth <= 0.0f || ReferenceHeight <= 0.0f || screenW <= 0.0f || screenH <= 0.0f) {
        return 1.0f;
    }

    float widthScale = screenW / ReferenceWidth;
    float heightScale = screenH / ReferenceHeight;
    float m = std::clamp(Match, 0.0f, 1.0f);

    // match < 0 = "fit": the whole reference size stays visible, the spare room goes to the longer side.
    float sf = Match < 0.0f ? std::min(widthScale, heightScale) : std::pow(widthScale, 1.0f - m) * std::pow(heightScale, m);
    if (std::isnan(sf) || std::isinf(sf) || sf <= 1e-6f) {
        return 1.0f;
    }
    return sf;
}

float UICanvas::LogicalWidth() const
{
    float screenW = static_cast<float>(GameInput().ScreenWidth);
    if (screenW <= 0.0f) return 0.0f;
    return screenW / ScaleFactor();
}

float UICanvas::LogicalHeight() const
{
    float screenH = static_cast<float>(GameInput().ScreenHeight);
    if (screenH <= 0.0f) return 0.0f;
    return screenH / ScaleFactor();
}

UIElement* UICanvas::Root()
{
    return m_root.get();
}

UITheme& UICanvas::Theme()
{
    return m_theme;
}

const UITheme& UICanvas::Theme() const
{
    return m_theme;
}

void UICanvas::SetTheme(const UITheme& theme)
{
    m_theme = theme;
    m_theme.ApplyTypeScale(m_textScale);
    ++m_themeGeneration;
}

void UICanvas::SetTextScale(float scale)
{
    m_textScale = scale;
    m_theme.ApplyTypeScale(m_textScale);
    ++m_themeGeneration;
}

void UICanvas::SetFocused(UIElement* element)
{
    if (element != m_focusedElement) ImeCancelOwner(m_focusedElement);
    m_focusedElement = (element && element->Alive) ? element : nullptr;
}

std::string UICanvas::UniqueName(const std::string& requestedName, UIElementType type)
{
    std::string base = requestedName;
    if (base.empty()) {
        switch (type) {
            case UIElementType::Root: base = "Root"; break;
            case UIElementType::Panel: base = "Panel"; break;
            case UIElementType::Text: base = "Text"; break;
            case UIElementType::Image: base = "Image"; break;
            case UIElementType::Button: base = "Button"; break;
            case UIElementType::ProgressBar: base = "ProgressBar"; break;
            case UIElementType::Slider: base = "Slider"; break;
            case UIElementType::ScrollView: base = "ScrollView"; break;
            case UIElementType::TextInput: base = "TextInput"; break;
        }
    }

    if (m_liveNames.find(base) == m_liveNames.end()) {
        m_liveNames.emplace(base, nullptr);   // Add stores the element once it exists
        return base;
    }

    int counter = 2;
    while (true) {
        std::string cand = base + "_" + std::to_string(counter);
        if (m_liveNames.find(cand) == m_liveNames.end()) {
            m_liveNames.emplace(cand, nullptr);
            return cand;
        }
        ++counter;
    }
}

void UICanvas::ReleaseName(const std::string& name)
{
    m_liveNames.erase(name);
}

UIElement* UICanvas::Add(UIElementType type, const std::string& name, UIElement* parent)
{
    UIElement* p = parent ? parent : m_root.get();
    if (!p->Alive) return nullptr; // parent was removed; adding under it would leak into a dead subtree
    uint64_t id = AllocateId();
    std::string uname = UniqueName(name, type);

    std::unique_ptr<UIElement> elem;
    switch (type) {
        case UIElementType::Root:
            elem = std::make_unique<UIElement>(UIElementType::Root, id, uname, this);
            break;
        case UIElementType::Panel:
            elem = std::make_unique<UIPanel>(id, uname, this);
            break;
        case UIElementType::Text:
            elem = std::make_unique<UIText>(id, uname, this);
            break;
        case UIElementType::Image:
            elem = std::make_unique<UIImage>(id, uname, this);
            break;
        case UIElementType::Button:
            elem = std::make_unique<UIButton>(id, uname, this);
            break;
        case UIElementType::ProgressBar:
            elem = std::make_unique<UIProgressBar>(id, uname, this);
            break;
        case UIElementType::Slider:
            elem = std::make_unique<UISlider>(id, uname, this);
            break;
        case UIElementType::ScrollView:
            elem = std::make_unique<UIScrollView>(id, uname, this);
            break;
        case UIElementType::TextInput:
            elem = std::make_unique<UITextInput>(id, uname, this);
            break;
    }

    elem->Parent = p;
    UIElement* raw = elem.get();
    p->Children.push_back(std::move(elem));
    m_liveNames[uname] = raw;
    m_liveIds[id] = raw;
    return raw;
}

UIPanel* UICanvas::AddPanel(const std::string& name, UIElement* parent)
{
    return static_cast<UIPanel*>(Add(UIElementType::Panel, name, parent));
}

UIText* UICanvas::AddText(const std::string& name, UIElement* parent)
{
    return static_cast<UIText*>(Add(UIElementType::Text, name, parent));
}

UIImage* UICanvas::AddImage(const std::string& name, UIElement* parent)
{
    return static_cast<UIImage*>(Add(UIElementType::Image, name, parent));
}

UIButton* UICanvas::AddButton(const std::string& name, UIElement* parent)
{
    return static_cast<UIButton*>(Add(UIElementType::Button, name, parent));
}

UIProgressBar* UICanvas::AddProgressBar(const std::string& name, UIElement* parent)
{
    return static_cast<UIProgressBar*>(Add(UIElementType::ProgressBar, name, parent));
}

UISlider* UICanvas::AddSlider(const std::string& name, UIElement* parent)
{
    return static_cast<UISlider*>(Add(UIElementType::Slider, name, parent));
}

UIScrollView* UICanvas::AddScrollView(const std::string& name, UIElement* parent)
{
    return static_cast<UIScrollView*>(Add(UIElementType::ScrollView, name, parent));
}

UITextInput* UICanvas::AddTextInput(const std::string& name,
                                   UIElement* parent,
                                   const std::string& placeholder,
                                   bool allowLetters,
                                   bool allowDigits,
                                   bool allowSymbols)
{
    auto* elem = static_cast<UITextInput*>(Add(UIElementType::TextInput, name, parent));
    elem->Placeholder = placeholder;
    elem->AllowLetters = allowLetters;
    elem->AllowDigits = allowDigits;
    elem->AllowSymbols = allowSymbols;
    return elem;
}

// The root is never added through Add, so it is checked first, as the old tree walk found it first.
UIElement* UICanvas::Find(const std::string& name)
{
    if (m_root && m_root->Name == name) return m_root.get();
    auto it = m_liveNames.find(name);
    return it != m_liveNames.end() ? it->second : nullptr;
}

UIElement* UICanvas::FindById(uint64_t id)
{
    if (m_root && m_root->Id == id) return m_root.get();
    auto it = m_liveIds.find(id);
    return it != m_liveIds.end() ? it->second : nullptr;
}

void UICanvas::ClearUI()
{
    ImeCancelOwner(m_focusedElement);
    KillAllTweens();
    KillAllSequences();
    m_layoutCache.clear();
    m_layoutSignature.clear();
    if (!m_root) return;
    for (auto& child : m_root->Children) {
        NotifySubtreeRemoving(child.get());
    }
    std::vector<std::unique_ptr<UIElement>> detached = std::move(m_root->Children);
    m_root->Children.clear();
    for (auto& child : detached) {
        RetireSubtree(std::move(child));
    }
    m_hoveredElement = nullptr;
    m_capturedElement = nullptr;
    m_focusedElement = nullptr;
}

void UICanvas::RetireSubtree(std::unique_ptr<UIElement> subtree)
{
    if (!subtree) return;
    auto markDead = [](UIElement* e, auto&& self) -> void {
        e->Alive = false;
        for (const auto& c : e->Children) self(c.get(), self);
    };
    markDead(subtree.get(), markDead);
    subtree->Parent = nullptr;
    m_retired.push_back(std::move(subtree));
}

static UIElementReferencedFn s_referencedHook = nullptr;
void SetUIElementReferencedHook(UIElementReferencedFn fn) { s_referencedHook = fn; }

// Why a subtree and not single nodes: children are owned by their parent, so freeing a parent frees every
// descendant; one referenced node anywhere keeps the whole subtree alive. Bypassing this (freeing on remove)
// reintroduces dangling pointers in retained Python wrappers.
static bool SubtreeReferenced(const UIElement* e)
{
    if (s_referencedHook && s_referencedHook(e)) return true;
    for (const auto& c : e->Children) if (SubtreeReferenced(c.get())) return true;
    return false;
}

void UICanvas::ReclaimRetired()
{
    std::erase_if(m_retired, [](const std::unique_ptr<UIElement>& root) { return !SubtreeReferenced(root.get()); });
}

bool UICanvas::AnyElementReferenced() const
{
    if (m_root && SubtreeReferenced(m_root.get())) return true;
    for (const auto& root : m_retired) if (SubtreeReferenced(root.get())) return true;
    return false;
}

void UICanvas::NotifySubtreeRemoving(UIElement* subtree)
{
    if (!subtree) return;

    KillTweensFor(subtree, true);

    auto checkInSubtree = [&](UIElement* elem) -> bool {
        if (!elem) return false;
        UIElement* cur = elem;
        while (cur) {
            if (cur == subtree) return true;
            cur = cur->Parent;
        }
        return false;
    };

    if (checkInSubtree(m_hoveredElement)) m_hoveredElement = nullptr;
    if (checkInSubtree(m_capturedElement)) m_capturedElement = nullptr;
#ifdef IYI_MOBILE
    if (checkInSubtree(m_touchScroll)) { m_touchScroll = nullptr; m_scrollVelocity = 0.0f; m_touchDragging = false; }
#endif
    if (checkInSubtree(m_focusedElement)) SetFocused(nullptr);

    auto releaseNames = [&](UIElement* elem, auto&& self) -> void {
        if (!elem) return;
        ReleaseName(elem->Name);
        m_liveIds.erase(elem->Id);
        for (const auto& child : elem->Children) {
            self(child.get(), self);
        }
    };
    releaseNames(subtree, releaseNames);
}

void UICanvas::ClearTransientFlags(UIElement* element)
{
    if (!element) return;
    if (element->Type == UIElementType::Button) {
        static_cast<UIButton*>(element)->Clicked = false;
    } else if (element->Type == UIElementType::Slider) {
        static_cast<UISlider*>(element)->Changed = false;
    } else if (element->Type == UIElementType::TextInput) {
        static_cast<UITextInput*>(element)->Submitted = false;
    }

    for (size_t i = 0; i < element->Children.size(); ++i) {
        ClearTransientFlags(element->Children[i].get());
    }
}

void UICanvas::Update(float dt)
{
    ProfileBegin("ui");
    ReclaimRetired();
    ClearTransientFlags(m_root.get());

    const Vector2 logicalMousePos = LogicalMousePosition();

    ResolveLayoutTree();
    HitTestTree(logicalMousePos, dt);
    UpdateFocusNavigation(dt);
    UpdateStateTransitions(m_root.get(), dt);
    UpdateTweens(dt);
    UpdateSequences(dt);
    ProfileEnd();
}

} // namespace IYI::UI

namespace IYI::Api {

UI::UICanvas* UISpawnCanvas(const std::string& name, const std::string& script, float referenceWidth, float referenceHeight, float match, bool persistent)
{
    auto canvas = std::make_unique<UI::UICanvas>();
    canvas->ReferenceWidth = referenceWidth;
    canvas->ReferenceHeight = referenceHeight;
    canvas->Match = match;
    canvas->Persistent = persistent;
    GameActor* actor = CurrentApp().GetScene().Actors.Spawn(std::move(canvas), name.empty() ? "Canvas" : name, script, Vector3{ 0.0f, 0.0f, 0.0f });
    return static_cast<UI::UICanvas*>(actor);
}

bool UIPointerOverUI()
{
    for (const auto& actor : CurrentApp().GetScene().Actors.GetActors()) {
        const auto* canvas = dynamic_cast<const UI::UICanvas*>(actor.get());
        if (canvas && canvas->PointerOverUI()) return true;
    }
    return false;
}

} // namespace IYI::Api
