// Declares the IYIUI retained-mode UI subsystem types, widgets, styles, themes, and UICanvas actor.
// Provides Unity-like RectTransform anchors, parent-to-child layout resolution, styling/theming,
// mouse/keyboard/gamepad input dispatch, clipping, and debug visualization.
// Follows strict engine header discipline: no Python.h, pybind11, nlohmann/json, or rlgl.h.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <map>
#include <cmath>
#include "raylib.h"
#include "IYI/GameActor.h"
#include "IYI/IYIUI/IYIUITween.h"
#include "IYI/LocalizationManager.h"

namespace IYI { class Video; }

namespace IYI::UI {

// 2x3 Affine transform: [a c tx ; b d ty]
struct UITransform2D
{
    float a = 1.0f;
    float b = 0.0f;
    float c = 0.0f;
    float d = 1.0f;
    float tx = 0.0f;
    float ty = 0.0f;

    static UITransform2D Identity()
    {
        return { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    }

    static UITransform2D Translate(float x, float y)
    {
        return { 1.0f, 0.0f, 0.0f, 1.0f, x, y };
    }

    static UITransform2D ScaleRotateAround(Vector2 origin, Vector2 scale, float degrees)
    {
        float rad = degrees * (3.14159265358979323846f / 180.0f);
        float cosA = std::cos(rad);
        float sinA = std::sin(rad);

        float a = cosA * scale.x;
        float b = sinA * scale.x;
        float c = -sinA * scale.y;
        float d = cosA * scale.y;
        float tx = origin.x - (a * origin.x + c * origin.y);
        float ty = origin.y - (b * origin.x + d * origin.y);
        return { a, b, c, d, tx, ty };
    }

    UITransform2D Compose(const UITransform2D& child) const
    {
        return {
            a * child.a + c * child.b,
            b * child.a + d * child.b,
            a * child.c + c * child.d,
            b * child.c + d * child.d,
            a * child.tx + c * child.ty + tx,
            b * child.tx + d * child.ty + ty
        };
    }

    Vector2 Apply(Vector2 p) const
    {
        return {
            a * p.x + c * p.y + tx,
            b * p.x + d * p.y + ty
        };
    }

    Vector2 ApplyInverse(Vector2 p) const
    {
        float det = a * d - b * c;
        if (std::fabs(det) < 1e-6f) return p;
        float invDet = 1.0f / det;
        float px = p.x - tx;
        float py = p.y - ty;
        return {
            (d * px - c * py) * invDet,
            (-b * px + a * py) * invDet
        };
    }
};

struct RectTransform
{
    Vector2 AnchorMin { 0.5f, 0.5f };
    Vector2 AnchorMax { 0.5f, 0.5f };
    Vector2 Pivot     { 0.5f, 0.5f };

    Vector2 AnchoredPosition { 0.0f, 0.0f };
    Vector2 SizeDelta        { 100.0f, 100.0f };

    Vector2 MinSize { 0.0f, 0.0f };
    Vector2 MaxSize { 0.0f, 0.0f }; // 0 or negative on an axis = no limit

