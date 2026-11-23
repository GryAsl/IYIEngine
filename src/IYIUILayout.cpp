// Resolves IYIUI layout: the per-frame layout-input signature and its small variant cache, element rects,
// content sizes, row/column/grid containers, and the transform/opacity pass that still runs every frame.
// Moved verbatim out of IYIUI.cpp; the signature must cover every input these functions read (hard rule 19).

#include "IYI/IYIUI/IYIUI.h"
#include "IYI/Profiler.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <vector>

namespace IYI::UI {

// ponytail: Keep eight recent layout variants; rare variants safely recompute on a miss.
static constexpr std::size_t kLayoutCacheLimit = 8;
static int s_resolves = 0;   // ResolveElementLayout calls in this solve, reported as the profile count `resolves`

static void AppendLayoutFloat(std::vector<std::uint64_t>& signature, float value)
{
    signature.push_back(std::bit_cast<std::uint32_t>(value));
}

static void AppendLayoutVector(std::vector<std::uint64_t>& signature, Vector2 value)
{
    AppendLayoutFloat(signature, value.x);
    AppendLayoutFloat(signature, value.y);
}

static void AppendLayoutSignature(UIElement* element, std::vector<std::uint64_t>& signature)
{
    signature.push_back(element->Id);
    signature.push_back(static_cast<std::uint64_t>(element->Type));
    signature.push_back(element->Visible ? 1 : 0);
    signature.push_back(element->IgnoreLayout ? 1 : 0);

    const RectTransform& rect = element->Rect;
    AppendLayoutVector(signature, rect.AnchorMin);
    AppendLayoutVector(signature, rect.AnchorMax);
    AppendLayoutVector(signature, rect.Pivot);
    AppendLayoutVector(signature, rect.AnchoredPosition);
    AppendLayoutVector(signature, rect.SizeDelta);
    AppendLayoutVector(signature, rect.MinSize);
    AppendLayoutVector(signature, rect.MaxSize);
    signature.push_back(static_cast<std::uint64_t>(element->Layout));
    AppendLayoutFloat(signature, element->FlexWeight);
    AppendLayoutFloat(signature, element->PaddingLeft);
    AppendLayoutFloat(signature, element->PaddingRight);
    AppendLayoutFloat(signature, element->PaddingTop);
    AppendLayoutFloat(signature, element->PaddingBottom);
    AppendLayoutFloat(signature, element->Spacing);
    signature.push_back(static_cast<std::uint64_t>(element->ChildAlignment));
    signature.push_back(element->ExpandChildrenWidth ? 1 : 0);
    signature.push_back(element->ExpandChildrenHeight ? 1 : 0);
    signature.push_back(element->StretchChildrenCrossAxis ? 1 : 0);
    signature.push_back(element->WrapChildren ? 1 : 0);
    signature.push_back(element->FitHeight ? 1 : 0);
    if (element->FitHeight) signature.push_back(element->TextLayoutKey());

    if (element->Type == UIElementType::ScrollView) {
        const auto* scroll = static_cast<const UIScrollView*>(element);
        AppendLayoutFloat(signature, scroll->ScrollOffset);
    }

    // A hidden element's subtree is left out: nothing in it is drawn or hit, and showing the element
    // changes its own Visible bit above, which lays everything out again. So a hidden subtree's rects
    // may be stale while it stays hidden (a changed element count there just fails the restore and
    // re-solves). HR keeps one screen of several thousand elements visible at a time.
    if (!element->Visible) return;
    signature.push_back(static_cast<std::uint64_t>(element->Children.size()));
    for (const auto& child : element->Children) {
        AppendLayoutSignature(child.get(), signature);
    }
}

static void CaptureLayoutSnapshot(UIElement* element, std::vector<Rectangle>& rects,
                                  std::vector<float>& scrollContentHeights)
{
    rects.push_back(element->ResolvedRect);
    scrollContentHeights.push_back(element->Type == UIElementType::ScrollView
        ? static_cast<UIScrollView*>(element)->ContentHeight : 0.0f);
    for (const auto& child : element->Children) {
        CaptureLayoutSnapshot(child.get(), rects, scrollContentHeights);
    }
}

static bool RestoreLayoutSnapshot(UIElement* element, const std::vector<Rectangle>& rects,
                                  const std::vector<float>& scrollContentHeights, std::size_t& index)
{
    if (index >= rects.size() || index >= scrollContentHeights.size()) return false;
    element->ResolvedRect = rects[index];
    if (element->Type == UIElementType::ScrollView) {
        static_cast<UIScrollView*>(element)->ContentHeight = scrollContentHeights[index];
    }
    ++index;
    for (const auto& child : element->Children) {
        if (!RestoreLayoutSnapshot(child.get(), rects, scrollContentHeights, index)) return false;
    }
    return true;
}

void UICanvas::ResolveLayoutTree()
{
    ProfileBegin("layout");
    float lw = LogicalWidth();
    float lh = LogicalHeight();
    Rectangle rootRect = { 0.0f, 0.0f, lw, lh };
    m_root->ResolvedRect = rootRect;

    // UIElement layout fields are public and scripts assign them directly, so compare the full
    // layout inputs here instead of relying on setters that game code can bypass.
    auto buildSignature = [&]() {
        m_layoutSignature.clear();
        AppendLayoutFloat(m_layoutSignature, lw);
        AppendLayoutFloat(m_layoutSignature, lh);
        m_layoutSignature.push_back(m_themeGeneration);
        AppendLayoutSignature(m_root.get(), m_layoutSignature);
    };

    ProfileBegin("signature");
    buildSignature();
    ProfileEnd();
    // The newest entry holds the rects already in the tree (only layout writes ResolvedRect), so a
    // second pass with nothing changed since (Drawing after Update) has nothing to restore.
    bool restored = !m_layoutCache.empty() && m_layoutCache.back().Signature == m_layoutSignature;
    if (!restored) {
        auto cached = std::find_if(m_layoutCache.rbegin(), m_layoutCache.rend(), [&](const LayoutCacheEntry& entry) {
            return entry.Signature == m_layoutSignature;
        });
        if (cached != m_layoutCache.rend()) {
            LayoutCacheEntry entry = std::move(*cached);
            m_layoutCache.erase(std::next(cached).base());
            std::size_t index = 0;
            restored = RestoreLayoutSnapshot(m_root.get(), entry.ResolvedRects,
                                             entry.ScrollContentHeights, index)
                && index == entry.ResolvedRects.size()
                && index == entry.ScrollContentHeights.size();
            if (restored) m_layoutCache.push_back(std::move(entry));
        }
    }

    if (!restored) {
        ProfileBegin("solve");
        m_root->ResolvedRect = rootRect;
        s_resolves = 0;
        ResolveElementContent(m_root.get());
        ProfileCount("resolves", s_resolves);

        LayoutCacheEntry entry;
        buildSignature(); // Scroll views may clamp their offset or update their content height.
        entry.Signature = m_layoutSignature;
        CaptureLayoutSnapshot(m_root.get(), entry.ResolvedRects, entry.ScrollContentHeights);
        ProfileCount("elements", static_cast<double>(entry.ResolvedRects.size()));
        if (m_layoutCache.size() >= kLayoutCacheLimit) m_layoutCache.erase(m_layoutCache.begin());
        m_layoutCache.push_back(std::move(entry));
        ProfileEnd();
    }

    ProfileBegin("transforms");
    ResolveTransformsAndOpacity(m_root.get(), UITransform2D::Identity(), 1.0f);
    ProfileEnd();
    ProfileEnd();
}

void UICanvas::ResolveElementLayout(UIElement* element, const Rectangle& parentRect, bool content)
{
    if (!element) return;
    ++s_resolves;

    const auto& rt = element->Rect;

    float ax0 = parentRect.x + parentRect.width * rt.AnchorMin.x;
    float ax1 = parentRect.x + parentRect.width * rt.AnchorMax.x;
    float ay0 = parentRect.y + parentRect.height * rt.AnchorMin.y;
    float ay1 = parentRect.y + parentRect.height * rt.AnchorMax.y;

    float w = 0.0f;
    if (rt.AnchorMin.x == rt.AnchorMax.x) {
        w = rt.SizeDelta.x;
    } else {
        w = (ax1 - ax0) + rt.SizeDelta.x;
    }

    float h = 0.0f;
    if (rt.AnchorMin.y == rt.AnchorMax.y) {
        h = rt.SizeDelta.y;
    } else {
        h = (ay1 - ay0) + rt.SizeDelta.y;
    }

    if (rt.MinSize.x > 0.0f && w < rt.MinSize.x) w = rt.MinSize.x;
    if (rt.MaxSize.x > 0.0f && w > rt.MaxSize.x) w = rt.MaxSize.x;
    if (rt.MinSize.y > 0.0f && h < rt.MinSize.y) h = rt.MinSize.y;
    if (rt.MaxSize.y > 0.0f && h > rt.MaxSize.y) h = rt.MaxSize.y;
    if (element->FitHeight) {
        element->ResolvedRect = Rectangle{ 0.0f, 0.0f, w, h };
        h = FitHeightAt(element, w);
    }

    float x = 0.0f;
    if (rt.AnchorMin.x == rt.AnchorMax.x) {
        x = ax0 + rt.AnchoredPosition.x - rt.Pivot.x * w;
    } else {
        x = ax0 + rt.AnchoredPosition.x + (ax1 - ax0 - w) * rt.Pivot.x;
    }

    float y = 0.0f;
    if (rt.AnchorMin.y == rt.AnchorMax.y) {
        y = ay0 + rt.AnchoredPosition.y - rt.Pivot.y * h;
    } else {
        y = ay0 + rt.AnchoredPosition.y + (ay1 - ay0 - h) * rt.Pivot.y;
    }

    element->ResolvedRect = Rectangle{ x, y, w, h };

    if (content) ResolveElementContent(element);
}

void UICanvas::ResolveElementContent(UIElement* element)
{
    if (!element) return;

    // A container lays its shown children out again and resolves their content then (ApplyContainerLayout),
    // so here they only need their own size; resolving their subtrees twice per level grew as 4^depth.
    // Scroll views still resolve now: their ContentHeight decides the scrollbar reserve below.
    const bool container = element->Layout != UILayoutMode::None;
    for (auto& child : element->Children) {
        UIElement* c = child.get();
        const bool laidOutLater = container && c->Visible && !c->IgnoreLayout && c->Type != UIElementType::ScrollView;
        ResolveElementLayout(c, element->ResolvedRect, !laidOutLater);
    }
    if (element->Layout != UILayoutMode::None) {
        ApplyContainerLayout(element);
    }

    // Single chokepoint for ScrollView layout and content sizing:
    // Any ScrollView (whether standalone or positioned as a child of a layout container)
    // must measure its full content subtree bounds, clamp its ScrollOffset against its
    // actual resolved viewport, and translate all descendant rectangles by ScrollOffset.
    // If bypassed (e.g. during parent container layout), content never shifts when scrolled,
    // ContentHeight under-reports when child panels contain taller descendants, and ScrollOffset
    // is clamped against stale/default dimensions.
    if (element->Type == UIElementType::ScrollView) {
        UIScrollView* sv = static_cast<UIScrollView*>(element);

        // Measure full subtree bounds of all visible descendants rather than direct children
        // alone. If direct children alone were measured, child panels that declare a short
        // height but contain taller overflowing content would cause the scroll view to under-report
        // its content height and prevent scrolling. Nested ScrollViews manage their own viewports/clipping,
        // so the recursion stops at them.
        bool wasScrollable = (element->ResolvedRect.height > 0.0f && sv->ContentHeight > element->ResolvedRect.height);
        float maxY = 0.0f;
        auto measureSubtree = [&](UIElement* elem, auto&& self) -> void {
            for (auto& child : elem->Children) {
                if (child->Visible) {
                    float bottomEdge = child->ResolvedRect.y + child->ResolvedRect.height;
                    float relBottom = bottomEdge - element->ResolvedRect.y;
                    if (relBottom > maxY) maxY = relBottom;
                    if (child->Type != UIElementType::ScrollView) {
                        self(child.get(), self);
                    }
                }
            }
        };
        measureSubtree(element, measureSubtree);

        sv->ContentHeight = std::max(0.0f, maxY);
        bool isScrollable = (element->ResolvedRect.height > 0.0f && sv->ContentHeight > element->ResolvedRect.height);
        if (wasScrollable != isScrollable && element->Layout != UILayoutMode::None) {
            ApplyContainerLayout(element);
        }

        float maxScroll = std::max(0.0f, sv->ContentHeight - element->ResolvedRect.height);
        sv->ScrollOffset = std::clamp(sv->ScrollOffset, 0.0f, maxScroll);

        if (sv->ScrollOffset != 0.0f) {
            auto offsetDescendants = [](UIElement* e, float off, auto&& self) -> void {
                for (auto& c : e->Children) {
                    c->ResolvedRect.y -= off;
                    self(c.get(), off, self);
                }
            };

            for (size_t i = 0; i < element->Children.size(); ++i) {
                element->Children[i]->ResolvedRect.y -= sv->ScrollOffset;
                offsetDescendants(element->Children[i].get(), sv->ScrollOffset, offsetDescendants);
            }
        }
    }
}

// Row-major flow of `sizes` into rows `width` wide; fills `pos` (offsets in the box) and returns the height.
static float FlowRows(const std::vector<Vector2>& sizes, float width, float spacing, std::vector<Vector2>* pos)
{
    float x = 0.0f, y = 0.0f, rowH = 0.0f;
    for (const Vector2& s : sizes) {
        if (x > 0.0f && x + s.x > width) { y += rowH + spacing; x = 0.0f; rowH = 0.0f; }
        if (pos) pos->push_back({ x, y });
        x += s.x + spacing;
        rowH = std::max(rowH, s.y);
    }
    return y + rowH;
}

static float ClampHeight(const UIElement* e, float h)
{
    if (e->Rect.MinSize.y > 0.0f) h = std::max(h, e->Rect.MinSize.y);
    if (e->Rect.MaxSize.y > 0.0f) h = std::min(h, e->Rect.MaxSize.y);
    return h;
}

// FitHeight: the element's content height at `width`. Children are laid out on their own anchors in a box
// `width` wide (its ResolvedRect's x, y; a stretching vertical stack gives them its inner width), so
// a fit parent's bottom-anchored children are measured where they would sit at its old height.
float UICanvas::FitHeightAt(UIElement* element, float width)
{
    const float text = element->TextHeightAt(width);
    if (text >= 0.0f) return ClampHeight(element, text);

    const UILayoutMode mode = element->Layout;
    const Rectangle box = { element->ResolvedRect.x, element->ResolvedRect.y, width, element->ResolvedRect.height };
    const float innerW = std::max(0.0f, width - element->PaddingLeft - element->PaddingRight);
    std::vector<Vector2> sizes;
    float lowest = 0.0f;
    for (auto& child : element->Children) {
        UIElement* c = child.get();
        if (!c->Visible || (mode != UILayoutMode::None && c->IgnoreLayout)) continue;
        ResolveElementLayout(c, box, false);   // a size is all a measure needs; the caller resolves content after
        float cw = c->ResolvedRect.width;
        if (mode == UILayoutMode::Vertical && element->StretchChildrenCrossAxis) {
            cw = innerW;
            if (c->FitHeight) c->ResolvedRect.height = FitHeightAt(c, cw);
        }
        sizes.push_back({ cw, c->ResolvedRect.height });
        lowest = std::max(lowest, c->ResolvedRect.y + c->ResolvedRect.height - box.y);
    }
    float content = 0.0f;
    if (mode == UILayoutMode::None) return ClampHeight(element, lowest + element->PaddingBottom);
    if (mode == UILayoutMode::Vertical) {
        for (const Vector2& s : sizes) content += s.y;
        if (!sizes.empty()) content += element->Spacing * static_cast<float>(sizes.size() - 1);
    } else if (element->WrapChildren) {
        content = FlowRows(sizes, innerW, element->Spacing, nullptr);
    } else {
        for (const Vector2& s : sizes) content = std::max(content, s.y);
    }
    return ClampHeight(element, element->PaddingTop + content + element->PaddingBottom);
}

void UICanvas::ApplyContainerLayout(UIElement* element)
{
    if (!element || element->Layout == UILayoutMode::None) return;

    float reservedScrollW = 0.0f;
    if (element->Type == UIElementType::ScrollView) {
        UIScrollView* sv = static_cast<UIScrollView*>(element);
        if (element->ResolvedRect.height > 0.0f && sv->ContentHeight > element->ResolvedRect.height) {
            reservedScrollW = UIScrollView::ScrollbarWidth;
        }
    }

    Rectangle contentBox = {
        element->ResolvedRect.x + element->PaddingLeft,
        element->ResolvedRect.y + element->PaddingTop,
        std::max(0.0f, element->ResolvedRect.width - element->PaddingLeft - element->PaddingRight - reservedScrollW),
        std::max(0.0f, element->ResolvedRect.height - element->PaddingTop - element->PaddingBottom)
    };

    std::vector<UIElement*> visibleChildren;
    for (auto& child : element->Children) {
        if (child->Visible && !child->IgnoreLayout) {
            visibleChildren.push_back(child.get());
        }
    }

    if (visibleChildren.empty()) return;

    const bool horizontal = element->Layout == UILayoutMode::Horizontal;
    if (horizontal && element->WrapChildren) {   // rows of children at their own sizes
        std::vector<Vector2> sizes, pos;
        for (auto* c : visibleChildren) sizes.push_back({ c->ResolvedRect.width, c->ResolvedRect.height });
        FlowRows(sizes, contentBox.width, element->Spacing, &pos);
        for (size_t i = 0; i < visibleChildren.size(); ++i) {
            visibleChildren[i]->ResolvedRect = { contentBox.x + pos[i].x, contentBox.y + pos[i].y, sizes[i].x, sizes[i].y };
            ResolveElementContent(visibleChildren[i]);
        }
        return;
    }
    const bool expand = horizontal ? element->ExpandChildrenWidth : element->ExpandChildrenHeight;
    const float spacing = element->Spacing;
    if (!horizontal && element->StretchChildrenCrossAxis) {   // a fit child's height at the stack's width
        for (auto* c : visibleChildren) {
            if (c->FitHeight) c->ResolvedRect.height = FitHeightAt(c, contentBox.width);
        }
    }
    const float boxMain = horizontal ? contentBox.width : contentBox.height;
    const float boxCross = horizontal ? contentBox.height : contentBox.width;

    // Main-axis sizes. In an expanding container a child with flex_weight 0 keeps its own size and
    // the children with flex_weight > 0 share what is left by weight, each within its min/max size.
    std::vector<float> sizes;
    sizes.reserve(visibleChildren.size());
    float fixed = 0.0f, weightSum = 0.0f;
    for (auto* c : visibleChildren) {
        sizes.push_back(horizontal ? c->ResolvedRect.width : c->ResolvedRect.height);
        if (expand && c->FlexWeight > 0.0f) weightSum += c->FlexWeight;
        else fixed += sizes.back();
    }
    const float totalSpacing = spacing * static_cast<float>(visibleChildren.size() - 1);
    float run = totalSpacing;
    const float freeSpace = std::max(0.0f, boxMain - totalSpacing - fixed);
    for (size_t i = 0; i < visibleChildren.size(); ++i) {
        const UIElement* c = visibleChildren[i];
        if (expand && weightSum > 0.0f && c->FlexWeight > 0.0f) {
            float s = freeSpace * c->FlexWeight / weightSum;
            const float lo = horizontal ? c->Rect.MinSize.x : c->Rect.MinSize.y;
            const float hi = horizontal ? c->Rect.MaxSize.x : c->Rect.MaxSize.y;
            if (hi > 0.0f) s = std::min(s, hi);
            if (lo > 0.0f) s = std::max(s, lo);
            sizes[i] = s;
        }
        run += sizes[i];
    }

    float cur = horizontal ? contentBox.x : contentBox.y;
    if (element->ChildAlignment == UIAlign::Center) cur += (boxMain - run) * 0.5f;
    else if (element->ChildAlignment == UIAlign::End) cur += boxMain - run;

    for (size_t i = 0; i < visibleChildren.size(); ++i) {
        UIElement* c = visibleChildren[i];
        const bool stretch = element->StretchChildrenCrossAxis;
        if (horizontal && !stretch && c->FitHeight) c->ResolvedRect.height = FitHeightAt(c, sizes[i]);
        const float cross = stretch ? boxCross : (horizontal ? c->ResolvedRect.height : c->ResolvedRect.width);
        float crossPos = horizontal ? contentBox.y : contentBox.x;
        if (!stretch && element->ChildAlignment == UIAlign::Center) crossPos += (boxCross - cross) * 0.5f;
        else if (!stretch && element->ChildAlignment == UIAlign::End) crossPos += boxCross - cross;

        c->ResolvedRect = horizontal ? Rectangle{ cur, crossPos, sizes[i], cross }
                                     : Rectangle{ crossPos, cur, cross, sizes[i] };
        cur += sizes[i] + spacing;

        ResolveElementContent(c);
    }
}

void UICanvas::ResolveTransformsAndOpacity(UIElement* element, const UITransform2D& parentTransform, float parentOpacity)
{
    if (!element) return;

    element->ResolvedOpacity = parentOpacity * std::clamp(element->Opacity, 0.0f, 1.0f);

    float pivotX = element->ResolvedRect.x + element->Rect.Pivot.x * element->ResolvedRect.width;
    float pivotY = element->ResolvedRect.y + element->Rect.Pivot.y * element->ResolvedRect.height;

    float scaleMul = element->TransitionState.CurrentScaleMul;
    Vector2 effScale = { element->Rect.Scale.x * scaleMul, element->Rect.Scale.y * scaleMul };

    UITransform2D localTransform = UITransform2D::ScaleRotateAround(
        Vector2{ pivotX, pivotY },
        effScale,
        element->Rect.Rotation
    );
    localTransform.tx += element->Rect.Translation.x;   // Translate(t) after the scale/rotation
    localTransform.ty += element->Rect.Translation.y;

    element->ResolvedTransform = parentTransform.Compose(localTransform);

    if (!element->Visible) return;   // a hidden subtree is not drawn or hit (§20: may be stale while hidden)
    for (size_t i = 0; i < element->Children.size(); ++i) {
        ResolveTransformsAndOpacity(element->Children[i].get(), element->ResolvedTransform, element->ResolvedOpacity);
    }
}

} // namespace IYI::UI
