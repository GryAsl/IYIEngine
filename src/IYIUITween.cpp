// Implements the IYIUI tweening and sequence animation subsystem, easing mathematics, handle methods, and the
// UICanvas tween/sequence lifecycle (StartTween through UpdateSequences).
// Handles are ID-based handles {canvas, id} rather than raw pointers, avoiding invalidation during container
// reallocation or element removal. Delta time is clamped to 0.1s per update step to prevent animation skipping
// after long frame stalls or OS window dragging. Active tweens on the same (element, property) are replaced atomically.
// Public headers remain clean: no Python.h, pybind11, nlohmann/json, or rlgl.h.

#include "IYI/IYIUI/IYIUITween.h"
#include "IYI/IYIUI/IYIUI.h"
#include "IYI/Log.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace IYI::UI {

static constexpr float kPI = 3.14159265358979323846f;

// Easing functions (pure, mathematical implementations for all 21 curves)
static float EaseLinear(float t) { return t; }

static float EaseInSine(float t) { return 1.0f - std::cos(t * (kPI * 0.5f)); }
static float EaseOutSine(float t) { return std::sin(t * (kPI * 0.5f)); }
static float EaseInOutSine(float t) { return -0.5f * (std::cos(kPI * t) - 1.0f); }

static float EaseInQuad(float t) { return t * t; }
static float EaseOutQuad(float t) { return t * (2.0f - t); }
static float EaseInOutQuad(float t) { return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t; }

static float EaseInCubic(float t) { return t * t * t; }
static float EaseOutCubic(float t) { float p = t - 1.0f; return p * p * p + 1.0f; }
static float EaseInOutCubic(float t) {
    if (t < 0.5f) return 4.0f * t * t * t;
    float p = 2.0f * t - 2.0f;
    return 0.5f * p * p * p + 1.0f;
}

static float EaseInQuart(float t) { return t * t * t * t; }
static float EaseOutQuart(float t) { float p = t - 1.0f; return 1.0f - p * p * p * p; }
static float EaseInOutQuart(float t) {
    if (t < 0.5f) return 8.0f * t * t * t * t;
    float p = t - 1.0f;
    return 1.0f - 8.0f * p * p * p * p;
}

static float EaseInExpo(float t) { return (t <= 0.0f) ? 0.0f : std::pow(2.0f, 10.0f * (t - 1.0f)); }
static float EaseOutExpo(float t) { return (t >= 1.0f) ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t); }
static float EaseInOutExpo(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (t < 0.5f) return std::pow(2.0f, 20.0f * t - 10.0f) * 0.5f;
    return (2.0f - std::pow(2.0f, -20.0f * t + 10.0f)) * 0.5f;
}

static float EaseInBack(float t) {
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    return c3 * t * t * t - c1 * t * t;
}
static float EaseOutBack(float t) {
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    float p = t - 1.0f;
    return 1.0f + c3 * p * p * p + c1 * p * p;
}
static float EaseInOutBack(float t) {
    constexpr float c1 = 1.70158f;
    constexpr float c2 = c1 * 1.525f;
    if (t < 0.5f) {
        return (std::pow(2.0f * t, 2.0f) * ((c2 + 1.0f) * 2.0f * t - c2)) * 0.5f;
    }
    float p = 2.0f * t - 2.0f;
    return (std::pow(p, 2.0f) * ((c2 + 1.0f) * p + c2) + 2.0f) * 0.5f;
}

static float EaseOutBounce(float t) {
    constexpr float n1 = 7.5625f;
    constexpr float d1 = 2.75f;
    if (t < 1.0f / d1) {
        return n1 * t * t;
    } else if (t < 2.0f / d1) {
        float p = t - 1.5f / d1;
        return n1 * p * p + 0.75f;
    } else if (t < 2.5f / d1) {
        float p = t - 2.25f / d1;
        return n1 * p * p + 0.9375f;
    } else {
        float p = t - 2.625f / d1;
        return n1 * p * p + 0.984375f;
    }
}

static float EaseOutElastic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    constexpr float c4 = (2.0f * kPI) / 3.0f;
    return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
}