    Vector2 Scale    { 1.0f, 1.0f };
    float   Rotation = 0.0f;        // degrees, clockwise
    Vector2 Translation { 0.0f, 0.0f }; // visual offset after Scale/Rotation; never feeds layout (a pan costs no re-solve)
};

enum class UIElementType { Root, Panel, Text, Image, Button, ProgressBar, Slider, ScrollView, TextInput };

enum class UILayoutMode  { None, Horizontal, Vertical };
enum class UIAlign       { Start, Center, End };
enum class UIHAlign      { Left, Center, Right };
enum class UIVAlign      { Top, Middle, Bottom };
enum class UIFillDirection { LeftToRight, RightToLeft, BottomToTop, TopToBottom };

struct UITextFit
{
    bool  Wrap        = true;    // word wrap inside the box width
    bool  AutoShrink  = true;    // reduce font size until it fits
    float MinFontSize = 8.0f;    // logical units; the shrink floor
};

// A pixel font draws on screen only at whole multiples of one of its native sizes (`grids`, px, ascending).
// nearest: the multiple closest to the asked size (ties: the larger native size); else (a shrunk fit) the
// largest not above it (1 px slack). At least the smallest native size; returned as a logical size.
inline float SnapPixelFontSize(float fontSize, float screenScale, const std::vector<int>& grids, bool nearest)
{
    if (grids.empty() || screenScale <= 0.0f) return fontSize;
    const float target = fontSize * screenScale;
    float best = 0.0f;
    for (int g : grids) {
        for (float c = static_cast<float>(g); c <= target * 2.0f + g; c += g) {
            const bool better = nearest ? best == 0.0f || std::fabs(c - target) <= std::fabs(best - target) + 1e-3f
                                        : c <= target + 1.0f && c >= best;   // 1 px: FitText floors sizes
            if (better) best = c;
        }
    }
    return (best > 0.0f ? best : static_cast<float>(grids.front())) / screenScale;
}

struct UITextMemo
{
    std::string LastText;
    float LastBoxWidth = -1.0f;
    float LastBoxHeight = -1.0f;
    float LastRequestedFontSize = -1.0f;
    const void* LastFontPtr = nullptr;
    int LastFontBaseSize = -1;
    unsigned int LastFontTexture = 0;
    bool LastWrap = false;
    bool LastAutoShrink = false;
    float LastMinFontSize = -1.0f;
    bool Valid = false;

    float FittedFontSize = 0.0f;
    std::vector<std::string> Lines;
    bool Ellipsized = false;
    float MeasuredWidth = 0.0f;
    float MeasuredHeight = 0.0f;
    std::vector<float> LineWidths;        // of Lines at FittedFontSize and LineWidthSpacing (DrawTextFitted)
    float LineWidthSpacing = -1.0f;
    float LineWidthFontSize = -1.0f;

    // FontForText's answers for LastText: kept while AssetManager::FontGeneration() is FontGen.
    const GlyphInfo* FontSource = nullptr;
    uint64_t FontGen = UINT64_MAX;
    const Font* ResolvedFont = nullptr;
    float ResolvedPixelSize = -1.0f;      // the pixel size ResolvedFont was asked for
    std::vector<int> PixelGrids;          // AssetManager::PixelFontSizes of GridsSource, at GridsGen
    const GlyphInfo* GridsSource = nullptr;
    uint64_t GridsGen = UINT64_MAX;
    float DrawPixelSize = -1.0f;
    const Font* DrawFont = nullptr;

    bool NeedsUpdate(const std::string& text, float boxW, float boxH,
                     float reqFontSize, const Font* font, const UITextFit& fit) const
    {
        if (!Valid) return true;
        if (LastText != text) return true;
        if (LastBoxWidth != boxW || LastBoxHeight != boxH) return true;
        if (LastRequestedFontSize != reqFontSize) return true;
        if (LastFontPtr != (font ? font->glyphs : nullptr)) return true;
        if (font && LastFontBaseSize != font->baseSize) return true;
        if (font && LastFontTexture != font->texture.id) return true;
        if (LastWrap != fit.Wrap || LastAutoShrink != fit.AutoShrink || LastMinFontSize != fit.MinFontSize) return true;
        return false;
    }

    void UpdateInputs(const std::string& text, float boxW, float boxH,
                      float reqFontSize, const Font* font, const UITextFit& fit)
    {
        LastText = text;
        LastBoxWidth = boxW;
        LastBoxHeight = boxH;
        LastRequestedFontSize = reqFontSize;
        LastFontPtr = font ? font->glyphs : nullptr;
        LastFontBaseSize = font ? font->baseSize : -1;
        LastFontTexture = font ? font->texture.id : 0;
        LastWrap = fit.Wrap;
        LastAutoShrink = fit.AutoShrink;
        LastMinFontSize = fit.MinFontSize;
        Valid = true;
        LineWidths.clear();
    }
};

// A LocKey resolved for drawing: formatted again only when the key, args or locale changed (read several
// times a frame: fit, draw). An empty key gives `text` as is.
struct UILocCache
{
    std::string Text, Key;
    std::map<std::string, std::string> Args;
    uint64_t Revision = UINT64_MAX;

