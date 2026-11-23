// Declares the IYIUI tweening and sequence animation subsystem, easing curves, and handle types.
// Provides DOTween-like animation ergonomics from Python with non-owning handle-by-ID safety.
// Tweens are owned and updated by UICanvas with real frame dt clamped to 0.1s to prevent stall fast-forwarding.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "raylib.h"

namespace IYI::UI {

class UICanvas;
class UIElement;

enum class UIEase {
    Linear,
    InSine, OutSine, InOutSine,
    InQuad, OutQuad, InOutQuad,
    InCubic, OutCubic, InOutCubic,
    InQuart, OutQuart, InOutQuart,
    InExpo, OutExpo, InOutExpo,
    InBack, OutBack, InOutBack,
    OutElastic, OutBounce
};

UIEase ParseEase(const std::string& name);
float EvaluateEase(UIEase ease, float t);

enum class UITweenProperty {
    AnchoredPosition,
    SizeDelta,
    Scale,
    Rotation,
    Translation,
    Opacity,
    BackgroundColor,
    TextColor
};

struct UITweenData
{
    uint64_t         Id = 0;
    uint64_t         TargetElementId = 0;
    UITweenProperty  Property = UITweenProperty::Opacity;
    float            FromValues[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float            ToValues[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    int              ValueCount = 1;
    float            Duration = 0.0f;
    float            Elapsed = 0.0f;
    float            Delay = 0.0f;
    UIEase           Ease = UIEase::Linear;
    int              Loops = 0; // 0 = once, >0 = count, negative = forever
    int              CurrentLoop = 0;
    bool             Yoyo = false;
    bool             Paused = false;
    bool             Completed = false;
    bool             Dead = false;
};

struct UITweenHandle
{
    UICanvas* Canvas = nullptr;
    uint64_t  TweenId = 0;

    bool Active() const;
    bool Completed() const;
    float Progress() const;

    UITweenHandle& Delay(float seconds);
    float GetDelay() const;

    UITweenHandle& Loops(int count);
    int GetLoops() const;

    UITweenHandle& Yoyo(bool enable = true);
    bool GetYoyo() const;

    void Kill();
    void Pause();
    void Resume();
};

struct UISequenceStep
{
    uint64_t         TargetElementId = 0;
    UITweenProperty  Property = UITweenProperty::Opacity;
    float            ToValues[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    int              ValueCount = 1;
    float            Duration = 0.0f;
    float            StartTime = 0.0f;
    UIEase           Ease = UIEase::Linear;
    bool             Started = false;
    uint64_t         SpawnedTweenId = 0;
};

struct UISequenceData
{
    uint64_t         Id = 0;
    std::vector<UISequenceStep> Steps;
    float            Elapsed = 0.0f;
    float            TotalDuration = 0.0f;
    float            CurrentPosition = 0.0f;
    float            LastStepStart = 0.0f;
    bool             IsPlaying = false;
    bool             Completed = false;
    bool             Dead = false;
};

struct UISequenceHandle
{
    UICanvas* Canvas = nullptr;
    uint64_t  SequenceId = 0;

    bool Active() const;
    bool Completed() const;

    UISequenceHandle& AppendMove(UIElement* element, float x, float y, float duration, const std::string& ease = "linear");
    UISequenceHandle& JoinMove(UIElement* element, float x, float y, float duration, const std::string& ease = "linear");

    UISequenceHandle& AppendSize(UIElement* element, float w, float h, float duration, const std::string& ease = "linear");
    UISequenceHandle& JoinSize(UIElement* element, float w, float h, float duration, const std::string& ease = "linear");

    UISequenceHandle& AppendScale(UIElement* element, float sx, float sy, float duration, const std::string& ease = "linear");
    UISequenceHandle& JoinScale(UIElement* element, float sx, float sy, float duration, const std::string& ease = "linear");

    UISequenceHandle& AppendRotate(UIElement* element, float degrees, float duration, const std::string& ease = "linear");
    UISequenceHandle& JoinRotate(UIElement* element, float degrees, float duration, const std::string& ease = "linear");

    UISequenceHandle& AppendFade(UIElement* element, float opacity, float duration, const std::string& ease = "linear");
    UISequenceHandle& JoinFade(UIElement* element, float opacity, float duration, const std::string& ease = "linear");

    UISequenceHandle& AppendColor(UIElement* element, Color color, float duration, const std::string& ease = "linear");
    UISequenceHandle& JoinColor(UIElement* element, Color color, float duration, const std::string& ease = "linear");

    UISequenceHandle& AppendTextColor(UIElement* element, Color color, float duration, const std::string& ease = "linear");
    UISequenceHandle& JoinTextColor(UIElement* element, Color color, float duration, const std::string& ease = "linear");

    UISequenceHandle& AppendInterval(float seconds);

    void Play();
    void Kill();
};

} // namespace IYI::UI