UIEase ParseEase(const std::string& name)
{
    static const std::unordered_map<std::string, UIEase> s_easeMap = {
        { "linear", UIEase::Linear },
        { "in_sine", UIEase::InSine },
        { "out_sine", UIEase::OutSine },
        { "in_out_sine", UIEase::InOutSine },
        { "in_quad", UIEase::InQuad },
        { "out_quad", UIEase::OutQuad },
        { "in_out_quad", UIEase::InOutQuad },
        { "in_cubic", UIEase::InCubic },
        { "out_cubic", UIEase::OutCubic },
        { "in_out_cubic", UIEase::InOutCubic },
        { "in_quart", UIEase::InQuart },
        { "out_quart", UIEase::OutQuart },
        { "in_out_quart", UIEase::InOutQuart },
        { "in_expo", UIEase::InExpo },
        { "out_expo", UIEase::OutExpo },
        { "in_out_expo", UIEase::InOutExpo },
        { "in_back", UIEase::InBack },
        { "out_back", UIEase::OutBack },
        { "in_out_back", UIEase::InOutBack },
        { "out_elastic", UIEase::OutElastic },
        { "out_bounce", UIEase::OutBounce }
    };

    auto it = s_easeMap.find(name);
    if (it != s_easeMap.end()) {
        return it->second;
    }

    // Warn only once for unknown ease string
    static std::unordered_set<std::string> s_warnedUnknownEase;
    if (s_warnedUnknownEase.find(name) == s_warnedUnknownEase.end()) {
        s_warnedUnknownEase.insert(name);
        LogWrite(LogLevel::Warn, "ui", "Unknown ease function '" + name + "', falling back to linear");
    }

    return UIEase::Linear;
}

float EvaluateEase(UIEase ease, float t)
{
    float clampedT = std::clamp(t, 0.0f, 1.0f);
    switch (ease) {
        case UIEase::Linear: return EaseLinear(clampedT);
        case UIEase::InSine: return EaseInSine(clampedT);
        case UIEase::OutSine: return EaseOutSine(clampedT);
        case UIEase::InOutSine: return EaseInOutSine(clampedT);
        case UIEase::InQuad: return EaseInQuad(clampedT);
        case UIEase::OutQuad: return EaseOutQuad(clampedT);
        case UIEase::InOutQuad: return EaseInOutQuad(clampedT);
        case UIEase::InCubic: return EaseInCubic(clampedT);
        case UIEase::OutCubic: return EaseOutCubic(clampedT);
        case UIEase::InOutCubic: return EaseInOutCubic(clampedT);
        case UIEase::InQuart: return EaseInQuart(clampedT);
        case UIEase::OutQuart: return EaseOutQuart(clampedT);
        case UIEase::InOutQuart: return EaseInOutQuart(clampedT);
        case UIEase::InExpo: return EaseInExpo(clampedT);
        case UIEase::OutExpo: return EaseOutExpo(clampedT);
        case UIEase::InOutExpo: return EaseInOutExpo(clampedT);
        case UIEase::InBack: return EaseInBack(clampedT);
        case UIEase::OutBack: return EaseOutBack(clampedT);
        case UIEase::InOutBack: return EaseInOutBack(clampedT);
        case UIEase::OutElastic: return EaseOutElastic(clampedT);
        case UIEase::OutBounce: return EaseOutBounce(clampedT);
    }
    return clampedT;
}

// UITweenHandle implementation
bool UITweenHandle::Active() const
{
    return Canvas ? Canvas->IsTweenActive(TweenId) : false;
}

bool UITweenHandle::Completed() const
{
    return Canvas ? Canvas->IsTweenCompleted(TweenId) : false;
}

float UITweenHandle::Progress() const
{
    return Canvas ? Canvas->GetTweenProgress(TweenId) : 1.0f;
}

UITweenHandle& UITweenHandle::Delay(float seconds)
{
    if (Canvas) Canvas->SetTweenDelay(TweenId, seconds);
    return *this;
}

float UITweenHandle::GetDelay() const
{
    return Canvas ? Canvas->GetTweenDelay(TweenId) : 0.0f;
}

UITweenHandle& UITweenHandle::Loops(int count)
{
    if (Canvas) Canvas->SetTweenLoops(TweenId, count);
    return *this;
}

int UITweenHandle::GetLoops() const
{
    return Canvas ? Canvas->GetTweenLoops(TweenId) : 0;
}

UITweenHandle& UITweenHandle::Yoyo(bool enable)
{
    if (Canvas) Canvas->SetTweenYoyo(TweenId, enable);
    return *this;
}

bool UITweenHandle::GetYoyo() const
{
    return Canvas ? Canvas->GetTweenYoyo(TweenId) : false;
}

void UITweenHandle::Kill()
{
    if (Canvas) Canvas->KillTween(TweenId);
}