    const std::string& Resolve(const std::string& text, const std::string& key, const std::map<std::string, std::string>& args)
    {
        if (key.empty()) return text;
        const uint64_t revision = LocalizationManager::Get().Revision();
        if (revision != Revision || key != Key || args != Args) {
            Text = LocalizationManager::Get().Text(key, {args.begin(), args.end()});
            Key = key;
            Args = args;
            Revision = revision;
        }
        return Text;
    }
};

class UIElement;
// owner: the element drawing the text; with a FontGroup it draws at its group's shared size.
void DrawTextFitted(const Font& font, const std::string& text, Rectangle box,
                    float requestedFontSize, float spacing, Color color,
                    UIHAlign hAlign, UIVAlign vAlign, const UITextFit& fit,
                    UITextMemo* memo = nullptr, UIElement* owner = nullptr);

struct UIStyleState
{
    bool  HasBackground = false; Color Background {};
    bool  HasBorder     = false; Color Border {};
    bool  HasText       = false; Color Text {};
    float ScaleMul      = 1.0f;  // visual scale response, 1.0 = none
};

struct UIStyle
{
    Color BackgroundColor { 40, 42, 48, 255 };
    Color TextColor       { 235, 235, 235, 255 };
    Color BorderColor     { 0, 0, 0, 0 };
    float BorderWidth  = 0.0f;
    float CornerRadius = 0.0f;          // logical units, 0 = square

    Color   ShadowColor  { 0, 0, 0, 0 };
    Vector2 ShadowOffset { 0.0f, 2.0f };
    float   ShadowSize   = 0.0f;

    float PaddingLeft = 0.0f, PaddingRight = 0.0f, PaddingTop = 0.0f, PaddingBottom = 0.0f;
    float FontSize = 18.0f;             // logical
    // A type-scale role ("body", "title", ...): set, the canvas sets FontSize to the theme's TypeScale
    // size for it times the canvas text scale (UITheme::ApplyTypeScale).
    std::string FontRole;

    bool       UseNPatch = false;
    const Texture2D* NPatchTexture = nullptr; // cache entry; id may still be 0 until uploaded
    int        NPatchLeft = 0, NPatchTop = 0, NPatchRight = 0, NPatchBottom = 0;

    bool Beveled = false;               // classic raised/sunken edges
    bool BevelSunken = false;
    Color BevelLight { 255, 255, 255, 160 };
    Color BevelDark  {   0,   0,   0, 110 };

    UIStyleState Hover, Pressed, Disabled;

    float TransitionDuration = 0.0f;    // seconds; 0 = instant
};

struct UITheme
{
    std::unordered_map<std::string, UIStyle> Styles;
    Font DefaultFont {};
    bool HasDefaultFont = false;

    // Named text sizes (logical, at text scale 1) that styles pick by FontRole.
    std::unordered_map<std::string, float> TypeScale;

    UIStyle&       Get(const std::string& styleClass);
    const UIStyle& Resolve(const std::string& styleClass) const;
    // A new style class that starts as a copy of `base` (no live chain: later edits to base do not reach it).
    UIStyle&       Extend(const std::string& styleClass, const std::string& base);
    void           ApplyTypeScale(float textScale);
};

UITheme CreateModernTheme();
UITheme CreateClassicTheme();
UITheme CreateClassicDarkTheme();

inline Color WithOpacity(Color c, float o)
{
    if (o <= 0.0f) return Color{ c.r, c.g, c.b, 0 };
    if (o >= 1.0f) return c;
    return Color{
        c.r,
        c.g,
        c.b,
        static_cast<unsigned char>(static_cast<float>(c.a) * o)
    };
}

class UICanvas;

struct UIElementTransitionState
{
    Color CurrentBg { 0, 0, 0, 0 };
    Color CurrentBorder { 0, 0, 0, 0 };
    Color CurrentText { 0, 0, 0, 0 };
    float CurrentScaleMul = 1.0f;
    bool Initialized = false;
};

class UIElement
{
public:
    UIElement(UIElementType type, uint64_t id, const std::string& name, UICanvas* canvas);
    virtual ~UIElement() = default;

