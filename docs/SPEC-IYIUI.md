# IYIUI, Implementation Contract

Approved extension of `SPEC.md`. Same rules apply: this file wins over the source
plan where they differ, because it records decisions the project owner approved.

IYIUI is a retained-mode UI/HUD system: Unity-like anchors and canvas scaling,
DOTween-like animation ergonomics from Python, swappable themes, procedural
visuals, and the engine's existing instant hot-reload workflow.

**No new dependency.** raylib only, including `rlgl.h`, which ships with raylib.
No raygui, Dear ImGui, RmlUi, Yoga, external tween or icon library.

## Toolbox: read before building a screen

Most UI bugs in the games came from doing by hand what IYIUI already does. Before you place
elements with `y = i * row_h` or guess a text's height from its length, use these:

| Need | Use | § |
|---|---|---|
| A list, a row, a column | `layout_mode` + `spacing` + `padding` (game_kit `stack()`) | 5 |
| Chips or tags that flow onto new rows | `layout_mode = "horizontal"`, `wrap_children = True` (`stack(axis="wrap")`) | 5 |
| A bubble, card or row as tall as its text or children | `fit_height = True` (game_kit `text(fit=True)`, `stack(fit=True)`) | 5 |
| One size for a row of texts (Pause / 1x / 2x) | `font_group = "speed"` on each | 4 |
| Text sizes that read as one scale, a "text size" setting | `theme.type_scale` + `style.font_role`, `canvas.text_scale` | 8 |
| A style that differs from another in one field | `theme.extend("chip_on", "chip")` | 8 |
| A movable window with a close box | game_kit `window()` + `update_windows()` | game_kit |
| Fills the rest, stays fixed | `expand_children_*` + `flex_weight` (0 = fixed) | 5 |
| A backdrop or badge inside a laid-out container | `ignore_layout = True` | 5 |
| Why is it wrong on screen? | `canvas.debug_draw = True` (problems in red), `canvas.problems()`, agent `"ui_problems": true` | 17 |

Check every new screen with `"ui_problems": true` (docs/spec/cli.md §3.1): it reports overflow, clipped
or shrunk text, missing glyphs and overlapping clickable buttons by name.

---

## 0. Approved decisions

| Topic | Decision |
|---|---|
| Namespace | `IYI::UI` |
| Python surface | `engine.ui` submodule (`m.def_submodule("ui")`) |
| Canvas is an actor | `UICanvas : GameActor`, `ScreenSpace` forced `true` |
| Elements are not actors | owned by their canvas, never in `ActorRegistry` |
| Layout | cached by exact layout-input signature; misses resolve parent → child and retain the last eight variants |
| Font scaling | one atlas, `DrawTextEx` called with `font_size * scale_factor`; a 2x/4x atlas when drawn far larger (§9c) |
| Visual transform | `rlPushMatrix`/`rlTranslatef`/`rlRotatef`/`rlScalef` for drawing; a small affine struct for hit testing only |
| Clipping | `BeginScissorMode` with an intersection stack, axis-aligned always |
| Phases | Phase 1 = everything except tweens/Reload/demo. Phase 2 = tweens, sequences, state transitions, `Reload`, `clear_ui`, demo, docs |

### Additions beyond the source plan (approved)

| Addition | Phase | Why |
|---|---|---|
| `canvas.debug_draw` | 1 | One bool that outlines every resolved rect and labels it. Answers "why is my panel invisible" instantly. A flag, not a framework. |
| `UISlider` | 1 | Settings menus need it immediately; it is `UIProgressBar` plus drag input. |
| Keyboard / gamepad focus navigation | 1 | Controller-playable UI. Focus ring, directional move, activate. |
| `tween.completed`, `tween.progress` | 2 | The plan forbids Python callbacks in native tweens and says "poll instead", but defined nothing to poll. |
| `font_group` | 2026-11-12 | Texts that read as one row draw at one size (§4). |
| `fit_height`, `wrap_children` | 2026-11-12 | Content-sized heights and flowing rows (§5); games guessed text heights from character counts. |
| Type scale (`type_scale`, `font_role`, `canvas.text_scale`) and `theme.extend` | 2026-11-12 | Named sizes and copy-based style variants (§8). Gry asked for style inheritance; `extend` is a copy, not a live chain, so resolution stays shallow. |
| `canvas.problems()`, `debug_draw` problems, agent `ui_problems` | 2026-11-12 | The visual tests' UI rules in the engine, live on screen (§17). |

---

## 1. Files

```text
IYIEngine/
├── include/IYI/IYIUI/
│   ├── IYIUI.h          types, UIElement tree, widgets, theme, UICanvas
│   └── IYIUITween.h     tween + sequence types and easing enum
└── src/
    ├── IYIUI.cpp        element tree, widget creation, tween shortcuts, canvas lifecycle
    ├── IYIUIDraw.cpp    drawing, text fitting (DrawTextFitted), clipping (PushScissor)
    ├── IYIUIInput.cpp   input, hit testing, focus navigation, pointer mapping
    ├── IYIUILayout.cpp  layout signature/cache, rects, containers, transforms
    ├── IYIUITheme.cpp   stock themes, style class resolution
    ├── IYIUITween.cpp   canvas tweens/sequences, update + easing functions
    ├── IYIUIIcons.cpp   procedural icon drawing
    └── IYIUIBindings.cpp  ALL pybind11-facing UI code
```

Public headers may include `raylib.h`. They must **not** include `Python.h`,
`pybind11/*`, `nlohmann/json.hpp`, or `rlgl.h` (rlgl belongs in the `IYIUI*.cpp` sources).

`CMakeLists.txt`: the sources are part of `IYIEngineCore`. No new
`FetchContent`, no new link library, no change to warning flags.

---

## 2. Coordinate model, get this right first, everything depends on it

Three spaces:

1. **Logical space**, where all layout happens and every number a game author
   writes lives. The canvas root rect is `(0, 0, logical_width, logical_height)`.
2. **Screen space**, actual pixels.
3. **Element-local space**, used only for hit testing rotated/scaled elements.

```text
width_scale   = screen_width  / reference_width
height_scale  = screen_height / reference_height
scale_factor  = pow(width_scale, 1 - match) * pow(height_scale, match)

logical_width  = screen_width  / scale_factor
logical_height = screen_height / scale_factor
```

`match`: `0.0` favours width, `1.0` favours height, `0.5` balanced. Clamp to
`[0,1]`. A negative `match` is "fit" (since 2026-11-13): `scale_factor = min(width_scale, height_scale)`,
so the logical size is never below the reference on either axis; a 5:4 or 16:10 screen gets extra height
instead of a narrower layout (HR uses it: its screens are laid out for 1920 wide). Guard against a zero or negative reference size and against a zero
screen dimension (a minimised window), fall back to `scale_factor = 1`.

Consequence, and the whole point: a fixed `size_delta` control keeps a consistent
apparent size across resolutions, while anchors still see the true aspect ratio,
so an ultrawide window gains logical horizontal space instead of stretching.

Everything stored in `RectTransform` and `ResolvedRect` is **logical**. The canvas
applies `scale_factor` once, as an `rlScalef` at the root, when drawing. Hit
testing divides the raylib mouse position by `scale_factor` to enter logical
space. Font sizes are logical and reach `DrawTextEx` multiplied by
`scale_factor`; scissor rectangles are converted to pixels before
`BeginScissorMode`.

---

## 3. `RectTransform`