void UITweenHandle::Pause()
{
    if (Canvas) Canvas->PauseTween(TweenId);
}

void UITweenHandle::Resume()
{
    if (Canvas) Canvas->ResumeTween(TweenId);
}

// UISequenceHandle implementation
bool UISequenceHandle::Active() const
{
    return Canvas ? Canvas->IsSequenceActive(SequenceId) : false;
}

bool UISequenceHandle::Completed() const
{
    return Canvas ? Canvas->IsSequenceCompleted(SequenceId) : false;
}

UISequenceHandle& UISequenceHandle::AppendMove(UIElement* element, float x, float y, float duration, const std::string& ease)
{
    if (Canvas && element) {
        UISequenceStep step;
        step.TargetElementId = element->Id;
        step.Property = UITweenProperty::AnchoredPosition;
        step.ToValues[0] = x;
        step.ToValues[1] = y;
        step.ValueCount = 2;
        step.Duration = duration;
        step.Ease = ParseEase(ease);
        Canvas->AddSequenceStep(SequenceId, step, false);
    }
    return *this;
}

UISequenceHandle& UISequenceHandle::JoinMove(UIElement* element, float x, float y, float duration, const std::string& ease)
{
    if (Canvas && element) {
        UISequenceStep step;
        step.TargetElementId = element->Id;
        step.Property = UITweenProperty::AnchoredPosition;
        step.ToValues[0] = x;
        step.ToValues[1] = y;
        step.ValueCount = 2;
        step.Duration = duration;
        step.Ease = ParseEase(ease);
        Canvas->AddSequenceStep(SequenceId, step, true);
    }
    return *this;
}

UISequenceHandle& UISequenceHandle::AppendSize(UIElement* element, float w, float h, float duration, const std::string& ease)
{
    if (Canvas && element) {
        UISequenceStep step;
        step.TargetElementId = element->Id;
        step.Property = UITweenProperty::SizeDelta;
        step.ToValues[0] = w;
        step.ToValues[1] = h;
        step.ValueCount = 2;
        step.Duration = duration;
        step.Ease = ParseEase(ease);
        Canvas->AddSequenceStep(SequenceId, step, false);
    }
    return *this;
}

UISequenceHandle& UISequenceHandle::JoinSize(UIElement* element, float w, float h, float duration, const std::string& ease)
{
    if (Canvas && element) {
        UISequenceStep step;
        step.TargetElementId = element->Id;
        step.Property = UITweenProperty::SizeDelta;
        step.ToValues[0] = w;
        step.ToValues[1] = h;
        step.ValueCount = 2;
        step.Duration = duration;
        step.Ease = ParseEase(ease);
        Canvas->AddSequenceStep(SequenceId, step, true);
    }
    return *this;
}

UISequenceHandle& UISequenceHandle::AppendScale(UIElement* element, float sx, float sy, float duration, const std::string& ease)
{
    if (Canvas && element) {
        UISequenceStep step;
        step.TargetElementId = element->Id;
        step.Property = UITweenProperty::Scale;
        step.ToValues[0] = sx;
        step.ToValues[1] = sy;
        step.ValueCount = 2;
        step.Duration = duration;
        step.Ease = ParseEase(ease);
        Canvas->AddSequenceStep(SequenceId, step, false);
    }
    return *this;
}

UISequenceHandle& UISequenceHandle::JoinScale(UIElement* element, float sx, float sy, float duration, const std::string& ease)
{
    if (Canvas && element) {
        UISequenceStep step;
        step.TargetElementId = element->Id;
        step.Property = UITweenProperty::Scale;
        step.ToValues[0] = sx;
        step.ToValues[1] = sy;
        step.ValueCount = 2;
        step.Duration = duration;
        step.Ease = ParseEase(ease);
        Canvas->AddSequenceStep(SequenceId, step, true);
    }
    return *this;
}

UISequenceHandle& UISequenceHandle::AppendRotate(UIElement* element, float degrees, float duration, const std::string& ease)
{
    if (Canvas && element) {
        UISequenceStep step;
        step.TargetElementId = element->Id;
        step.Property = UITweenProperty::Rotation;
        step.ToValues[0] = degrees;
        step.ValueCount = 1;
        step.Duration = duration;
        step.Ease = ParseEase(ease);
        Canvas->AddSequenceStep(SequenceId, step, false);
    }
    return *this;
}