    UIElement(const UIElement&) = delete;
    UIElement& operator=(const UIElement&) = delete;

    uint64_t     Id = 0;
    std::string  Name;
    UIElementType Type = UIElementType::Root;
    UICanvas*    Canvas = nullptr;

    UIElement*   Parent = nullptr;                              // non-owning
    // false once removed from the tree. Python holds raw element pointers, so a removed subtree is not
    // freed: the canvas parks it (RetireSubtree) and every mutating entry point no-ops on !Alive.
    bool         Alive = true;
    std::vector<std::unique_ptr<UIElement>> Children;           // owning, draw order

    bool  Visible = true;             // false -> not drawn, not hit tested, children skipped
    bool  Enabled = true;             // false -> uses the disabled style, no interaction
    float Opacity = 1.0f;             // 0..1, multiplies down the tree

    RectTransform Rect;
    float         FlexWeight = 1.0f;
    bool          IgnoreLayout = false;  // a layout container leaves it on its anchors (a backdrop, a badge)
    // Height follows the content: a text's or button's wrapped text at its width, a container's children
    // (stacked for vertical, tallest for horizontal, flowed rows for wrap, lowest bottom otherwise).
    bool          FitHeight = false;
    std::string   StyleClass;         // "" -> the type's default class
    // Texts (text, button, input) of one font group draw at one size: the smallest any of them fits at.
    std::string   FontGroup;
    UITextMemo    GroupMemo;          // this element's own fit, which it reports to its group
    bool          ClipChildren = false;
    bool          Focusable = false;

    // layout container
    UILayoutMode Layout = UILayoutMode::None;
    float PaddingLeft = 0.0f, PaddingRight = 0.0f, PaddingTop = 0.0f, PaddingBottom = 0.0f;
    float Spacing = 0.0f;
    UIAlign ChildAlignment = UIAlign::Start;
    bool ExpandChildrenWidth = false, ExpandChildrenHeight = false;
    bool StretchChildrenCrossAxis = true;
    bool WrapChildren = false;          // horizontal: children flow onto new rows when the row is full

    // resolved each frame, never set by the game
    Rectangle    ResolvedRect {};
    float        ResolvedOpacity = 1.0f;
    UITransform2D ResolvedTransform {};

    // optional per-element style override
    std::unique_ptr<UIStyle> CustomStyle;
    UIStyle& GetOrCreateCustomStyle();

    // animated interaction state
    UIElementTransitionState TransitionState;

    std::string TypeName() const;

    void Remove();
    void ClearChildren();
    void BringToFront();
    void SendToBack();

    // Tween helpers
    UITweenHandle MoveTo(float x, float y, float duration, const std::string& ease = "linear");
    UITweenHandle TranslateTo(float x, float y, float duration, const std::string& ease = "linear");
    UITweenHandle SizeTo(float w, float h, float duration, const std::string& ease = "linear");
    UITweenHandle ScaleTo(float sx, float sy, float duration, const std::string& ease = "linear");
    UITweenHandle RotateTo(float degrees, float duration, const std::string& ease = "linear");
    UITweenHandle FadeTo(float opacity, float duration, const std::string& ease = "linear");
    UITweenHandle ColorTo(Color color, float duration, const std::string& ease = "linear");
    UITweenHandle TextColorTo(Color color, float duration, const std::string& ease = "linear");

    virtual void DrawContent(const UIStyle& resolvedStyle, float opacity);
    // FitHeight: the content's height at `width` (padding included); < 0 when the element has no text.
    virtual float TextHeightAt(float width) { (void)width; return -1.0f; }
    // What its text height depends on, for the layout cache (0 = no text).
    virtual uint64_t TextLayoutKey() const { return 0; }
};

class UIPanel : public UIElement
{
public:
    UIPanel(uint64_t id, const std::string& name, UICanvas* canvas);
    bool BlocksInputBehind = false;
    void DrawContent(const UIStyle& resolvedStyle, float opacity) override;
};

class UIText : public UIElement
{
public:
    UIText(uint64_t id, const std::string& name, UICanvas* canvas);