```cpp
struct RectTransform
{
    Vector2 AnchorMin { 0.5f, 0.5f };
    Vector2 AnchorMax { 0.5f, 0.5f };
    Vector2 Pivot     { 0.5f, 0.5f };

    Vector2 AnchoredPosition { 0.0f, 0.0f };
    Vector2 SizeDelta        { 100.0f, 100.0f };

    Vector2 MinSize { 0.0f, 0.0f };
    Vector2 MaxSize { 0.0f, 0.0f };   // 0 or negative on an axis = no limit

    Vector2 Scale   { 1.0f, 1.0f };
    float   Rotation = 0.0f;          // degrees, clockwise
    Vector2 Translation { 0.0f, 0.0f }; // visual offset after Scale/Rotation
};
```

Resolution against a parent rect `P`:

```text
ax0 = P.x + P.width  * anchor_min.x      ax1 = P.x + P.width  * anchor_max.x
ay0 = P.y + P.height * anchor_min.y      ay1 = P.y + P.height * anchor_max.y

if anchor_min.x == anchor_max.x:  width  = size_delta.x
else:                             width  = (ax1 - ax0) + size_delta.x
same for height on y

clamp width/height to min_size / max_size where those are > 0

if anchor_min.x == anchor_max.x:                     // point anchor
    x = ax0 + anchored_position.x - pivot.x * width
else:                                                // separated (stretch) anchor
    x = ax0 + anchored_position.x + (ax1 - ax0 - width) * pivot.x
same shape for y
```

The pivot term in the separated case is what makes the inset symmetric. With the
default pivot `(0.5, 0.5)`, `anchor_min=(0,0)`, `anchor_max=(1,1)` and
`size_delta=(-40,-40)` on a 1920×1080 canvas, the result is
`(20, 20, 1880, 1040)`, a 20-unit margin on all four sides. Omitting the pivot
term pushes the whole inset to one side, which is wrong.

So with separated anchors `size_delta` acts as an inset/outset: `(0,0)` gives a
full-stretch child, negative values inset it, positive values overflow the parent.

`ResolvedRect` is the axis-aligned logical rect produced here, **before** `Scale`,
`Rotation` and `Translation`, those are visual only and never feed layout, so a scaled button
does not push its siblings around.

---

## 4. `UIElement`

```cpp
enum class UIElementType { Root, Panel, Text, Image, Button, ProgressBar, Slider, ScrollView };

enum class UILayoutMode  { None, Horizontal, Vertical };
enum class UIAlign       { Start, Center, End };
enum class UIHAlign      { Left, Center, Right };
enum class UIVAlign      { Top, Middle, Bottom };
```

Common state:

```cpp
uint64_t     Id = 0;              // monotonic within the canvas, starts at 1
std::string  Name;                // unique within the canvas: Button, Button_2, ...
UIElementType Type;

UIElement*   Parent = nullptr;                              // non-owning
std::vector<std::unique_ptr<UIElement>> Children;           // owning, draw order

bool  Visible = true;             // false -> not drawn, not hit tested, children skipped
bool  Enabled = true;             // false -> uses the disabled style, no interaction
float Opacity = 1.0f;             // 0..1, multiplies down the tree

RectTransform Rect;
float         FlexWeight = 1.0f;
std::string   StyleClass;         // "" -> the type's default class (see §8)
std::string   FontGroup;          // texts (text, button, input) of one group draw at one size: the smallest
                                  // any of them fits at (last frame's), e.g. a row of buttons that read as one
bool          FitHeight = false;   // height follows the content (§5 "Fit height")
bool          ClipChildren = false;
bool          Focusable = false;   // Button and Slider default to true

// layout container
UILayoutMode Layout = UILayoutMode::None;
float PaddingLeft = 0, PaddingRight = 0, PaddingTop = 0, PaddingBottom = 0;
float Spacing = 0;
UIAlign ChildAlignment = UIAlign::Start;
bool ExpandChildrenWidth = false, ExpandChildrenHeight = false;  // main axis only
bool StretchChildrenCrossAxis = true;                            // cross axis

// resolved each frame, never set by the game
Rectangle    ResolvedRect {};
float        ResolvedOpacity = 1.0f;
UITransform2D ResolvedTransform {};   // logical -> logical, includes ancestors
```

Names: reuse the `GameActor` idea, if `Button` is taken try `Button_2`,
`Button_3`. An empty requested name becomes the type name (`Panel`, `Text`, …).
No GUIDs. Names are released when the element is actually destroyed.

Tree operations:

```text
canvas.find(name)        -> element or None
canvas.find_by_id(id)    -> element or None
element.remove()          removes the element and its whole subtree
element.clear_children()  removes all descendants, keeps the element
element.bring_to_front()  moves it last among its siblings
element.send_to_back()    moves it first among its siblings
```

Removal must kill every tween targeting the element **or any descendant**, and
clear the canvas's focus/hover/capture pointers if they referenced a removed
element. Missing this is a use-after-free, not a cosmetic bug.