UISequenceHandle& UISequenceHandle::JoinRotate(UIElement* element, float degrees, float duration, const std::string& ease)
{
    if (Canvas && element) {
        UISequenceStep step;
        step.TargetElementId = element->Id;
        step.Property = UITweenProperty::Rotation;
        step.ToValues[0] = degrees;
        step.ValueCount = 1;
        step.Duration = duration;
        step.Ease = ParseEase(ease);
        Canvas->AddSequenceStep(SequenceId, step, true);
    }
    return *this;
}

UISequenceHandle& UISequenceHandle::AppendFade(UIElement* element, float opacity, float duration, const std::string& ease)
{
    if (Canvas && element) {
        UISequenceStep step;
        step.TargetElementId = element->Id;
        step.Property = UITweenProperty::Opacity;
        step.ToValues[0] = opacity;
        step.ValueCount = 1;
        step.Duration = duration;
        step.Ease = ParseEase(ease);
        Canvas->AddSequenceStep(SequenceId, step, false);
    }
    return *this;
}

UISequenceHandle& UISequenceHandle::JoinFade(UIElement* element, float opacity, float duration, const std::string& ease)
{
    if (Canvas && element) {
        UISequenceStep step;
        step.TargetElementId = element->Id;
        step.Property = UITweenProperty::Opacity;
        step.ToValues[0] = opacity;
        step.ValueCount = 1;
        step.Duration = duration;
        step.Ease = ParseEase(ease);
        Canvas->AddSequenceStep(SequenceId, step, true);
    }
    return *this;
}

UISequenceHandle& UISequenceHandle::AppendColor(UIElement* element, Color color, float duration, const std::string& ease)
{
    if (Canvas && element) {
        UISequenceStep step;
        step.TargetElementId = element->Id;
        step.Property = UITweenProperty::BackgroundColor;
        step.ToValues[0] = static_cast<float>(color.r);
        step.ToValues[1] = static_cast<float>(color.g);
        step.ToValues[2] = static_cast<float>(color.b);
        step.ToValues[3] = static_cast<float>(color.a);
        step.ValueCount = 4;
        step.Duration = duration;
        step.Ease = ParseEase(ease);
        Canvas->AddSequenceStep(SequenceId, step, false);
    }
    return *this;
}

UISequenceHandle& UISequenceHandle::JoinColor(UIElement* element, Color color, float duration, const std::string& ease)
{
    if (Canvas && element) {
        UISequenceStep step;
        step.TargetElementId = element->Id;
        step.Property = UITweenProperty::BackgroundColor;
        step.ToValues[0] = static_cast<float>(color.r);
        step.ToValues[1] = static_cast<float>(color.g);
        step.ToValues[2] = static_cast<float>(color.b);
        step.ToValues[3] = static_cast<float>(color.a);
        step.ValueCount = 4;
        step.Duration = duration;
        step.Ease = ParseEase(ease);
        Canvas->AddSequenceStep(SequenceId, step, true);
    }
    return *this;
}

UISequenceHandle& UISequenceHandle::AppendTextColor(UIElement* element, Color color, float duration, const std::string& ease)
{
    if (Canvas && element) {
        UISequenceStep step;
        step.TargetElementId = element->Id;
        step.Property = UITweenProperty::TextColor;
        step.ToValues[0] = static_cast<float>(color.r);
        step.ToValues[1] = static_cast<float>(color.g);
        step.ToValues[2] = static_cast<float>(color.b);
        step.ToValues[3] = static_cast<float>(color.a);
        step.ValueCount = 4;
        step.Duration = duration;
        step.Ease = ParseEase(ease);
        Canvas->AddSequenceStep(SequenceId, step, false);
    }
    return *this;
}

UISequenceHandle& UISequenceHandle::JoinTextColor(UIElement* element, Color color, float duration, const std::string& ease)
{
    if (Canvas && element) {
        UISequenceStep step;
        step.TargetElementId = element->Id;
        step.Property = UITweenProperty::TextColor;
        step.ToValues[0] = static_cast<float>(color.r);
        step.ToValues[1] = static_cast<float>(color.g);
        step.ToValues[2] = static_cast<float>(color.b);
        step.ToValues[3] = static_cast<float>(color.a);
        step.ValueCount = 4;
        step.Duration = duration;
        step.Ease = ParseEase(ease);
        Canvas->AddSequenceStep(SequenceId, step, true);
    }
    return *this;
}

UISequenceHandle& UISequenceHandle::AppendInterval(float seconds)
{
    if (Canvas) {
        Canvas->AddSequenceInterval(SequenceId, seconds);
    }
    return *this;
}