    std::string Text;
    std::string LocKey;                              // set: drawn instead of Text (ResolvedText)
    std::map<std::string, std::string> LocArgs;
    Font*       FontOverride = nullptr;
    float       FontSize = 0.0f;      // 0 = take from style
    Color       ColorOverride { 0, 0, 0, 0 }; // a == 0 -> take from style
    UIHAlign    HAlign = UIHAlign::Left;
    UIVAlign    VAlign = UIVAlign::Top;
    bool        Wrap = true;
    bool        AutoShrink = true;
    float       MinFontSize = 8.0f;

    UITextMemo  Memo;
    UITextFit   Fit() const { return { Wrap, AutoShrink, MinFontSize }; }

    void EnsureFitted();
    float FittedFontSize();
    float MeasuredWidth();
    float MeasuredHeight();
    UITextMemo  FitMemo;              // FitHeight's measure at an unbounded height
    float TextHeightAt(float width) override;
    uint64_t TextLayoutKey() const override;

    void DrawContent(const UIStyle& resolvedStyle, float opacity) override;
    const std::string& ResolvedText() const { return m_loc.Resolve(Text, LocKey, LocArgs); }   // the text as drawn

private:
    mutable UILocCache m_loc;
};

class UIImage : public UIElement
{
public:
    UIImage(uint64_t id, const std::string& name, UICanvas* canvas);

    const Texture2D* Texture = nullptr; // cache entry (id read at replay)
    std::shared_ptr<Video> VideoSource; // set: shows this video's current frame instead of Texture
    Color     Tint = WHITE;
    Rectangle SourceRect { 0.0f, 0.0f, 0.0f, 0.0f }; // zero size = whole texture
    bool      PreserveAspect = false;
    // Pixel art: draw at the largest whole multiple of the source size that fits, centred, so every
    // pixel stays the same size. A rect smaller than the source falls back to PreserveAspect.
    bool      IntegerScale = false;

    void DrawContent(const UIStyle& resolvedStyle, float opacity) override;
};

class UIButton : public UIElement
{
public:
    UIButton(uint64_t id, const std::string& name, UICanvas* canvas);

    std::string Text;
    std::string LocKey;                              // set: drawn instead of Text (ResolvedText)
    std::map<std::string, std::string> LocArgs;
    std::string Icon;                 // procedural icon name
    const Texture2D* IconTexture = nullptr; // cache entry; wins over Icon if set
    bool        Wrap = false;
    bool        AutoShrink = true;
    float       MinFontSize = 8.0f;
    Vector2     HitScale = { 1.0f, 1.0f }; // pointer area grown about the centre per axis (1.25 = 25% bigger); drawing unchanged

    bool Hovered = false;
    bool Pressed = false;
    bool Clicked = false;             // transient, cleared at start of update

    Rectangle HitRect() const {
        const Rectangle& r = ResolvedRect;
        const float gx = r.width * (HitScale.x - 1.0f) * 0.5f, gy = r.height * (HitScale.y - 1.0f) * 0.5f;
        return { r.x - gx, r.y - gy, r.width + 2.0f * gx, r.height + 2.0f * gy };
    }

    UITextMemo  Memo;
    UITextFit   Fit() const { return { Wrap, AutoShrink, MinFontSize }; }

    void EnsureFitted();
    float FittedFontSize();
    float MeasuredWidth();
    float MeasuredHeight();
    UITextMemo  FitMemo;              // FitHeight's measure at an unbounded height
    float TextHeightAt(float width) override;
    uint64_t TextLayoutKey() const override;

    void DrawContent(const UIStyle& resolvedStyle, float opacity) override;
    const std::string& ResolvedText() const { return m_loc.Resolve(Text, LocKey, LocArgs); }   // the text as drawn

private:
    mutable UILocCache m_loc;
};

class UIProgressBar : public UIElement
{
public:
    UIProgressBar(uint64_t id, const std::string& name, UICanvas* canvas);