**Lifetime rule:** a Python `UIElement` reference is a non-owning pointer, but a
removed element is never freed while a Python wrapper still references it. `remove()`, `clear_children()`
and `clear_ui()` detach the subtree, mark every element in it `alive = False`
(`element.alive`, read-only) and park it in a canvas-owned retired list. A retained
reference to a retired element is therefore safe to read and inert to use:
`remove()`, `clear_children()`, `bring_to_front()`, `send_to_back()` are no-ops;
tween helpers (`move_to`, `fade_to`, ...) refuse: no tween is created and they return an
inactive handle (`active == False`), and sequence `append_*` / `join_*` calls with a
retired element are silently ignored (no step added), so nothing keeps running on a
dead element; `canvas.add_*` with a retired `parent`
returns `None`; `canvas.focused = retired` clears focus. `find()` / `find_by_id()`
never return a retired element, and its name is released for reuse. Reclamation:
at the start of every `UICanvas::Update` (never during iteration, drawing or a Python
callback) the canvas frees each retired subtree in which no element has a live Python
wrapper; one referenced element anywhere in a subtree keeps the whole subtree. The
check is a host hook (`SetUIElementReferencedHook`, installed by the bindings, which
asks pybind11's instance registry), so `IYIUI.h` stays pybind-free. With no hook
installed (native tests) nothing outside can hold an element, so retired subtrees are
freed at the next `Update` / `ReclaimRetired()`. The canvas itself is an actor: a
destroyed `UICanvas` is parked like any actor (SPEC §12) while it or any of its elements
is still wrapped, and a dead canvas raises `ReferenceError` like a dead actor. Tweens and sequence steps are already
killed on removal and stay killed. Still keep the **name or id** and
re-resolve rather than holding elements across frames.

---

## 5. Layout containers

Only what §9 of the plan allows. On an element whose `Layout != None`:

1. Compute the content box: `ResolvedRect` inset by the four paddings.
2. Lay visible children out along the axis in sibling order, separated by
   `Spacing`. Each child's size on the main axis is its own resolved size unless
   `ExpandChildren*` is set for that axis, in which case children with a positive
   `FlexWeight` share the remaining space (below).
3. `ChildAlignment` positions the whole run on the main axis (`Start`, `Center`,
   `End`).
4. The two axes are controlled by **separate, unambiguous flags**, because
   overloading one flag for both would need tri-state defaults:

   - `ExpandChildrenWidth` / `ExpandChildrenHeight` (default `false`):
     **main axis only**: when set for the container's own axis, children with a
     positive `FlexWeight` share the main-axis space the others leave, by weight.
   - `FlexWeight` (`flex_weight`, default `1.0f`), per-child layout weight in an
     expanding container. `0` keeps the child's own main-axis size (a fixed row);
     `> 0` gives it `free * (weight / weightSum)`, where `free` is the content box
     less the spacing and every fixed child, clamped to the child's `MinSize` /
     `MaxSize` on that axis (an overfull run overflows, and a scroll view scrolls).
     Untouched weights (all 1) split the box equally, as before. Typical: a card
     whose title and buttons stay fixed while one row takes the rest, or rows that
     start at their natural height and grow in proportion (`weight = min = height`).
     Since 2026-11-03; before, every child flexed and weight 0 meant zero size.
   - `IgnoreLayout` (`ignore_layout`, default `false`): the container skips the child, which keeps
     its own anchors inside the container's rect (stretch it for a backdrop under the laid-out
     siblings; it still draws in child order and counts toward a scroll view's content). Since 2026-11-06.
   - `StretchChildrenCrossAxis` (default **`true`**), children fill the content
     box on the **cross** axis. When `false`, a child keeps its own cross-axis
     size and is aligned by `ChildAlignment`.

   The cross-axis default is `true` deliberately: with it `false`, a child that
   only sets its main-axis size (`size_delta = (0, 40)` in a vertical list)
   resolves to zero width and silently disappears. That is the most common UI
   mistake there is and not worth inflicting by default.

   `StretchChildrenCrossAxis` is one bool beyond the source plan's §9 flag list,
   added for exactly this reason.
4. A laid-out child's `AnchoredPosition` is **not** mutated, the container writes
   the child's `ResolvedRect` directly. The game's authored values stay untouched,
   so switching `Layout` back to `None` restores manual positioning exactly.

5. `WrapChildren` (`wrap_children`, default `false`), horizontal only: children keep their own sizes
   and flow left to right, starting a new row (`Spacing` below the tallest of the last row) when the
   next one would pass the content box's right edge. Expand, flex and alignment do not apply to a
   wrapping row. Since 2026-11-12.

Invisible children take no space. No grid, no flexbox, no constraint solver.

### Fit height

`FitHeight` (`fit_height`, default `false`, since 2026-11-12): the element's height is its content's,
clamped to `MinSize.y` / `MaxSize.y`; its width still comes from its anchors (or its layout parent).

| Element | Content height |
|---|---|
| Text, Button | its text wrapped at its width at the requested font size, plus the style's vertical padding |
| Vertical container | the children's heights + `Spacing` between + top and bottom padding |
| Horizontal container | the tallest child + padding; with `WrapChildren`, the flowed rows |
| Anything else | the lowest child bottom (from its own top) + `PaddingBottom` |

A fit child of a stretching vertical container is measured at the container's inner width, and of a
non-stretching horizontal one at its flexed width. The pivot still applies: a bottom-pivot fit element
grows upward. Children are measured on their own anchors in a box of the element's old height, so
anchor the children of a fit parent to its top. A fit chain costs one extra measure per level (text
measures are memoized, §9b).

Typical: a chat bubble (a `fit_height` vertical container with padding, holding a `fit_height` text),
stacked in a vertical container with `child_alignment = "end"` so the newest sits at the bottom.

---

## 6. Visual transform and opacity

```cpp
struct UITransform2D          // 2x3 affine: [a c tx ; b d ty]
{
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;

    static UITransform2D Translate(float x, float y);
    static UITransform2D ScaleRotateAround(Vector2 origin, Vector2 scale, float degrees);
    UITransform2D Compose(const UITransform2D& child) const;   // this * child
    Vector2 Apply(Vector2 p) const;
    Vector2 ApplyInverse(Vector2 p) const;                     // for hit testing
};
```

Per element, the local transform is scale-then-rotate about the pivot point of
`ResolvedRect`, plus any tween translation offset. `ResolvedTransform =
parent.ResolvedTransform.Compose(local)`.

**Drawing** does not transform primitives by hand. Around each element's own
drawing, push the local transform onto rlgl:

```cpp
rlPushMatrix();
rlTranslatef(pivotX, pivotY, 0.0f);
rlRotatef(rotation, 0.0f, 0.0f, 1.0f);
rlScalef(scale.x, scale.y, 1.0f);
rlTranslatef(-pivotX, -pivotY, 0.0f);
// ... every raylib draw call for this element and its children ...
rlPopMatrix();
```

This makes rotation and scale work uniformly for rounded rectangles, N-patches
and text without special cases. `ResolvedTransform` exists **only** so hit testing
can run `ApplyInverse` on the mouse position.

Opacity is multiplicative: `ResolvedOpacity = parent.ResolvedOpacity * Opacity`.
Never mutate a theme colour, compute the draw colour:

```cpp
Color WithOpacity(Color c, float o);   // c.a = (unsigned char)(c.a * clamp(o,0,1))
```

Every single draw call in IYIUI goes through it.

**Documented limitation:** clipping is always axis-aligned. A rotated element that
clips uses the axis-aligned bounding box of its resolved rect, because
`BeginScissorMode` is a screen-space rectangle and stencil masks are out of scope.

---

## 7. Clipping

A scissor stack in `IYIUIDraw.cpp`:

```text
push(rect):  new = intersect(current_top_or_full_screen, rect_in_pixels)
             BeginScissorMode(new)
pop():       EndScissorMode(); if the stack is non-empty, BeginScissorMode(new top)
```

**The clip rectangle must be transformed the same way the drawing is.**
`BeginScissorMode` is a screen-space GPU scissor and is completely unaffected by
the rlgl model-view matrix, while every element draws inside
`rlPushMatrix`/`rlTranslatef`/`rlRotatef`/`rlScalef` (§6). Converting a clip rect
with `scale_factor` alone therefore places it correctly only while every
transform in the ancestor chain is the identity. The moment anything is tweened,
scaled by a hover/pressed `ScaleMul`, or rotated, the scissor sits where the
content is not, and text and children are sliced at the edges.

So before converting to pixels, transform the rectangle by the element's
accumulated `ResolvedTransform` and take the axis-aligned bounding box of the
four transformed corners (clipping is axis-aligned by contract, §6):

```text
corners = the 4 corners of the logical rect
transformed = ResolvedTransform.Apply(each corner)
aabb = min/max of the transformed corners
pixels = aabb * scale_factor
```

Do this inside the scissor push itself, not at each call site, so every present
and future clip is correct by construction. The renderer keeps the current
accumulated transform in the same place it keeps the current scale factor.

Convert logical → pixels with `scale_factor` after that transform. An empty
intersection means the subtree is entirely hidden: skip drawing it rather than
calling `BeginScissorMode` with a negative size. The stack must be empty again at
the end of `UICanvas::Drawing()`, assert it in debug.

**Culling.** An element whose transformed rect, grown by what it may draw past it
(16 units, its shadow, a slider's knob), lies wholly outside the innermost clip
draws nothing; when it clips its children they are skipped too, otherwise each
child tests itself. A video image is always drawn (drawing drives it). An element
with no scale, rotation or translation records no matrix of its own.

Hit testing respects the same intersection: a pointer outside an ancestor's clip
rect cannot hit a child.

---

## 8. Themes and styles

```cpp
struct UIStyleState               // what an interaction state overrides
{
    bool  HasBackground = false;  Color Background {};
    bool  HasBorder     = false;  Color Border {};
    bool  HasText       = false;  Color Text {};
    float ScaleMul = 1.0f;        // visual scale response, 1.0 = none
};

struct UIStyle
{
    Color BackgroundColor { 40, 42, 48, 255 };
    Color TextColor       { 235, 235, 235, 255 };
    Color BorderColor     { 0, 0, 0, 0 };
    float BorderWidth  = 0.0f;     // square borders straddle the edge (half outside): touching panels share one line
    float CornerRadius = 0.0f;          // logical units, 0 = square

    Color   ShadowColor  { 0, 0, 0, 0 };
    Vector2 ShadowOffset { 0.0f, 2.0f };
    float   ShadowSize   = 0.0f;

    float PaddingLeft = 0, PaddingRight = 0, PaddingTop = 0, PaddingBottom = 0;
    float FontSize = 18.0f;             // logical

    bool       UseNPatch = false;       // §10
    Texture2D  NPatchTexture {};
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
    Font  DefaultFont {};
    bool  HasDefaultFont = false;

    std::unordered_map<std::string, float> TypeScale;   // named sizes at text scale 1

    UIStyle&       Get(const std::string& styleClass);        // creates on demand
    const UIStyle& Resolve(const std::string& styleClass) const;  // falls back
    UIStyle&       Extend(const std::string& styleClass, const std::string& base);  // a copy of base
    void           ApplyTypeScale(float textScale);
};
```

`UIStyle::FontRole` (`font_role`, "" = none) names a `TypeScale` entry. The canvas sets every such
style's `FontSize` to `TypeScale[role] * canvas.text_scale` when the theme is set and whenever
`canvas.text_scale` changes, so a "text size" setting is one assignment, not a theme rebuild. A role
missing from `TypeScale` leaves `FontSize` as it is. Assign `theme.type_scale` as a whole dict (item
assignment edits a Python copy). game_kit's scale: tiny 14, small 16, label 18, body 19, heading 22,
title 28, display 34.

`theme.extend(name, base)` makes `name` a copy of `base` (all fields and states) and returns it, so a
variant only sets what differs: `theme.extend("chip_on", "chip").background_color = ACCENT`. It is a
copy made once, not a link: later edits to `base` do not reach `name`, and resolution stays shallow
(below). An unknown `base` raises `ValueError`.

Style classes used by default when `StyleClass` is empty:

| Type | class |
|---|---|
| Panel | `panel` |
| Text | `text` |
| Image | `image` |
| Button | `button` |
| ProgressBar | `progress` (fill uses `progress_fill`) |
| Slider | `slider` (fill `slider_fill`, knob `slider_knob`) |
| ScrollView | `scroll_view` |
| TextInput | `text_input` |

Both built-in themes must also define `card`, `title` and `button_primary`,
because the plan's examples use them.

Resolution order is deliberately shallow: **element override → theme style →
built-in default**. No selectors, no cascade, no live inheritance chains (`extend` copies).

The canvas **owns a copy** of its theme (`set_theme` copies), so a Python
temporary cannot dangle.

### Built-in themes

`engine.ui.modern_theme()`, rounded corners (radius ~8, `card` ~14), soft
shadows, low-contrast borders, generous spacing, `TransitionDuration` 0.08–0.15,
`Pressed.ScaleMul` ~0.97, dark neutral surfaces with one accent for
`button_primary`.

`engine.ui.classic_theme()`, square corners, `Beveled = true` with light
top/left and dark bottom/right, `BevelSunken` for pressed and for progress/scroll
troughs, a blue accent for titles, compact padding, `TransitionDuration = 0`.

`engine.ui.classic_dark_theme()`, pixel-identical layout to `classic_theme`
(same 14 style classes, paddings, bevel widths, corner radii, font sizes) with a
dark charcoal palette, high-contrast text, muted bevels, and boosted shadows.

All three are generated from colours and rectangles. **No copyrighted assets, no
bundled textures, no imitation of a specific product's artwork.**

---

## 9. Widgets

Every widget draws: shadow → background (N-patch, or rounded/plain rect, or
bevel) → border → its own content. All colours pass through `WithOpacity`.

**UIPanel**, container/background/border/shadow/layout parent. Plus
`BlocksInputBehind` (§11).

**UIText**, `Text`, `LocKey` (empty = use `Text` normally; set = resolve from active language),
`LocArgs` (string dictionary of placeholder arguments; assign whole dict), optional `Font` override,
`FontSize` (0 = take from style), `Color` (transparent = take from style), `HAlign`, `VAlign`, and the
`UITextFit` fields of §9b (`Wrap`, `AutoShrink`, `MinFontSize`).
`MeasureTextEx` + `DrawTextEx` (funneled through `DrawTextFitted`). Wrapping is greedy at the
resolved width, with Han/kana boundaries and common CJK punctuation restrictions. Oversized
words break at UTF-8 codepoints; Korean normally keeps space-separated words. No rich text.

**UIImage**, `Texture`, `Tint`, `SourceRect` (zero size = whole texture; a negative height
flips it vertically, as a render texture needs),
`PreserveAspect`, `IntegerScale` (pixel art: the largest whole multiple of the source
size that fits, centred, so every texel stays the same size; a rect smaller than the
source falls back to `PreserveAspect`). `DrawTexturePro`. A zero-`id` texture draws
nothing rather than crashing.

**UIButton**, `Text`, `LocKey`, `LocArgs` (matching `UIText`), `Icon` (procedural icon name, `""` = none),
`IconTexture` (optional, wins over `Icon`), and transient `Hovered`, `Pressed`, `Clicked`.
`Clicked` means *clicked this frame*; the canvas clears it at the start of each
update, before hit testing, so a Python `Update` that runs after the native
update sees exactly one frame of `True`. Icon and text lay out side by side when
both are present, icon-only centres the icon.

**UIProgressBar**, `Value` clamped 0..1, `FillDirection`
(`LeftToRight`, `RightToLeft`, `BottomToTop`, `TopToBottom`). Background uses
`progress`, fill uses `progress_fill`.

**UISlider** *(addition)*, `Value`, `MinValue = 0`, `MaxValue = 1`, optional
`Step` (0 = continuous), `Vertical`. Click-to-jump plus drag with pointer
capture; `Changed` is a transient "changed this frame" flag cleared like
`Clicked`. Draws trough (`slider`), fill (`slider_fill`) and knob
(`slider_knob`). Focusable: left/right (or down/up) adjusts by `Step`, or 5% when
`Step` is 0.

**UIScrollView**, always clips. `ScrollOffset` (logical y), `ContentHeight`
computed from the union of its children's resolved rects. Mouse wheel scrolls
while hovered; offset clamps to `[0, max(0, content - viewport)]`. Children are
laid out with the offset subtracted from their y. A thin non-interactive
indicator bar may be drawn when content overflows, no draggable scrollbar
widget, no horizontal scrolling.

**UITextInput** *(addition)*, `Text`, `Placeholder` (shown dimmed via
`WithOpacity(textColor, 0.5f)` when `Text` is empty), filter flags `AllowLetters`
(default `true`), `AllowDigits` (default `true`), `AllowSymbols` (default `true`,
covers spaces and punctuation). Rejected characters are silently dropped.
Single-line append-at-end Unicode text field with backspace and submit. Focusable:
participates in click-to-focus, Tab/Shift-Tab, and directional gamepad/keyboard
focus navigation. Loops `GetCharPressed()` for all codepoints pressed during the
frame; each accepted codepoint is encoded as UTF-8 into `Text` (`include/IYI/Utf8Text.h`).
Classification: ASCII letters, Latin-1/Latin Extended, Greek, Cyrillic, Han, kana,
Hangul syllables and Jamo are
"letters"; `0`-`9` are "digits"; every other printable codepoint is a "symbol"; control
characters, surrogates and values above U+10FFFF are always rejected. Turkish, German
and CJK input is therefore accepted under `allow_letters`. **Paste:** Ctrl+V (Cmd+V on
macOS) inserts the clipboard text (`engine.get_clipboard_text()`) at the end, filtered by
`FilterPaste` in `Utf8Text.h` with exactly the same classification and `allow_*` flags as
typed input; newlines, tabs and other controls and invalid UTF-8 are dropped. There is no
maximum length for typed input, so none for paste. `IsKeyPressed(KEY_BACKSPACE)`
(and `IsKeyPressedRepeat`, so a held key repeats at the OS keyboard repeat delay/rate)
removes exactly one codepoint (never half a multibyte sequence). `text` is UTF-8; any
length limit a game applies must count codepoints (`len(text)` in Python does). The
widget has no native max-length. No shaping, bidi or grapheme clusters.
On Windows, the focused visible field anchors the native IME composition and candidate
windows at its transformed caret. Windows renders preedit; committed characters still
come only from raylib. Composition and its commit/cancel frame suppress keyboard focus
navigation, paste, backspace, submit and Python keyboard queries. Focus changes, removal
and window focus loss cancel composition. See `IME.md` for the provider verification limits.
`IsKeyPressed(KEY_ENTER)` sets transient `Submitted` flag for one frame
(cleared in `ClearTransientFlags`). A simple blinking vertical bar text cursor
flips every ~0.5s and is drawn after the text only while focused. Text and
placeholder rendering strictly funnels through `DrawTextFitted` (Hard Rule 15).

Not now: multi-line text fields, rich text, selection/caret navigation, dropdowns, tree views, data grids, docking, tabs.

---

## 9b. Text must never leave its box

A UI that lets text spill outside its rectangle is broken, and leaving it to each
game script to remember is how it keeps happening. The engine guarantees it
instead.

**Every text drawing path in IYIUI goes through one internal helper.** There are
currently three independent `DrawTextEx` call sites; they collapse into:

```cpp
struct UITextFit
{
    bool  Wrap        = true;    // word wrap inside the box width
    bool  AutoShrink  = true;    // reduce font size until it fits
    float MinFontSize = 8.0f;    // logical units; the shrink floor
};

// Lays out and draws `text` so that it is ALWAYS inside `box`.
void DrawTextFitted(const Font& font, const std::string& text, Rectangle box,
                    float requestedFontSize, float spacing, Color color,
                    UIHAlign hAlign, UIVAlign vAlign, const UITextFit& fit);
```

`box` is the element's `ResolvedRect` inset by the resolved style's padding.
No widget may call `DrawTextEx` / `DrawTextPro` directly any more.

### The algorithm, in order

1. Empty text → draw nothing.
2. Lay the text out at `requestedFontSize`: split into lines on `
`, and, when
   `Wrap`, greedily wrap each to `box.width` using word/CJK boundaries. Oversized
   words break at complete UTF-8 codepoints.
3. Measure: width = widest line, height = line count × line height.
4. Fits (`width <= box.width && height <= box.height`) → draw and stop.
5. `AutoShrink` → **binary search** the font size in
   `[MinFontSize, requestedFontSize]` for the largest size that fits, at most
   **10 iterations**, re-running step 2 each time. Never decrement by one: a
   40 pt title would cost 40 measure passes per frame.
6. Still not fitting at `MinFontSize` → **ellipsize** with `…` (U+2026):
   - wrapped: keep the whole lines that fit vertically; truncate the last visible
     line character by character until line + `…` fits the width, then append it.
   - single line: truncate the line the same way.
   - If even `…` alone does not fit, draw nothing.
7. Push the box onto the scissor stack for the duration of the draw, as a final
   hard guarantee. Rounding must not be able to put a pixel outside. Text the fit
   measured inside its box (not ellipsized, measured width and height within the
   box) skips the scissor: every scissor change flushes the draw batch.

Steps 5–7 mean overflow is not merely discouraged, it is unreachable.

### Per-element memo

Text measurement is the expensive part of UI layout and this runs every frame.
Each text-bearing element caches its fitted result, resolved font size, the line
list, and the ellipsized flag, and recomputes only when one of the inputs
changes: the string, the box size, the requested font size, the font glyph buffer/texture identity, or
any `UITextFit` field. It also keeps the line widths (until the next refit) and the
`FontForText` answers for its text (while `AssetManager::FontGeneration()` is unchanged).

This is a local memo on one element, separate from the canvas layout cache below;
neither memo tracks dependencies between elements.

### Defaults, and why they differ per widget

| Widget | `wrap` | `auto_shrink` | `min_font_size` |
|---|---|---|---|
| `UIText` | `true` | `true` | `8.0` |
| `UIButton` label | `false` | `true` | `8.0` |

`UIText` defaults to wrapping because a description or paragraph should reflow.
A button label should not become two lines when the button gets narrow, it
should shrink, so `UIButton` defaults `wrap` to `false`. Any other widget that
draws text uses the `UIText` defaults.

**`UIText.Wrap` changes from `false` to `true`.** That is a deliberate default
change: the safe behaviour has to be the one you get without asking.

### Python surface

```text
text.wrap            -> bool   (default True)
text.auto_shrink     -> bool   (default True)
text.min_font_size   -> float  (default 8.0, logical units)

button.wrap          -> bool   (default False)
button.auto_shrink   -> bool   (default True)
button.min_font_size -> float  (default 8.0)
```

Three read-only companions on both, for checking what the fitter decided:

```text
.fitted_font_size  -> float   the size actually used after shrinking
.measured_width    -> float   width of the laid-out text, logical units
.measured_height   -> float   height of the laid-out text, logical units
```

They are diagnostics, not a second way to set anything: they made the overflow
guarantee testable from Python, and they answer "why is my label tiny" without a
debugger. `measured_width <= resolved_rect.width` and
`measured_height <= resolved_rect.height` must hold for every text element in
every frame, that is the invariant this whole section exists to enforce.

Four combinations, from two bools, cover every case: wrap only, shrink only,
both, or neither, and "neither" still cannot overflow, because step 6 and step 7
always apply.

---

## 9c. Localization integration: `loc_key` and `loc_args`

`UIText` and `UIButton` support localized string binding through two properties: `loc_key` and `loc_args`.

### Dynamic resolution rule
- **`loc_key` empty (`""`)**: Default. Keeps existing `.text` behaviour untouched. The widget renders whatever string is in its `.text` property.
- **`loc_key` set (non-empty)**: The widget resolves `LocalizationManager::Get().Text(LocKey, args)` when its content is laid out or drawn (`ResolvedText()`), and keeps the result until the key, the args or `LocalizationManager::Revision()` (language, fallback, reload, hot-reloaded file) change.
- **Instant language updates**: Because resolution is dynamic on layout/drawing, calling `engine.loc.set_language("code")` immediately updates all existing widgets across the canvas on the very next frame with **no canvas rebuild or scene restart required**.

### Layout and text-fitting guarantee
Localized text funnels unconditionally through the exact same `FitText` and `DrawTextFitted` pipeline defined in §9b:
- Greedy word-wrapping, `auto_shrink` font binary search, and ellipsis (`…`) apply identically to localized strings.
- Translations with longer sentences or compound words (e.g. German, French) refit safely within their box without overflowing or corrupting UI layout.

### Two critical constraints
1. **Lowercase naming**: Language codes and locale filenames are always strictly lowercase (`assets/locales/<code>.json`), because `ResourceStore` normalizes all virtual paths to lowercase (e.g. `zh-cn.json`, `pt-br.json`).
2. **Copy-by-value across Python boundary**: `loc_args` is copied by value across the pybind11 boundary. You **must assign a whole dictionary** to update arguments:
   ```python
   # Correct:
   label.loc_args = {"player": "Alex", "score": "100"}

   # WRONG (mutating in place modifies a temporary copy and is SILENTLY LOST):
   label.loc_args["score"] = "200"
   ```

### Font and glyph scope
Text rendering in IYIUI is UTF-8 native. Font atlas glyph coverage includes:
- Basic Latin (`0x0020`–`0x007E`)
- Latin-1 Supplement (`0x00A0`–`0x00FF`)
- Latin Extended-A (`0x0100`–`0x017F`)
- Cyrillic (`0x0400`–`0x04FF`)

`engine.load_font(..., glyph_text="...")` adds a sparse UTF-8 repertoire. Suitable
project fonts can render CJK, including regional Han forms, kana and Hangul. UI
measurement and drawing resolve the same cumulative private atlas view for text
outside that repertoire; expansion flushes queued draws before replacing the view.
Text drawn more than 1.5x its atlas size on screen (an element `scale` such as a zoomed
view, or a high-DPI root scale) draws from a 2x or 4x atlas of the same glyphs, made once
per size on first use (a short hitch) and remade when the repertoire grows; lines are
still measured with the base atlas. Since 2026-11-03.
UTF-8 ellipsis never leaves partial byte sequences. See spec/localization.md §19.10 for scope.

Scripts requiring complex shaping, cursive attachment, or bidirectional / right-to-left (RTL) layout (such as Arabic, Hebrew, Persian, Devanagari, Thai) are **not supported**. No ICU or HarfBuzz libraries are linked.

---

## 10. N-patch

When a style sets `UseNPatch` with a valid texture, draw the background with
raylib's own `NPatchInfo` + `DrawTextureNPatch` (`NPATCH_NINE_PATCH`), using the
four border widths. This replaces the rect/rounded/bevel background for that
element; border, shadow and content still draw normally. Do not hand-roll
nine-slice geometry. Procedural styles stay the default so IYIUI needs no assets.

---

## 11. Input, hit testing and focus

All inside `UICanvas::Update(dt)`, using raylib input directly, and **before** the
Python `Update` hook runs (native lifecycle already runs first, see `spec/runtime.md` §6).

Order every frame:

1. Clear transient flags (`Clicked`, `Changed`) on every element.
2. If an active postprocess radial warp is configured, map the raw mouse position
   through the matching output-to-source radial lookup. Then convert it to logical
   space (`mapped_mouse_position / scale_factor`).
3. Resolve layout, opacity and transforms for the whole tree (§3–§6).
4. Hit test: walk the tree **back to front in reverse draw order** (last sibling
   first, depth first) and take the first element that is `Visible`, `Enabled`,
   interactive, has `ResolvedOpacity > 0.01`, contains the point after
   `ApplyInverse`, and is inside every ancestor clip rect. Only that element gets
   the pointer. A visible, enabled panel with `BlocksInputBehind` stops the walk
   at its own rect even though it is not itself interactive.
5. Pointer capture: while a button or slider is held, it keeps the pointer until
   release regardless of where the mouse moves. Release inside the element's rect
   produces `Clicked`; release outside does not.
   A button's `hit_scale` (`Vector2`, default (1, 1)) grows its pointer rect about its centre for
   steps 4-5 (x 1.25 = 25% wider), for isolated buttons on touch screens; drawing and layout are
   unchanged. Keep the growth inside the free space: an overlap goes to the later sibling.
6. Mouse wheel goes to the innermost hovered `UIScrollView`.
7. Focus navigation: arrow keys, or D-pad / left stick past a deadzone, move focus
   to the nearest focusable element whose centre lies in that direction (compare
   centres; prefer the smallest distance, ignore candidates behind the current
   one). Enter, Space or gamepad-A activates the focused element exactly as a
   mouse release would. Tab and Shift+Tab step through focusables in tree order.
   Mouse interaction takes focus. Focus is drawn as a border highlight derived
   from the style, not a separate widget.
8. Advance state transitions (§12) and, in Phase 2, tweens.

`engine.ui.pointer_over_ui()` asks the same question for game code: true when a live, visible
canvas of the scene has the mouse where it is now, the walk of step 4 finds a widget or a
`blocks_input_behind` panel (or a scroll view) under it, or a press is captured. A world actor
checks it before turning a click, drag or wheel into a world action. It re-runs the walk at the
current mouse position over the last resolved layout (`UICanvas::PointerOverUI`, sharing
`PickElement` with step 4), so it is right even when the asking actor updates before the
canvas does, and it can never disagree with what the UI itself reacts to.

No event bus, no callbacks, no bubbling. Python polls transient flags.

---

## 12. State transitions

Each interactive element holds current animated colours plus a visual scale
multiplier. Each frame the target is the style state for its interaction state
(disabled → pressed → hover → normal, first match wins), and the current values
move toward it over `TransitionDuration` using **real frame `dt`**, not the fixed
step. `TransitionDuration == 0` snaps, which is what the classic theme uses.

---

## 13. `UICanvas`

```cpp
class UICanvas : public GameActor
{
public:
    UICanvas();
    ~UICanvas() override;

    void Update(float dt) override;   // input, focus, layout, transitions, tweens
    void Drawing() override;          // draws the tree

    // canvas scaler
    float ReferenceWidth = 1920.0f, ReferenceHeight = 1080.0f;
    float Match = 0.5f;
    float ScaleFactor()   const;
    float LogicalWidth()  const;
    float LogicalHeight() const;

    UIElement* Root();
    UIElement* Add(UIElementType type, const std::string& name, UIElement* parent);
    UIElement* Find(const std::string& name);
    UIElement* FindById(uint64_t id);
    void ClearUI();                   // Phase 2, removes all elements, keeps state
    void SetTheme(const UITheme& theme);
    UITheme& Theme();

    bool DebugDraw = false;           // addition

    // tweens (Phase 2)
    uint64_t StartTween(...);
    void KillTweensFor(UIElement* element, bool includeDescendants);
    uint64_t CreateSequence();
};
```

`ScreenSpace` is forced `true` in the constructor and must not be settable to
`false` from Python. The canvas is spawned through the existing
`ActorRegistry::Spawn(std::unique_ptr<GameActor>, ...)`, so scene changes and
`Persistent` behave exactly as for any actor with no registry changes.

`UICanvas::Drawing()` runs inside the engine's existing HUD pass, outside
`BeginMode2D`, so the camera never moves the UI.

**Frame order.** `Update()` lays the tree out (for hit testing) before the canvas's Python
`Update` hook runs, and `Drawing()` lays it out again before drawing. So everything the hook
changes draws this frame: position, size, anchors, `pivot`, `opacity` and a new element's rect as
well as `visible`, `scale`, text and colours. Before 2026-11-03 `Drawing()` did not re-layout, and
the layout-read half showed a frame late (a zoom setting `scale` and `anchored_position` together
jumped; `opacity = 0` with `visible = True` flashed at the old opacity). Hit testing still uses the
layout from before the hook: a click lands on where the element was drawn last frame. The second
pass is cheap when nothing changed: it restores from the layout cache (§20).

`DebugDraw`: after the normal pass, outline every visible element's resolved rect
(1 logical unit, a distinct colour per depth) and draw its name at a small font
size. Skipped entirely when false.

---

## 14. Tween system *(Phase 2)*

Tweens belong to the canvas and update in `UICanvas::Update` with real frame time.
**Clamp the tween `dt` to at most 0.1 s** so one long frame after a window stall
or focus loss does not skip every animation to its end.

Properties: anchored position, size delta, scale, rotation, opacity, background
colour, text colour.

```cpp
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
```

Easing functions are small pure functions in `IYIUITween.cpp`. Python passes the
name as a string (`ease="out_cubic"`); an unknown name falls back to `Linear` and
prints one warning.

A tween stores: id, target element id, property, from/to (up to 4 floats),
duration, elapsed, delay, ease, `Loops` (0 = once, negative = forever), `Yoyo`,
`Paused`, `Completed`, `Dead`.

Rules:

- Always "from the property's current value → target".
- Starting a tween on the same (element, property) **kills the previous one**, so
  competing tweens cannot fight.
- Handles are `{canvas, tween id}`, never a pointer, the tween vector reallocates
  and elements get removed.
- Handle surface: `active`, `completed`, `progress`, `delay`, `yoyo`, `loops`,
  `kill()`, `pause()`, `resume()`.
- **No Python callbacks stored natively**, a stale function reference across hot
  reload is exactly the bug this avoids. Poll `completed` instead.
- Removing an element, `clear_children()` and `clear_ui()` kill affected tweens.

Element helpers, all returning a handle:

```python
element.move_to(x, y, duration, ease="linear")
element.translate_to(x, y, duration, ease="linear")   # translation: a slide that lays nothing out again
element.size_to(w, h, duration, ease="linear")
element.scale_to(sx, sy, duration, ease="linear")
element.rotate_to(degrees, duration, ease="linear")
element.fade_to(opacity, duration, ease="linear")
element.color_to(color, duration, ease="linear")        # background
element.text_color_to(color, duration, ease="linear")
```

### Sequences

```python
seq = canvas.sequence()
seq.append_move(panel, 100, 80, 0.35, "out_cubic")
seq.join_fade(panel, 1.0, 0.20)
seq.append_scale(panel, 1.05, 1.05, 0.15, "out_back")
seq.append_interval(0.10)
seq.append_scale(panel, 1.0, 1.0, 0.12, "out_cubic")
seq.play()
```

`append_*` starts after everything queued so far finishes; `join_*` starts at the
same time as the previous step; `append_interval(s)` adds dead time. `play()`,
`kill()`, and `active` are the whole control surface. Steps are stored with
computed start offsets and their tweens are created when the offset is reached.
Nothing more of DOTween is reproduced.

---

## 15. `Reload(actor)` lifecycle hook *(Phase 2)*

A new optional actor hook, for the case the engine could not previously handle:
editing UI **construction** code inside `Start()`.

```python
def Reload(actor):
    ...
```

- Called after that actor's own script file reloaded **successfully**, or on
  broadcast when an unowned helper module reloads.
- Never after a failed reload.
- Does not call `Start()` again, does not recreate the actor, does not touch
  `actor.state`.
- Missing hook is a silent no-op.

Implementation, deliberately small:

1. `ActorHook` gains `Reload`. It takes `(actor)` only, no `dt`.
2. `PythonRuntime::PollHotReload` records each successfully reloaded script's
   project-relative path via `NoteReloaded`.
3. `PythonRuntime` exposes `std::vector<std::string> TakeReloadedScripts()`,
   which returns and clears that list.
4. `Application`, right after `PollHotReload`, passes the list to the current
   scene's registry: `ActorRegistry::DispatchReload(const std::vector<std::string>&)`.
5. `DispatchReload` calls the hook once for each live actor whose `Script` matches
   a path in the list, same index-snapshot iteration discipline as every other
   pass, skipping `PendingDestroy`. If a reloaded path is unowned by any actor
   (a helper module or entry script), it broadcasts `Reload` to every live actor
   (at most once per actor), because the engine does not track which actor imported
   which module (see `docs/spec/runtime.md` §11).
   - Accepted consequence: a helper reload rebuilds retained UI, so transient UI
     state such as focus and scroll position resets. This is a deliberate trade-off,
     not a bug.

`canvas.clear_ui()` removes all UI elements, kills all tweens and sequences, and
resets focus/hover/capture, but does **not** destroy the canvas actor or clear
`canvas.state`. Together they give the intended workflow:

```python
def Start(canvas):
    canvas.state["theme_classic"] = False
    build_ui(canvas)

def Reload(canvas):
    canvas.clear_ui()
    build_ui(canvas)
```

---

## 16. `window_resizable` project setting

`ProjectConfig` gains one optional boolean, default `false`:

```json
"window_resizable": true
```

When true, `SetConfigFlags(FLAG_WINDOW_RESIZABLE)` **before** `InitWindow`. Add
the field to `spec/project.md` §2's field list and its default. Nothing else changes; no
window-settings framework.

---

## 17. Python API

`engine.ui` submodule:

```text
engine.ui.spawn_canvas(name="Canvas", script="", reference_width=1920,
                       reference_height=1080, match=0.5, persistent=False) -> UICanvas
engine.ui.modern_theme()       -> UITheme
engine.ui.classic_theme()      -> UITheme
engine.ui.classic_dark_theme() -> UITheme
engine.ui.theme()              -> UITheme      # empty theme to fill in from Python
engine.ui.icon_names()         -> list[str]
engine.ui.pointer_over_ui()    -> bool         # any live canvas would take the mouse here (§11)
```

Built-in themes provide ready-made visual styles: `engine.ui.modern_theme()` for
rounded aesthetics, `engine.ui.classic_theme()` for retro beveled surfaces, and
`engine.ui.classic_dark_theme()` for a night mode with identical layout, charcoal
surfaces, high-contrast text, and softened bevel highlights.

`UICanvas` (subclass of `GameActor`, so it also has `id`, `name`, `state`,
`world_pos`, `active`, `visible`, `persistent`, `destroy()`):

```text
canvas.root                                     -> UIElement (read-only)
canvas.add_panel(name="Panel", parent=None)     -> UIPanel
canvas.add_text(name="Text", parent=None)       -> UIText
canvas.add_image(name="Image", parent=None)     -> UIImage
canvas.add_button(name="Button", parent=None)   -> UIButton
canvas.add_progress_bar(name="ProgressBar", parent=None) -> UIProgressBar
canvas.add_slider(name="Slider", parent=None)   -> UISlider
canvas.add_scroll_view(name="ScrollView", parent=None) -> UIScrollView
canvas.add_text_input(name="TextInput", parent=None, placeholder="", allow_letters=True, allow_digits=True, allow_symbols=True) -> UITextInput

canvas.find(name) / canvas.find_by_id(id)       -> element or None
canvas.clear_ui()
canvas.set_theme(theme) / canvas.theme          -> UITheme (the canvas's own copy)
canvas.sequence()                               -> UISequence

canvas.reference_size    -> Vector2 (read/write)
canvas.match             -> float   (read/write, clamped 0..1)
canvas.scale_factor      -> float   (read-only)
canvas.logical_width     -> float   (read-only)
canvas.logical_height    -> float   (read-only)
canvas.debug_draw        -> bool    (read/write) outlines every rect; problems() in red with their kind
canvas.text_scale        -> float   (read/write) multiplies the type scale (§8)
canvas.problems()        -> [{kind, element, detail}]  the UI rules on the shown elements, below
canvas.focused           -> element or None (read/write)
canvas.hovered           -> element or None (read-only; the one widget under the pointer, kept while pressed)
```

`UIElement` (base of every widget):

```text
element.id / element.name / element.type_name      (read-only)
element.parent                                     -> element or None (read-only)
element.children                                   -> list (read-only snapshot)

element.visible / element.enabled / element.opacity / element.focusable
element.style_class / element.clip_children

element.anchor_min / element.anchor_max / element.pivot
element.anchored_position / element.size_delta
element.min_size / element.max_size
element.scale / element.rotation / element.translation   # visual only: changing them lays nothing out again

element.layout_mode      -> "none" | "horizontal" | "vertical"
element.padding                -> writes all four
element.padding_left / _right / _top / _bottom
element.spacing
element.child_alignment  -> "start" | "center" | "end"
element.expand_children_width / element.expand_children_height
element.stretch_children_cross_axis
element.flex_weight
element.ignore_layout
element.wrap_children    -> bool  horizontal: flow onto new rows (§5)
element.fit_height       -> bool  height follows the content (§5)
element.font_group       -> str   texts of one group draw at one size (§4)

element.resolved_rect    -> Rectangle (read-only)
element.resolved_opacity -> float     (read-only)

element.remove() / element.clear_children()
element.bring_to_front() / element.send_to_back()
element.style            -> UIStyle, this element's own override (created on access)

# tween helpers (Phase 2), see §14
```

Widget-specific: as named in §9, snake_case (`button.text`, `button.loc_key`,
`button.loc_args`, `button.icon`, `button.hovered`, `button.pressed`,
`button.clicked`, `text.text`, `text.loc_key`, `text.loc_args`, `text.wrap`,
`text.horizontal_alignment`, `image.preserve_aspect`, `image.integer_scale`, `image.source_rect`,
`image.video` (an `engine.video.Video` drawn instead of `texture`, SPEC §23),
`progress.value`, `progress.fill_direction`, `slider.value`, `slider.min_value`,
`slider.max_value`, `slider.step`, `slider.changed`, `scroll_view.scroll_offset`,
`scroll_view.content_height`, `panel.blocks_input_behind`, `text_input.text`,
`text_input.placeholder`, `text_input.allow_letters`, `text_input.allow_digits`,
`text_input.allow_symbols`, `text_input.submitted`, `text_input.hovered`,
`text_input.pressed`). Note that `loc_args` copies by value: assign a whole dict.

`UITheme`: `theme.get(style_class) -> UIStyle`, `theme.default_font` (write),
`theme.style_classes -> list[str]`.

`UIStyle`: writable properties mirroring §8 in snake_case, with
`style.hover`, `style.pressed`, `style.disabled` returning writable
`UIStyleState` (`background_color`, `text_color`, `border_color`, `scale_mul`).

Lifecycle hooks stay PascalCase (`Start`, `Update`, `FixedUpdate30`,
`FixedUpdate60`, `Drawing`, `Reload`); everything else is snake_case. **One way to
do each thing**, do not add a second spelling of any of the above.

---

### UI problems

`canvas.problems()` (C++ `UICanvas::Problems`) lays the tree out and checks every shown element:
`ZERO_SIZE` (not an empty panel: a row with nothing shown draws nothing), `OFFSCREEN` (outside the canvas, unless clipped), `OVERFLOW` (past its parent, unless
under a scroll view or `clip_children`), `TEXT_CLIP`, `FONT_SHRUNK` (fitted below the requested size,
as snapped for a pixel font), `UNSUPPORTED_GLYPH`, `TEXT_OVERLAP` (two texts of one parent whose drawn glyphs cross, by their alignment
and measured size: a long label running into its value), `BUTTON_OVERLAP` (enabled buttons sharing clickable
pixels, cut by every clipping ancestor; a button under a later `blocks_input_behind` panel such as a
modal backdrop is not clickable and not counted). `debug_draw` draws them in red with their kind; the
agent request `"ui_problems": true` returns them for every canvas; the visual tests and the UI
playtester read that list.

## 18. Demo *(removed)*

The `ui_demo.py` demo in IYIGame no longer exists; the games under `IYIProjects/` exercise IYIUI.

---

## 19. Procedural icons

`IYIUIIcons.cpp` draws icons from raylib primitives, lines, circles, rectangles,
triangles, into a given rectangle with a given colour, scaling cleanly. No icon
font, no image assets.

```text
blank play pause stop close check checkbox checkbox_checked plus minus
arrow_left arrow_right arrow_up arrow_down
chevron_left chevron_right chevron_up chevron_down
home menu search settings volume mute info warning
```

One entry point: `void DrawIcon(const std::string& name, Rectangle area, Color color);`
An unknown name draws nothing and warns once. `button.icon = "play"` uses it;
`button.icon_texture` covers anything procedural cannot express.

---

## 20. Layout cache

Each canvas keeps up to eight recent resolved layout variants. The exact
signature is rebuilt on every layout pass (twice a frame, §13 "Frame order") from the
logical viewport, element tree/order, visibility, layout rect fields, container
settings, and scroll offset. A hidden element's subtree is left out: nothing in it is
drawn or hit, and showing the element changes its own visibility in the signature, so
the next pass re-solves. Its rects may therefore be stale while it stays hidden; read
`resolved_rect` of visible elements only. An exact hit restores the cached rectangles
and scroll content heights (nothing at all when it is the newest entry, whose rects are
already in the tree); a miss resolves the tree and stores the result, evicting the least
recently used entry when full. Scale, rotation, translation, transitions, opacity, and other visual
state still resolve every pass, so move a panned or animated element with `translation`, not
`anchored_position`: the latter re-solves the whole canvas on every frame it changes.

A `FitHeight` element adds its text and font size to the signature, and setting the theme or the
text scale bumps a generation in it, so a changed text or size re-solves the fit.

Measured 2026-11-03 in HR (one screen of several shown): with hidden subtrees in the
signature it took 0.2-0.36 ms of a 0.3-0.6 ms pass; without them 0.03-0.05 ms of a
0.12-0.19 ms pass, so both passes together cost less than the one pass did. The profile
sections `layout/signature`, `layout/solve` (a miss) and `layout/transforms` show it;
transforms and opacity also skip hidden subtrees.

## 21. Non-goals

Everything in `spec/build.md` §16 and `ROADMAP.md`, plus: raygui, Dear ImGui, RmlUi,
Yoga/Flexbox, external tween or icon libraries, ECS UI, editor, visual UI
designer, CSS parser, HTML, docking, multi-line text fields, tree views, data grids, a
desktop window manager, stencil masks, shader-heavy UI rendering, dirty-layout
dependency tracking, event buses, and Python callbacks stored in native objects.

## 22. Validation scope

Same as `spec/build.md` §15. A small native regression suite (`tests/engine_tests.cpp`,
`ctest`) covers pure logic and UI lifetime; the remaining gates are: it configures and builds; the
Python bindings initialize; the game scripts pass `py_compile`; a game using IYIUI runs with no traceback (`IYI_RUNTIME_READY`, §3.1 agent channel); and
editing a UI actor script triggers `Reload` and rebuilds the UI without restarting.