void UISequenceHandle::Play()
{
    if (Canvas) {
        Canvas->PlaySequence(SequenceId);
    }
}

void UISequenceHandle::Kill()
{
    if (Canvas) {
        Canvas->KillSequence(SequenceId);
    }
}


// Tweens (Phase 2)
uint64_t UICanvas::StartTween(uint64_t targetElementId, UITweenProperty prop, const float* toValues, int count, float duration, UIEase ease)
{
    // FindById walks only the live tree, so a retired element is refused here: id 0, no tween created.
    UIElement* elem = FindById(targetElementId);
    if (!elem) return 0;

    // Rule: Starting a tween on the same (element, property) kills the previous one
    for (auto& tw : m_tweens) {
        if (!tw.Dead && tw.TargetElementId == targetElementId && tw.Property == prop) {
            tw.Dead = true;
            tw.Paused = true;
        }
    }

    UITweenData tw;
    tw.Id = m_nextTweenId++;
    tw.TargetElementId = targetElementId;
    tw.Property = prop;
    tw.ValueCount = std::clamp(count, 1, 4);
    for (int i = 0; i < tw.ValueCount; ++i) {
        tw.ToValues[i] = toValues[i];
    }
    tw.Duration = std::max(0.0f, duration);
    tw.Ease = ease;

    // Capture initial values (from current property value)
    switch (prop) {
        case UITweenProperty::AnchoredPosition:
            tw.FromValues[0] = elem->Rect.AnchoredPosition.x;
            tw.FromValues[1] = elem->Rect.AnchoredPosition.y;
            break;
        case UITweenProperty::SizeDelta:
            tw.FromValues[0] = elem->Rect.SizeDelta.x;
            tw.FromValues[1] = elem->Rect.SizeDelta.y;
            break;
        case UITweenProperty::Scale:
            tw.FromValues[0] = elem->Rect.Scale.x;
            tw.FromValues[1] = elem->Rect.Scale.y;
            break;
        case UITweenProperty::Rotation:
            tw.FromValues[0] = elem->Rect.Rotation;
            break;
        case UITweenProperty::Translation:
            tw.FromValues[0] = elem->Rect.Translation.x;
            tw.FromValues[1] = elem->Rect.Translation.y;
            break;
        case UITweenProperty::Opacity:
            tw.FromValues[0] = elem->Opacity;
            break;
        case UITweenProperty::BackgroundColor: {
            Color c = elem->CustomStyle ? elem->CustomStyle->BackgroundColor : elem->TransitionState.CurrentBg;
            tw.FromValues[0] = static_cast<float>(c.r);
            tw.FromValues[1] = static_cast<float>(c.g);
            tw.FromValues[2] = static_cast<float>(c.b);
            tw.FromValues[3] = static_cast<float>(c.a);
            break;
        }
        case UITweenProperty::TextColor: {
            Color c = { 255, 255, 255, 255 };
            if (elem->Type == UIElementType::Text) {
                UIText* txt = static_cast<UIText*>(elem);
                c = (txt->ColorOverride.a > 0) ? txt->ColorOverride : elem->TransitionState.CurrentText;
            } else {
                c = elem->CustomStyle ? elem->CustomStyle->TextColor : elem->TransitionState.CurrentText;
            }
            tw.FromValues[0] = static_cast<float>(c.r);
            tw.FromValues[1] = static_cast<float>(c.g);
            tw.FromValues[2] = static_cast<float>(c.b);
            tw.FromValues[3] = static_cast<float>(c.a);
            break;
        }
    }

    m_tweens.push_back(tw);
    return tw.Id;
}

void UICanvas::KillTweensFor(UIElement* element, bool includeDescendants)
{
    if (!element) return;
    std::unordered_set<uint64_t> targetIds;
    targetIds.insert(element->Id);

    if (includeDescendants) {
        auto collectIds = [&](UIElement* elem, auto&& self) -> void {
            if (!elem) return;
            for (auto& c : elem->Children) {
                targetIds.insert(c->Id);
                self(c.get(), self);
            }
        };
        collectIds(element, collectIds);
    }

    for (auto& tw : m_tweens) {
        if (targetIds.find(tw.TargetElementId) != targetIds.end()) {
            tw.Dead = true;
        }
    }

    for (auto& seq : m_sequences) {
        for (const auto& step : seq.Steps) {
            if (targetIds.find(step.TargetElementId) != targetIds.end()) {
                seq.Dead = true;
                break;
            }
        }
    }
}