    float Value = 0.0f;               // clamped 0..1
    UIFillDirection FillDirection = UIFillDirection::LeftToRight;

    void DrawContent(const UIStyle& resolvedStyle, float opacity) override;
};

class UISlider : public UIElement
{
public:
    UISlider(uint64_t id, const std::string& name, UICanvas* canvas);

    float Value = 0.0f;
    float MinValue = 0.0f;
    float MaxValue = 1.0f;
    float Step = 0.0f;                // 0 = continuous
    bool  Vertical = false;

    bool Hovered = false;
    bool Pressed = false;
    bool Changed = false;             // transient, cleared at start of update

    void DrawContent(const UIStyle& resolvedStyle, float opacity) override;
};

class UIScrollView : public UIElement
{
public:
    UIScrollView(uint64_t id, const std::string& name, UICanvas* canvas);

    static constexpr float ScrollbarWidth = 8.0f;

    float ScrollOffset = 0.0f;        // logical y
    float ContentHeight = 0.0f;
    float DragThumbGrabOffset = 0.0f;

    Rectangle GetThumbRect() const;

    void DrawContent(const UIStyle& resolvedStyle, float opacity) override;
};

class UITextInput : public UIElement
{
public:
    UITextInput(uint64_t id, const std::string& name, UICanvas* canvas);

    std::string Text;
    std::string Placeholder;

    bool AllowLetters = true;
    bool AllowDigits = true;
    bool AllowSymbols = true;

    bool Hovered = false;
    bool Pressed = false;
    bool Submitted = false; // transient, cleared at start of update

    UITextMemo Memo;
    UITextFit  Fit() const { return { false, true, 8.0f }; }

    float CursorBlinkTimer = 0.0f;
    bool  CursorVisible = true;

    void DrawContent(const UIStyle& resolvedStyle, float opacity) override;
};

// Host hook asked about every node of a retired subtree: return true while anything outside the engine
// (a Python wrapper) still holds `element`. Installed once by the bindings so this header stays pybind-free.
// With no hook installed nothing outside can hold an element, so retired subtrees are freed at the next boundary.
using UIElementReferencedFn = bool (*)(const UIElement* element);
void SetUIElementReferencedHook(UIElementReferencedFn fn);

class UICanvas : public GameActor
{
public:
    UICanvas();
    ~UICanvas() override;

    void Update(float dt) override;
    void Drawing() override;

    // canvas scaler
    float ReferenceWidth = 1920.0f;
    float ReferenceHeight = 1080.0f;
    float Match = 0.5f;

    float ScaleFactor()   const;
    // A font group member's text size: the group's smallest last frame (own: this member's own fit,
    // reported for this frame). Groups settle in one frame and follow members that hide or change.
    float FontGroupSize(const std::string& group, float own);
    void  NextFontGroupFrame();   // Drawing: this frame's sizes become the caps of the next
    float LogicalWidth()  const;
    float LogicalHeight() const;

    UIElement* Root();
    UIElement* Add(UIElementType type, const std::string& name, UIElement* parent);
    UIElement* Find(const std::string& name);
    UIElement* FindById(uint64_t id);
    void ClearUI();
    void SetTheme(const UITheme& theme);
    UITheme& Theme();
    const UITheme& Theme() const;
    // Multiplies every FontRole style's type-scale size (a settings "text size" without rebuilding the theme).
    float TextScale() const { return m_textScale; }
    void SetTextScale(float scale);

    // DebugDraw outlines every rect, and the Problems() ones in red with their kind.
    bool DebugDraw = false;
    struct Problem { std::string Kind; const UIElement* Element; std::string Detail; };
    // The UI rules (docs/VISUAL-TESTS.md) on the shown elements: ZERO_SIZE, OFFSCREEN, OVERFLOW,
    // TEXT_CLIP, FONT_SHRUNK, UNSUPPORTED_GLYPH, BUTTON_OVERLAP. Lays the tree out first.
    std::vector<Problem> Problems();
    // The characters of `text` that the font `e` draws with has no glyph for.
    std::string MissingGlyphs(const UIElement& e, const std::string& text) const;

    UIElement* GetFocused() const { return m_focusedElement; }
    UIElement* GetHovered() const { return m_hoveredElement; }
    // True when this canvas would take the mouse at its current position: a widget or a
    // blocks_input_behind panel is under it, a scroll view is, or a press is captured. Uses the
    // layout resolved this frame (or last frame, before this canvas updates), never stale input.
    bool PointerOverUI() const;
    // The same question at a logical (canvas-space) point; PointerOverUI passes the mapped mouse.
    bool PointerOverUIAt(Vector2 logicalPos) const;
    // The element that gets the pointer at a logical point (nullptr: none), as HitTestTree decides it;
    // the agent channel asks it whether a click would land on the element it aims at.
    UIElement* PickElement(Vector2 logicalPos, UIElement** outHoveredScroll = nullptr) const;
    void SetFocused(UIElement* element);
    // Lays the tree out now (cached when nothing changed). Update and Drawing both call it; the
    // agent channel calls it to read what is about to be drawn.
    void ResolveLayoutTree();

    // Helpers
    UIPanel* AddPanel(const std::string& name = "", UIElement* parent = nullptr);
    UIText* AddText(const std::string& name = "", UIElement* parent = nullptr);
    UIImage* AddImage(const std::string& name = "", UIElement* parent = nullptr);
    UIButton* AddButton(const std::string& name = "", UIElement* parent = nullptr);
    UIProgressBar* AddProgressBar(const std::string& name = "", UIElement* parent = nullptr);
    UISlider* AddSlider(const std::string& name = "", UIElement* parent = nullptr);
    UIScrollView* AddScrollView(const std::string& name = "", UIElement* parent = nullptr);
    UITextInput* AddTextInput(const std::string& name = "",
                             UIElement* parent = nullptr,
                             const std::string& placeholder = "",
                             bool allowLetters = true,
                             bool allowDigits = true,
                             bool allowSymbols = true);

    // Tweens
    uint64_t StartTween(uint64_t targetElementId, UITweenProperty prop, const float* toValues, int count, float duration, UIEase ease);
    void KillTweensFor(UIElement* element, bool includeDescendants);
    void KillAllTweens();
    void UpdateTweens(float dt);

    bool IsTweenActive(uint64_t tweenId) const;
    bool IsTweenCompleted(uint64_t tweenId) const;
    float GetTweenProgress(uint64_t tweenId) const;
    void SetTweenDelay(uint64_t tweenId, float delay);
    float GetTweenDelay(uint64_t tweenId) const;
    void SetTweenLoops(uint64_t tweenId, int loops);
    int GetTweenLoops(uint64_t tweenId) const;
    void SetTweenYoyo(uint64_t tweenId, bool yoyo);
    bool GetTweenYoyo(uint64_t tweenId) const;
    void KillTween(uint64_t tweenId);
    void PauseTween(uint64_t tweenId);
    void ResumeTween(uint64_t tweenId);

    // Sequences
    UISequenceHandle Sequence();
    uint64_t CreateSequence();
    void UpdateSequences(float dt);
    void KillSequence(uint64_t seqId);
    void KillAllSequences();
    bool IsSequenceActive(uint64_t seqId) const;
    bool IsSequenceCompleted(uint64_t seqId) const;
    void PlaySequence(uint64_t seqId);
    void AddSequenceStep(uint64_t seqId, const UISequenceStep& step, bool isJoin);
    void AddSequenceInterval(uint64_t seqId, float seconds);