void UICanvas::KillAllTweens()
{
    for (auto& tw : m_tweens) {
        tw.Dead = true;
    }
    m_tweens.clear();
}

void UICanvas::UpdateTweens(float dt)
{
    // Clamp tween dt to at most 0.1s to prevent stall fast-forwarding
    float tweenDt = std::min(dt, 0.1f);

    const size_t count = m_tweens.size();
    for (size_t i = 0; i < count; ++i) {
        auto& tw = m_tweens[i];
        if (tw.Dead || tw.Paused || tw.Completed) continue;

        UIElement* elem = FindById(tw.TargetElementId);
        if (!elem) {
            tw.Dead = true;
            continue;
        }

        if (tw.Delay > 0.0f) {
            if (tweenDt < tw.Delay) {
                tw.Delay -= tweenDt;
                continue;
            } else {
                float dtRem = tweenDt - tw.Delay;
                tw.Delay = 0.0f;
                tw.Elapsed += dtRem;
            }
        } else {
            tw.Elapsed += tweenDt;
        }

        float dur = std::max(tw.Duration, 1e-6f);
        float rawProgress = tw.Elapsed / dur;
        bool finishedCycle = (rawProgress >= 1.0f);

        float t = std::clamp(rawProgress, 0.0f, 1.0f);
        if (tw.Yoyo && (tw.CurrentLoop % 2 == 1)) {
            t = 1.0f - t;
        }

        float easedT = EvaluateEase(tw.Ease, t);

        switch (tw.Property) {
            case UITweenProperty::AnchoredPosition:
                elem->Rect.AnchoredPosition.x = tw.FromValues[0] + (tw.ToValues[0] - tw.FromValues[0]) * easedT;
                elem->Rect.AnchoredPosition.y = tw.FromValues[1] + (tw.ToValues[1] - tw.FromValues[1]) * easedT;
                break;
            case UITweenProperty::SizeDelta:
                elem->Rect.SizeDelta.x = tw.FromValues[0] + (tw.ToValues[0] - tw.FromValues[0]) * easedT;
                elem->Rect.SizeDelta.y = tw.FromValues[1] + (tw.ToValues[1] - tw.FromValues[1]) * easedT;
                break;
            case UITweenProperty::Scale:
                elem->Rect.Scale.x = tw.FromValues[0] + (tw.ToValues[0] - tw.FromValues[0]) * easedT;
                elem->Rect.Scale.y = tw.FromValues[1] + (tw.ToValues[1] - tw.FromValues[1]) * easedT;
                break;
            case UITweenProperty::Rotation:
                elem->Rect.Rotation = tw.FromValues[0] + (tw.ToValues[0] - tw.FromValues[0]) * easedT;
                break;
            case UITweenProperty::Translation:
                elem->Rect.Translation.x = tw.FromValues[0] + (tw.ToValues[0] - tw.FromValues[0]) * easedT;
                elem->Rect.Translation.y = tw.FromValues[1] + (tw.ToValues[1] - tw.FromValues[1]) * easedT;
                break;
            case UITweenProperty::Opacity:
                elem->Opacity = std::clamp(tw.FromValues[0] + (tw.ToValues[0] - tw.FromValues[0]) * easedT, 0.0f, 1.0f);
                break;
            case UITweenProperty::BackgroundColor: {
                Color c{
                    static_cast<unsigned char>(std::clamp(std::round(tw.FromValues[0] + (tw.ToValues[0] - tw.FromValues[0]) * easedT), 0.0f, 255.0f)),
                    static_cast<unsigned char>(std::clamp(std::round(tw.FromValues[1] + (tw.ToValues[1] - tw.FromValues[1]) * easedT), 0.0f, 255.0f)),
                    static_cast<unsigned char>(std::clamp(std::round(tw.FromValues[2] + (tw.ToValues[2] - tw.FromValues[2]) * easedT), 0.0f, 255.0f)),
                    static_cast<unsigned char>(std::clamp(std::round(tw.FromValues[3] + (tw.ToValues[3] - tw.FromValues[3]) * easedT), 0.0f, 255.0f))
                };
                elem->GetOrCreateCustomStyle().BackgroundColor = c;
                elem->TransitionState.CurrentBg = c;
                break;
            }
            case UITweenProperty::TextColor: {
                Color c{
                    static_cast<unsigned char>(std::clamp(std::round(tw.FromValues[0] + (tw.ToValues[0] - tw.FromValues[0]) * easedT), 0.0f, 255.0f)),
                    static_cast<unsigned char>(std::clamp(std::round(tw.FromValues[1] + (tw.ToValues[1] - tw.FromValues[1]) * easedT), 0.0f, 255.0f)),
                    static_cast<unsigned char>(std::clamp(std::round(tw.FromValues[2] + (tw.ToValues[2] - tw.FromValues[2]) * easedT), 0.0f, 255.0f)),
                    static_cast<unsigned char>(std::clamp(std::round(tw.FromValues[3] + (tw.ToValues[3] - tw.FromValues[3]) * easedT), 0.0f, 255.0f))
                };
                if (elem->Type == UIElementType::Text) {
                    static_cast<UIText*>(elem)->ColorOverride = c;
                }
                elem->GetOrCreateCustomStyle().TextColor = c;
                elem->TransitionState.CurrentText = c;
                break;
            }
        }

        if (finishedCycle) {
            if (tw.Loops != 0) {
                tw.CurrentLoop++;
                if (tw.Loops > 0 && tw.CurrentLoop >= tw.Loops) {
                    tw.Completed = true;
                } else {
                    tw.Elapsed -= dur;
                }
            } else {
                tw.Completed = true;
            }
        }
    }

    std::erase_if(m_tweens, [](const UITweenData& tw) {
        return tw.Dead;
    });
}

bool UICanvas::IsTweenActive(uint64_t tweenId) const
{
    for (const auto& tw : m_tweens) {
        if (tw.Id == tweenId) {
            return !tw.Dead && !tw.Completed && !tw.Paused;
        }
    }
    return false;
}

bool UICanvas::IsTweenCompleted(uint64_t tweenId) const
{
    for (const auto& tw : m_tweens) {
        if (tw.Id == tweenId) {
            return tw.Completed;
        }
    }
    return false;
}

float UICanvas::GetTweenProgress(uint64_t tweenId) const
{
    for (const auto& tw : m_tweens) {
        if (tw.Id == tweenId) {
            if (tw.Completed) return 1.0f;
            float dur = std::max(tw.Duration, 1e-6f);
            return std::clamp(tw.Elapsed / dur, 0.0f, 1.0f);
        }
    }
    return 1.0f;
}

void UICanvas::SetTweenDelay(uint64_t tweenId, float delay)
{
    for (auto& tw : m_tweens) {
        if (tw.Id == tweenId) {
            tw.Delay = std::max(0.0f, delay);
            break;
        }
    }
}

float UICanvas::GetTweenDelay(uint64_t tweenId) const
{
    for (const auto& tw : m_tweens) {
        if (tw.Id == tweenId) return tw.Delay;
    }
    return 0.0f;
}

void UICanvas::SetTweenLoops(uint64_t tweenId, int loops)
{
    for (auto& tw : m_tweens) {
        if (tw.Id == tweenId) {
            tw.Loops = loops;
            break;
        }
    }
}

int UICanvas::GetTweenLoops(uint64_t tweenId) const
{
    for (const auto& tw : m_tweens) {
        if (tw.Id == tweenId) return tw.Loops;
    }
    return 0;
}

void UICanvas::SetTweenYoyo(uint64_t tweenId, bool yoyo)
{
    for (auto& tw : m_tweens) {
        if (tw.Id == tweenId) {
            tw.Yoyo = yoyo;
            break;
        }
    }
}

bool UICanvas::GetTweenYoyo(uint64_t tweenId) const
{
    for (const auto& tw : m_tweens) {
        if (tw.Id == tweenId) return tw.Yoyo;
    }
    return false;
}

void UICanvas::KillTween(uint64_t tweenId)
{
    for (auto& tw : m_tweens) {
        if (tw.Id == tweenId) {
            tw.Dead = true;
            break;
        }
    }
}

void UICanvas::PauseTween(uint64_t tweenId)
{
    for (auto& tw : m_tweens) {
        if (tw.Id == tweenId) {
            tw.Paused = true;
            break;
        }
    }
}

void UICanvas::ResumeTween(uint64_t tweenId)
{
    for (auto& tw : m_tweens) {
        if (tw.Id == tweenId) {
            tw.Paused = false;
            break;
        }
    }
}

// Sequences (Phase 2)
UISequenceHandle UICanvas::Sequence()
{
    uint64_t id = CreateSequence();
    return UISequenceHandle{ this, id };
}