    // Internal management
    void NotifySubtreeRemoving(UIElement* subtree);
    // Takes ownership of a detached subtree and marks it dead. Its memory stays valid until ReclaimRetired
    // frees it: a subtree is freed only when no node in it is still referenced (see SetUIElementReferencedHook).
    void RetireSubtree(std::unique_ptr<UIElement> subtree);
    // Frees unreferenced retired subtrees. UICanvas::Update calls it first, before any tree walk, so it
    // never runs during iteration, drawing, or a Python callback. Public so native tests can drive it.
    void ReclaimRetired();
    size_t RetiredCount() const { return m_retired.size(); }
    // True while any element of this canvas (live or retired) is still referenced: a removed canvas actor
    // is then parked, not freed, because those elements live inside it and point back at it.
    bool AnyElementReferenced() const;
    uint64_t AllocateId() { return m_nextId++; }
    std::string UniqueName(const std::string& requestedName, UIElementType type);
    void ReleaseName(const std::string& name);

private:
    struct LayoutCacheEntry
    {
        std::vector<std::uint64_t> Signature;
        std::vector<Rectangle> ResolvedRects;
        std::vector<float> ScrollContentHeights;
    };

    void ClearTransientFlags(UIElement* element);
    void ResolveElementLayout(UIElement* element, const Rectangle& parentRect, bool content = true);
    void ApplyContainerLayout(UIElement* element);
    void ResolveElementContent(UIElement* element);
    float FitHeightAt(UIElement* element, float width);
    void ResolveTransformsAndOpacity(UIElement* element, const UITransform2D& parentTransform, float parentOpacity);

    void HitTestTree(Vector2 logicalMousePos, float dt);
    Vector2 LogicalMousePosition() const;
    void UpdateFocusNavigation(float dt);
    void UpdateStateTransitions(UIElement* element, float dt);

    void DrawElement(UIElement* element);
    void DrawDebugOverlay(UIElement* element, int depth);

    std::unique_ptr<UIElement> m_root;
    std::unordered_map<std::string, float> m_fontGroupsNow, m_fontGroupsLast;   // smallest fit per group
    std::vector<std::unique_ptr<UIElement>> m_retired;
    std::vector<std::uint64_t> m_layoutSignature;
    std::vector<LayoutCacheEntry> m_layoutCache;
    UITheme m_theme;
    float m_textScale = 1.0f;
    uint64_t m_themeGeneration = 0;     // in the layout signature: fit heights depend on fonts and sizes
    uint64_t m_nextId = 1;
    // Live tree indexes behind Find/FindById, which scripts and tweens call every frame. Names are
    // unique per canvas; both maps are filled only in Add and emptied only in NotifySubtreeRemoving,
    // so a retired element is never found (Hard rule 22).
    std::unordered_map<std::string, UIElement*> m_liveNames;
    std::unordered_map<uint64_t, UIElement*> m_liveIds;

    UIElement* m_hoveredElement = nullptr;
    UIElement* m_capturedElement = nullptr;
    UIElement* m_focusedElement = nullptr;
#ifdef IYI_MOBILE
    UIScrollView* m_touchScroll = nullptr;
    Vector2 m_touchStart{}, m_touchLast{};
    float m_scrollVelocity = 0.0f;
    bool m_touchDragging = false;
#endif

    float m_focusNavRepeatTimer = 0.0f;

    std::vector<UITweenData> m_tweens;
    std::vector<UISequenceData> m_sequences;
    uint64_t m_nextTweenId = 1;
    uint64_t m_nextSequenceId = 1;
};

// Procedural icons
void DrawIcon(const std::string& name, Rectangle area, Color color);
const std::vector<std::string>& GetIconNames();
Vector2 WarpedScreenPoint(Vector2 screenPoint);   // a UI-drawn screen point as seen after the radial warp

} // namespace IYI::UI

namespace IYI::Api {
// Spawns a UICanvas actor into the current scene (empty name: "Canvas"; script: a Python actor script or "").
UI::UICanvas* UISpawnCanvas(const std::string& name = "Canvas", const std::string& script = "", float referenceWidth = 1920.0f,
                            float referenceHeight = 1080.0f, float match = 0.5f, bool persistent = false);
// Whether any live canvas of the current scene has the pointer (UICanvas::PointerOverUI): a world actor asks before a click is a world action.
bool UIPointerOverUI();
} // namespace IYI::Api