uint64_t UICanvas::CreateSequence()
{
    UISequenceData seq;
    seq.Id = m_nextSequenceId++;
    m_sequences.push_back(seq);
    return seq.Id;
}

void UICanvas::AddSequenceStep(uint64_t seqId, const UISequenceStep& step, bool isJoin)
{
    if (!FindById(step.TargetElementId)) return; // retired/unknown target: refuse the step, as StartTween does
    for (auto& seq : m_sequences) {
        if (seq.Id == seqId) {
            UISequenceStep s = step;
            if (isJoin) {
                s.StartTime = seq.LastStepStart;
                seq.TotalDuration = std::max(seq.TotalDuration, seq.LastStepStart + s.Duration);
                seq.CurrentPosition = std::max(seq.CurrentPosition, seq.LastStepStart + s.Duration);
            } else {
                s.StartTime = seq.CurrentPosition;
                seq.LastStepStart = seq.CurrentPosition;
                seq.CurrentPosition += s.Duration;
                seq.TotalDuration = std::max(seq.TotalDuration, seq.CurrentPosition);
            }
            seq.Steps.push_back(s);
            break;
        }
    }
}

void UICanvas::AddSequenceInterval(uint64_t seqId, float seconds)
{
    for (auto& seq : m_sequences) {
        if (seq.Id == seqId) {
            seq.CurrentPosition += std::max(0.0f, seconds);
            seq.LastStepStart = seq.CurrentPosition;
            seq.TotalDuration = std::max(seq.TotalDuration, seq.CurrentPosition);
            break;
        }
    }
}

void UICanvas::PlaySequence(uint64_t seqId)
{
    for (auto& seq : m_sequences) {
        if (seq.Id == seqId) {
            seq.IsPlaying = true;
            seq.Completed = false;
            seq.Elapsed = 0.0f;
            for (auto& step : seq.Steps) {
                step.Started = false;
                step.SpawnedTweenId = 0;
            }
            break;
        }
    }
}

void UICanvas::KillSequence(uint64_t seqId)
{
    for (auto& seq : m_sequences) {
        if (seq.Id == seqId) {
            seq.Dead = true;
            seq.IsPlaying = false;
            for (auto& step : seq.Steps) {
                if (step.SpawnedTweenId > 0) {
                    KillTween(step.SpawnedTweenId);
                }
            }
            break;
        }
    }
}

void UICanvas::KillAllSequences()
{
    for (auto& seq : m_sequences) {
        seq.Dead = true;
        for (auto& step : seq.Steps) {
            if (step.SpawnedTweenId > 0) {
                KillTween(step.SpawnedTweenId);
            }
        }
    }
    m_sequences.clear();
}

bool UICanvas::IsSequenceActive(uint64_t seqId) const
{
    for (const auto& seq : m_sequences) {
        if (seq.Id == seqId) {
            return !seq.Dead && seq.IsPlaying && !seq.Completed;
        }
    }
    return false;
}

bool UICanvas::IsSequenceCompleted(uint64_t seqId) const
{
    for (const auto& seq : m_sequences) {
        if (seq.Id == seqId) {
            return seq.Completed;
        }
    }
    return false;
}

void UICanvas::UpdateSequences(float dt)
{
    float tweenDt = std::min(dt, 0.1f);

    const size_t count = m_sequences.size();
    for (size_t i = 0; i < count; ++i) {
        auto& seq = m_sequences[i];
        if (seq.Dead || !seq.IsPlaying || seq.Completed) continue;

        seq.Elapsed += tweenDt;

        for (auto& step : seq.Steps) {
            if (!step.Started && seq.Elapsed >= step.StartTime) {
                step.Started = true;
                step.SpawnedTweenId = StartTween(
                    step.TargetElementId,
                    step.Property,
                    step.ToValues,
                    step.ValueCount,
                    step.Duration,
                    step.Ease
                );
            }
        }

        if (seq.Elapsed >= seq.TotalDuration) {
            bool allDone = true;
            for (const auto& step : seq.Steps) {
                if (!step.Started) {
                    allDone = false;
                    break;
                }
                if (step.SpawnedTweenId > 0 && IsTweenActive(step.SpawnedTweenId)) {
                    allDone = false;
                    break;
                }
            }
            if (allDone) {
                seq.Completed = true;
                seq.IsPlaying = false;
            }
        }
    }

    std::erase_if(m_sequences, [](const UISequenceData& seq) {
        return seq.Dead;
    });
}
} // namespace IYI::UI
