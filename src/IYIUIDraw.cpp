// Moved verbatim out of IYIUI.cpp; drawing, text fitting and scissor clipping.

#include "IYI/IYIUI/IYIUI.h"
#include "IYI/Application.h"
#include "IYI/Utf8Text.h"
#include "IYI/FramePacket.h"
#include "IYI/Ime.h"
#include "IYI/FrameInput.h"
#include "IYI/Profiler.h"
#include "IYI/Video.h"
#include "rlgl.h"
#include <algorithm>
#include <cstdio>
#include <bit>
#include <cmath>
#include <cassert>
#include <utility>

namespace IYI::UI {

struct ScissorRect
{
    int x;
    int y;
    int width;
    int height;
};

static std::vector<ScissorRect> s_scissorStack;
static float s_currentScaleFactor = 1.0f;
// Mirrors the active rlgl model-view matrix across element hierarchies.
// Used by PushScissor to map logical element bounds into screen-space GPU scissor
// coordinates without reading back GPU state.
static UITransform2D s_currentTransform = UITransform2D::Identity();

static Rectangle TransformRectAABB(const Rectangle& rect, const UITransform2D& transform)
{
    if (rect.width <= 0.0f || rect.height <= 0.0f ||
        std::isnan(rect.x) || std::isnan(rect.y) ||
        std::isnan(rect.width) || std::isnan(rect.height) ||
        std::isinf(rect.x) || std::isinf(rect.y) ||
        std::isinf(rect.width) || std::isinf(rect.height)) {
        return Rectangle{ 0.0f, 0.0f, 0.0f, 0.0f };
    }

    Vector2 p0 = transform.Apply(Vector2{ rect.x, rect.y });
    Vector2 p1 = transform.Apply(Vector2{ rect.x + rect.width, rect.y });
    Vector2 p2 = transform.Apply(Vector2{ rect.x + rect.width, rect.y + rect.height });
    Vector2 p3 = transform.Apply(Vector2{ rect.x, rect.y + rect.height });

    if (std::isnan(p0.x) || std::isnan(p0.y) ||
        std::isnan(p1.x) || std::isnan(p1.y) ||
        std::isnan(p2.x) || std::isnan(p2.y) ||
        std::isnan(p3.x) || std::isnan(p3.y) ||
        std::isinf(p0.x) || std::isinf(p0.y) ||
        std::isinf(p1.x) || std::isinf(p1.y) ||
        std::isinf(p2.x) || std::isinf(p2.y) ||
        std::isinf(p3.x) || std::isinf(p3.y)) {
        return Rectangle{ 0.0f, 0.0f, 0.0f, 0.0f };
    }

    float minX = std::min({ p0.x, p1.x, p2.x, p3.x });
    float maxX = std::max({ p0.x, p1.x, p2.x, p3.x });
    float minY = std::min({ p0.y, p1.y, p2.y, p3.y });
    float maxY = std::max({ p0.y, p1.y, p2.y, p3.y });

    float w = maxX - minX;
    float h = maxY - minY;

    if (w <= 0.0f || h <= 0.0f) {
        return Rectangle{ 0.0f, 0.0f, 0.0f, 0.0f };
    }

    return Rectangle{ minX, minY, w, h };
}

// Every GL draw call in this file is recorded with Record() (FramePacket.h) and replayed on the render side.

// Single chokepoint guarantee: scissor clipping strictly matches transformed element bounds.
// BeginScissorMode operates in screen-space pixels and completely ignores the rlgl model-view
// matrix, but UI elements render inside nested transforms (root scale and per-element
// translation, rotation, and pivot scaling). PushScissor reconciles the two by transforming
// the logical rectangle via accumulated s_currentTransform and taking its axis-aligned
// bounding box before converting to pixels and intersecting with the scissor stack.
// Calling BeginScissorMode directly means anything tweened, scaled by a hover/pressed
// style, or rotated gets sliced at its edges (e.g. a card title losing its first characters
// and top row).
// Invariant: s_currentTransform must stay composed and restored in exact symmetry with
// rlPushMatrix / rlPopMatrix, or sibling elements will inherit each other's transform.
static void PushScissor(Rectangle logicalRect, float scaleFactor)
{
    Rectangle aabb = TransformRectAABB(logicalRect, s_currentTransform);

    int px = static_cast<int>(std::floor(aabb.x * scaleFactor));
    int py = static_cast<int>(std::floor(aabb.y * scaleFactor));
    int pw = static_cast<int>(std::ceil(aabb.width * scaleFactor));
    int ph = static_cast<int>(std::ceil(aabb.height * scaleFactor));

    if (pw <= 0 || ph <= 0) {
        pw = 0;
        ph = 0;
    }

    int screenW = GameInput().ScreenWidth;
    int screenH = GameInput().ScreenHeight;

    ScissorRect current = s_scissorStack.empty()
        ? ScissorRect{ 0, 0, screenW, screenH }
        : s_scissorStack.back();

    int ix0 = std::max(current.x, px);
    int iy0 = std::max(current.y, py);
    int ix1 = std::min(current.x + current.width, px + pw);
    int iy1 = std::min(current.y + current.height, py + ph);

    int iw = std::max(0, ix1 - ix0);
    int ih = std::max(0, iy1 - iy0);

    ScissorRect next{ ix0, iy0, iw, ih };
    s_scissorStack.push_back(next);

    if (iw > 0 && ih > 0) {
        RecordState([=]() { BeginScissorMode(ix0, iy0, iw, ih); });
    }
}

// True when `logicalRect` grown by `pad` (what an element may draw past its rect: shadow, focus ring,
// a slider knob) lies wholly outside the innermost clip, so drawing it would leave no pixel.
static bool OutsideClip(Rectangle logicalRect, float pad, float scaleFactor)
{
    if (s_scissorStack.empty()) return false;
    Rectangle a = TransformRectAABB({ logicalRect.x - pad, logicalRect.y - pad,
                                      logicalRect.width + 2.0f * pad, logicalRect.height + 2.0f * pad }, s_currentTransform);
    const ScissorRect& c = s_scissorStack.back();
    return c.width <= 0 || c.height <= 0 || a.x * scaleFactor >= c.x + c.width || (a.x + a.width) * scaleFactor <= c.x
        || a.y * scaleFactor >= c.y + c.height || (a.y + a.height) * scaleFactor <= c.y;
}

static void PopScissor()
{
    if (s_scissorStack.empty()) return;
    RecordState([]() { EndScissorMode(); });
    s_scissorStack.pop_back();
    if (!s_scissorStack.empty()) {
        const ScissorRect top = s_scissorStack.back();
        if (top.width > 0 && top.height > 0) {
            RecordState([=]() { BeginScissorMode(top.x, top.y, top.width, top.height); });
        }
    }
}

// UIPanel
UIPanel::UIPanel(uint64_t id, const std::string& name, UICanvas* canvas)
    : UIElement(UIElementType::Panel, id, name, canvas)
{
}

void UIPanel::DrawContent(const UIStyle& /*resolvedStyle*/, float /*opacity*/)
{
}

// UIText
UIText::UIText(uint64_t id, const std::string& name, UICanvas* canvas)
    : UIElement(UIElementType::Text, id, name, canvas)
{
}

static const std::string kEllipsis = "\xE2\x80\xA6";

static std::vector<std::string> SplitLines(const std::string& str)
{
    std::vector<std::string> lines;
    if (str.empty()) {
        lines.push_back("");
        return lines;
    }
    size_t start = 0;
    while (start < str.size()) {
        size_t pos = str.find('\n', start);
        if (pos == std::string::npos) {
            lines.push_back(str.substr(start));
            break;
        }
        lines.push_back(str.substr(start, pos - start));
        start = pos + 1;
        if (start == str.size()) {
            lines.push_back("");
            break;
        }
    }
    return lines;
}

// Han/kana admit breaks without spaces; Korean keeps word boundaries when possible.
static bool CjkBreakCharacter(int cp)
{
    return (cp >= 0x3000 && cp <= 0x30ff) || (cp >= 0x3400 && cp <= 0x9fff) ||
           (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xff01 && cp <= 0xff60) ||
           (cp >= 0x20000 && cp <= 0x323af);
}

static bool CannotStartLine(int cp)
{
    static const std::string chars = "、。，．！？：；)]}）］｝〉》」』】〕〗〙〛ー々ぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ";
    int bytes = 0;
    const char* utf8 = CodepointToUTF8(cp, &bytes);
    return chars.find(std::string(utf8, bytes)) != std::string::npos;
}

static bool CannotEndLine(int cp)
{
    static const std::string chars = "([{（［｛〈《「『【〔〖〘〚";
    int bytes = 0;
    const char* utf8 = CodepointToUTF8(cp, &bytes);
    return chars.find(std::string(utf8, bytes)) != std::string::npos;
}

static std::vector<std::string> WordWrapLine(const Font& font, const std::string& line,
                                             float maxWidth, float fontSize, float spacing)
{
    if (line.empty()) return {""};
    struct Unit { std::string Text; bool Space; };
    std::vector<Unit> units;
    std::string word;
    bool space = false;
    bool wordSpace = false;
    bool opening = false;
    auto flush = [&]() {
        if (!word.empty()) { units.push_back({word, wordSpace}); word.clear(); }
        opening = false;
    };
    for (size_t pos = 0; pos < line.size();) {
        int bytes = 0;
        int cp = GetCodepointNext(line.c_str() + pos, &bytes);
        std::string glyph = line.substr(pos, bytes);
        pos += bytes;
        if (cp == ' ' || cp == '\t' || cp == 0x3000) { flush(); space = true; continue; }
        if (CannotStartLine(cp) && word.empty() && !units.empty() && !space) {
            units.back().Text += glyph;
            continue;
        }
        if (CjkBreakCharacter(cp) || CannotEndLine(cp)) {
            if (!opening) flush();
            if (word.empty()) wordSpace = space;
            word += glyph;
            space = false;
            opening = CannotEndLine(cp);
            if (!opening) flush();
        } else {
            if (word.empty()) wordSpace = space;
            word += glyph;
            space = false;
        }
    }
    flush();
    std::vector<std::string> result;
    std::string current;
    auto width = [&](const std::string& text) { return MeasureTextEx(font, text.c_str(), fontSize, spacing).x; };
    for (const auto& unit : units) {
        std::string joined = current + (current.empty() || !unit.Space ? "" : " ") + unit.Text;
        if (!current.empty() && width(joined) > maxWidth) {
            result.push_back(current);
            current.clear();
        }
        if (width(unit.Text) <= maxWidth) {
            current += (current.empty() || !unit.Space ? "" : " ") + unit.Text;
            continue;
        }
        // An unusually long Korean/Latin word or player name still breaks at UTF-8 boundaries.
        for (size_t pos = 0; pos < unit.Text.size();) {
            int bytes = 0;
            int cp = GetCodepointNext(unit.Text.c_str() + pos, &bytes);
            std::string glyph = unit.Text.substr(pos, bytes);
            pos += bytes;
            if (!current.empty() && width(current + glyph) > maxWidth && !CannotStartLine(cp)) {
                result.push_back(current);
                current.clear();
            }
            current += glyph;
        }
    }
    if (!current.empty()) result.push_back(current);
    return result.empty() ? std::vector<std::string>{""} : result;
}

static std::vector<std::string> LayoutText(const Font& font, const std::string& text,
                                           float maxWidth, float fontSize, float spacing, bool wrap)
{
    std::vector<std::string> rawLines = SplitLines(text);
    if (!wrap || maxWidth <= 0.0f) {
        return rawLines;
    }

    std::vector<std::string> wrapped;
    for (const auto& raw : rawLines) {
        std::vector<std::string> sub = WordWrapLine(font, raw, maxWidth, fontSize, spacing);
        wrapped.insert(wrapped.end(), sub.begin(), sub.end());
    }
    return wrapped;
}

static void MeasureLines(const Font& font, const std::vector<std::string>& lines,
                         float fontSize, float spacing, float& outWidth, float& outHeight)
{
    outWidth = 0.0f;
    outHeight = 0.0f;
    if (lines.empty()) return;

    for (const auto& l : lines) {
        if (!l.empty()) {
            Vector2 sz = MeasureTextEx(font, l.c_str(), fontSize, spacing);
            if (sz.x > outWidth) {
                outWidth = sz.x;
            }
        }
    }
    float lineSpacing = fontSize * 1.15f;
    outHeight = (lines.size() <= 1) ? (static_cast<float>(lines.size()) * fontSize)
                                    : (static_cast<float>(lines.size()) * lineSpacing);
}

static std::string TruncateWithEllipsis(const Font& font, const std::string& str,
                                        float maxWidth, float fontSize, float spacing, bool& outFits)
{
    float ellipsisW = MeasureTextEx(font, kEllipsis.c_str(), fontSize, spacing).x;
    if (ellipsisW > maxWidth) {
        outFits = false;
        return "";
    }

    std::string cur = str;
    while (!cur.empty()) {
        std::string testStr = cur + kEllipsis;
        float w = MeasureTextEx(font, testStr.c_str(), fontSize, spacing).x;
        if (w <= maxWidth) {
            outFits = true;
            return testStr;
        }
        IYI::PopLastCodepoint(cur);
    }

    outFits = true;
    return kEllipsis;
}

// FontForText for the text a memo fits, asked again only when the text or the font cache changed
// (FontForText takes a lock and scans every codepoint; it ran several times per text per frame).
static const Font& MemoFont(const Font& sourceFont, const std::string& text, UITextMemo& memo, float pixelSize)
{
    AssetManager& assets = CurrentApp().GetAssets();
    if (memo.ResolvedFont && memo.FontSource == sourceFont.glyphs && memo.FontGen == assets.FontGeneration()
        && memo.LastText == text && memo.ResolvedPixelSize == pixelSize) {
        return *memo.ResolvedFont;
    }
    memo.ResolvedFont = assets.FontForText(sourceFont, text, pixelSize);
    memo.ResolvedPixelSize = pixelSize;
    memo.FontSource = sourceFont.glyphs;
    memo.FontGen = assets.FontGeneration();
    memo.DrawFont = nullptr;
    return *memo.ResolvedFont;
}

// A pixel font's native sizes, asked again only when the font or the font cache changed.
static const std::vector<int>& MemoGrids(const Font& sourceFont, UITextMemo& memo)
{
    AssetManager& assets = CurrentApp().GetAssets();
    if (memo.GridsSource != sourceFont.glyphs || memo.GridsGen != assets.FontGeneration()) {
        memo.PixelGrids = assets.PixelFontSizes(sourceFont);
        memo.GridsSource = sourceFont.glyphs;
        memo.GridsGen = assets.FontGeneration();
    }
    return memo.PixelGrids;
}

// Logical units to screen pixels for what draws now (canvas scale times the current transform's).
static float ScreenScale()
{
    const UITransform2D& t = s_currentTransform;
    return s_currentScaleFactor * std::sqrt(std::fabs(t.a * t.d - t.b * t.c));
}

static const Font& FitText(const Font& sourceFont, const std::string& text, Rectangle box,
                    float requestedFontSize, float spacing, const UITextFit& fit,
                    UITextMemo& memo)
{
    // Measurement and drawing must resolve the identical expanded atlas, including names
    // absent from the preloaded locale. Memo identity uses the stable glyph buffer/texture.
    // A pixel font fits at the size it draws (the nearest multiple of a native size) with that size's
    // own glyphs, so lines wrap for what is drawn.
    const float scale = ScreenScale();
    const std::vector<int>& grids = MemoGrids(sourceFont, memo);
    requestedFontSize = SnapPixelFontSize(requestedFontSize, scale, grids, true);
    if (!grids.empty()) spacing = 0.0f;   // a pixel font's advances already hold its letter gap
    const Font& font = MemoFont(sourceFont, text, memo, grids.empty() ? 0.0f : requestedFontSize * scale);
    if (text.empty() || box.width <= 0.0f || box.height <= 0.0f) {
        memo.UpdateInputs(text, box.width, box.height, requestedFontSize, &font, fit);
        memo.FittedFontSize = requestedFontSize;
        memo.Lines.clear();
        memo.Ellipsized = false;
        memo.MeasuredWidth = 0.0f;
        memo.MeasuredHeight = 0.0f;
        return font;
    }

    if (!memo.NeedsUpdate(text, box.width, box.height, requestedFontSize, &font, fit)) {
        return font;
    }

    // Step 2: Lay text out at requestedFontSize
    std::vector<std::string> lines = LayoutText(font, text, box.width, requestedFontSize, spacing, fit.Wrap);

    // Step 3: Measure
    float width = 0.0f;
    float height = 0.0f;
    MeasureLines(font, lines, requestedFontSize, spacing, width, height);

    // Step 4: Fits?
    if (width <= box.width && height <= box.height) {
        memo.UpdateInputs(text, box.width, box.height, requestedFontSize, &font, fit);
        // Snap to integer size for stable glyph advances and consistent stroke weight across elements.
        memo.FittedFontSize = std::max(1.0f, std::floor(requestedFontSize));
        memo.Lines = std::move(lines);
        memo.Ellipsized = false;
        memo.MeasuredWidth = width;
        memo.MeasuredHeight = height;
        return font;
    }

    // Step 5: AutoShrink -> binary search in [MinFontSize, requestedFontSize]
    if (fit.AutoShrink && requestedFontSize > fit.MinFontSize) {
        float low = fit.MinFontSize;
        float high = requestedFontSize;

        std::vector<std::string> lowLines = LayoutText(font, text, box.width, low, spacing, fit.Wrap);
        float lowW = 0.0f, lowH = 0.0f;
        MeasureLines(font, lowLines, low, spacing, lowW, lowH);

        if (lowW <= box.width && lowH <= box.height) {
            float bestSize = low;
            std::vector<std::string> bestLines = std::move(lowLines);
            float bestW = lowW, bestH = lowH;

            for (int iter = 0; iter < 10; ++iter) {
                float mid = (low + high) * 0.5f;
                std::vector<std::string> midLines = LayoutText(font, text, box.width, mid, spacing, fit.Wrap);
                float midW = 0.0f, midH = 0.0f;
                MeasureLines(font, midLines, mid, spacing, midW, midH);

                if (midW <= box.width && midH <= box.height) {
                    bestSize = mid;
                    bestLines = std::move(midLines);
                    bestW = midW;
                    bestH = midH;
                    low = mid;
                } else {
                    high = mid;
                }
            }

            memo.UpdateInputs(text, box.width, box.height, requestedFontSize, &font, fit);
            // Snap to integer size for stable glyph advances and consistent stroke weight across elements.
            memo.FittedFontSize = std::max(1.0f, std::floor(bestSize));
            memo.Lines = std::move(bestLines);
            memo.Ellipsized = false;
            memo.MeasuredWidth = bestW;
            memo.MeasuredHeight = bestH;
            return font;
        }
    }

    // Step 6: Still not fitting at MinFontSize (or AutoShrink disabled) -> ellipsize with U+2026
    float fontSize = fit.AutoShrink ? fit.MinFontSize : requestedFontSize;
    std::vector<std::string> linesAtSize = LayoutText(font, text, box.width, fontSize, spacing, fit.Wrap);

    float ellipW = MeasureTextEx(font, kEllipsis.c_str(), fontSize, spacing).x;
    if (ellipW > box.width || box.height < fontSize) {
        memo.UpdateInputs(text, box.width, box.height, requestedFontSize, &font, fit);
        memo.FittedFontSize = fontSize;
        memo.Lines.clear();
        memo.Ellipsized = true;
        memo.MeasuredWidth = 0.0f;
        memo.MeasuredHeight = 0.0f;
        return font;
    }

    int maxLines = 1;
    float lineSpacing = fontSize * 1.15f;
    while (true) {
        float nextH = static_cast<float>(maxLines + 1) * lineSpacing;
        if (nextH <= box.height) {
            maxLines++;
        } else {
            break;
        }
    }

    bool wasCutVertically = false;
    if (linesAtSize.size() > static_cast<size_t>(maxLines)) {
        linesAtSize.resize(maxLines);
        wasCutVertically = true;
    }

    for (size_t i = 0; i < linesAtSize.size(); ++i) {
        bool isLastLine = (i == linesAtSize.size() - 1);
        float lineW = MeasureTextEx(font, linesAtSize[i].c_str(), fontSize, spacing).x;
        if (lineW > box.width || (isLastLine && wasCutVertically)) {
            bool fits = false;
            linesAtSize[i] = TruncateWithEllipsis(font, linesAtSize[i], box.width, fontSize, spacing, fits);
            if (!fits) {
                linesAtSize.clear();
                break;
            }
        }
    }

    float finalW = 0.0f, finalH = 0.0f;
    MeasureLines(font, linesAtSize, fontSize, spacing, finalW, finalH);

    memo.UpdateInputs(text, box.width, box.height, requestedFontSize, &font, fit);
    memo.FittedFontSize = fontSize;
    memo.Lines = std::move(linesAtSize);
    memo.Ellipsized = true;
    memo.MeasuredWidth = finalW;
    memo.MeasuredHeight = finalH;
    return font;
}

// Single chokepoint guarantee: UI text can never overflow or render outside its bounding box.
// This function is the only place UI text reaches raylib. Overflow is made unreachable by a
// strict pipeline: word wrap, bounded binary-search font shrink (at most 10 measure passes,
// never one per point), ellipsis at the shrink floor, and an enclosing PushScissor clip.
// Because ellipsis and scissor clipping always apply, text cannot leave its box even if both
// wrap and auto-shrink flags are disabled.
// A widget that calls DrawTextEx directly gets none of these protections and will look fine
// until someone resizes the window or localized strings expand.
// Note: engine.draw_text and draw_text_ex forwards in RaylibBindings.cpp are deliberately NOT
// covered — those handle the game's own world-space drawing and must stay unclipped.
void DrawTextFitted(const Font& sourceFont, const std::string& text, Rectangle box,
                    float requestedFontSize, float spacing, Color color,
                    UIHAlign hAlign, UIVAlign vAlign, const UITextFit& fit,
                    UITextMemo* memo, UIElement* owner)
{
    if (text.empty() || box.width <= 0.0f || box.height <= 0.0f) {
        return;
    }

    UITextMemo localMemo;
    UITextMemo& m = memo ? *memo : localMemo;
    if (owner && owner->Canvas && !owner->FontGroup.empty()) {
        UITextMemo& own = owner->GroupMemo;   // its own fit (as drawn), kept apart from the capped one
        FitText(sourceFont, text, box, requestedFontSize, spacing, fit, own);
        const float drawn = SnapPixelFontSize(own.FittedFontSize, ScreenScale(), own.PixelGrids, false);
        requestedFontSize = owner->Canvas->FontGroupSize(owner->FontGroup, drawn);
    }
    const Font& font = FitText(sourceFont, text, box, requestedFontSize, spacing, fit, m);

    if (m.Lines.empty()) {
        return;
    }

    // Step 7: Push box onto scissor stack around draw as final hard guarantee. Text the fit measured
    // inside its box needs none (each scissor change flushes the batch: two to three per text).
    const bool clip = m.Ellipsized || m.MeasuredWidth > box.width || m.MeasuredHeight > box.height;
    if (clip) PushScissor(box, s_currentScaleFactor);

    const float screenScale = ScreenScale();
    float fontSize = SnapPixelFontSize(m.FittedFontSize, screenScale, m.PixelGrids, false);   // a shrunk fit: next size down
    float lineSpacing = fontSize * 1.15f;
    float totalHeight = fontSize == m.FittedFontSize ? m.MeasuredHeight
                      : static_cast<float>(m.Lines.size() - 1) * lineSpacing + fontSize;

    float startY = box.y;
    if (vAlign == UIVAlign::Middle) {
        startY = box.y + (box.height - totalHeight) * 0.5f;
    } else if (vAlign == UIVAlign::Bottom) {
        startY = box.y + (box.height - totalHeight);
    }

    // Lines are measured with the font's own atlas; on screen a zoomed or high-DPI draw takes a larger
    // atlas of the same glyphs (more texels), so scaled text stays sharp. A pixel font draws (and so is
    // measured) with the native size the drawn size is a multiple of.
    const float pixelSize = fontSize * screenScale;
    if (!m.DrawFont || m.DrawPixelSize != pixelSize) {   // MemoFont clears it whenever the fonts changed
        m.DrawFont = CurrentApp().GetAssets().FontForText(sourceFont, text, pixelSize);
        m.DrawPixelSize = pixelSize;
    }
    const Font* drawFont = m.DrawFont;
    const Font& measureFont = m.PixelGrids.empty() ? font : *drawFont;
    if (!m.PixelGrids.empty()) spacing = 0.0f;   // as FitText measured it

    std::vector<std::pair<std::string, Vector2>> drawLines;
    drawLines.reserve(m.Lines.size());
    if (m.LineWidths.size() != m.Lines.size() || m.LineWidthSpacing != spacing || m.LineWidthFontSize != fontSize) {   // cleared by every refit
        m.LineWidths.clear();
        for (const std::string& line : m.Lines) m.LineWidths.push_back(MeasureTextEx(measureFont, line.c_str(), fontSize, spacing).x);
        m.LineWidthSpacing = spacing;
        m.LineWidthFontSize = fontSize;
    }
    for (size_t i = 0; i < m.Lines.size(); ++i) {
        const float lineW = m.LineWidths[i];
        float lineX = box.x;
        if (hAlign == UIHAlign::Center) {
            lineX = box.x + (box.width - lineW) * 0.5f;
        } else if (hAlign == UIHAlign::Right) {
            lineX = box.x + (box.width - lineW);
        }
        Vector2 pos = { lineX, startY + static_cast<float>(i) * lineSpacing };
        drawLines.emplace_back(m.Lines[i], pos);
    }
    RecordText([font = drawFont, lines = std::move(drawLines), fontSize, spacing, color]() {
        BeginTextDraw();   // EndTextDraw comes with the next non-text command (RecordText)
        for (const auto& line : lines) {
            DrawTextEx(*font, line.first.c_str(), line.second, fontSize, spacing, color);
        }
    });

    if (clip) PopScissor();
}

void UIText::EnsureFitted()
{
    Font f = FontOverride ? *FontOverride : (Canvas && Canvas->Theme().HasDefaultFont ? Canvas->Theme().DefaultFont : GetFontDefault());
    const UIStyle& resolvedStyle = CustomStyle ? *CustomStyle : (Canvas ? Canvas->Theme().Resolve(StyleClass.empty() ? "text" : StyleClass) : UIStyle{});
    float fontSize = FontSize > 0.0f ? FontSize : resolvedStyle.FontSize;
    Rectangle r = ResolvedRect;
    Rectangle box = {
        r.x + resolvedStyle.PaddingLeft,
        r.y + resolvedStyle.PaddingTop,
        std::max(0.0f, r.width - resolvedStyle.PaddingLeft - resolvedStyle.PaddingRight),
        std::max(0.0f, r.height - resolvedStyle.PaddingTop - resolvedStyle.PaddingBottom)
    };
    FitText(f, ResolvedText(), box, fontSize, 1.0f, Fit(), Memo);
}

// FitHeight's measure: the text wrapped at `width` (minus padding) in an unbounded box, plus padding.
static float TextHeightIn(const Font& font, const std::string& text, const UIStyle& style, float fontSize,
                          float width, const UITextFit& fit, UITextMemo& memo)
{
    const float pad = style.PaddingTop + style.PaddingBottom;
    if (text.empty()) return pad;
    const Rectangle box = { 0.0f, 0.0f, std::max(1.0f, width - style.PaddingLeft - style.PaddingRight), 1.0e6f };
    FitText(font, text, box, fontSize, 1.0f, fit, memo);
    return memo.MeasuredHeight + pad;
}

static uint64_t TextKey(const std::string& text, float fontSize, bool wrap, const std::string& styleClass)
{
    return std::hash<std::string>{}(text) ^ (std::hash<std::string>{}(styleClass) << 1)
        ^ (static_cast<uint64_t>(std::bit_cast<uint32_t>(fontSize)) << 20) ^ (wrap ? 1u : 0u);
}

float UIText::TextHeightAt(float width)
{
    Font f = FontOverride ? *FontOverride : (Canvas && Canvas->Theme().HasDefaultFont ? Canvas->Theme().DefaultFont : GetFontDefault());
    const UIStyle& style = CustomStyle ? *CustomStyle : (Canvas ? Canvas->Theme().Resolve(StyleClass.empty() ? "text" : StyleClass) : UIStyle{});
    return TextHeightIn(f, ResolvedText(), style, FontSize > 0.0f ? FontSize : style.FontSize, width, Fit(), FitMemo);
}

uint64_t UIText::TextLayoutKey() const
{
    const UIStyle& style = CustomStyle ? *CustomStyle : (Canvas ? Canvas->Theme().Resolve(StyleClass.empty() ? "text" : StyleClass) : UIStyle{});
    return TextKey(ResolvedText(), FontSize > 0.0f ? FontSize : style.FontSize, Wrap, StyleClass);
}

float UIText::FittedFontSize()
{
    EnsureFitted();
    return Memo.FittedFontSize;
}

float UIText::MeasuredWidth()
{
    EnsureFitted();
    return Memo.MeasuredWidth;
}

float UIText::MeasuredHeight()
{
    EnsureFitted();
    return Memo.MeasuredHeight;
}

void UIText::DrawContent(const UIStyle& resolvedStyle, float opacity)
{
    if (ResolvedText().empty()) return;

    Font f = FontOverride ? *FontOverride : (Canvas && Canvas->Theme().HasDefaultFont ? Canvas->Theme().DefaultFont : GetFontDefault());
    float fontSize = FontSize > 0.0f ? FontSize : resolvedStyle.FontSize;
    Color col = ColorOverride.a > 0 ? ColorOverride : TransitionState.CurrentText;
    col = WithOpacity(col, opacity);

    float spacing = 1.0f;
    Rectangle r = ResolvedRect;
    Rectangle box = {
        r.x + resolvedStyle.PaddingLeft,
        r.y + resolvedStyle.PaddingTop,
        std::max(0.0f, r.width - resolvedStyle.PaddingLeft - resolvedStyle.PaddingRight),
        std::max(0.0f, r.height - resolvedStyle.PaddingTop - resolvedStyle.PaddingBottom)
    };

    DrawTextFitted(f, ResolvedText(), box, fontSize, spacing, col, HAlign, VAlign, Fit(), &Memo, this);
}


// UIImage
UIImage::UIImage(uint64_t id, const std::string& name, UICanvas* canvas)
    : UIElement(UIElementType::Image, id, name, canvas)
{
}

void UIImage::DrawContent(const UIStyle& /*resolvedStyle*/, float opacity)
{
    if (!Texture && !VideoSource) return;

    // A negative source height draws the texture flipped vertically (render textures are stored upside down).
    // A video's size is known on the game side from its headers; its texture exists only on the render side.
    Rectangle src = (SourceRect.width > 0.0f && SourceRect.height != 0.0f) ? SourceRect
        : VideoSource ? Rectangle{ 0.0f, 0.0f, static_cast<float>(VideoSource->Width()), static_cast<float>(VideoSource->Height()) }
        : Rectangle{ 0.0f, 0.0f, static_cast<float>(Texture->width), static_cast<float>(Texture->height) };

    Rectangle dst = ResolvedRect;
    const float srcH = std::fabs(src.height);
    const float whole = (IntegerScale && src.width > 0.0f && srcH > 0.0f)
        ? std::floor(std::min(dst.width / src.width, dst.height / srcH)) : 0.0f;
    if (whole >= 1.0f) {
        dst = { dst.x + (dst.width - src.width * whole) * 0.5f, dst.y + (dst.height - srcH * whole) * 0.5f,
                src.width * whole, srcH * whole };
    } else if ((PreserveAspect || IntegerScale) && src.height != 0.0f && dst.height > 0.0f) {
        float srcAspect = src.width / std::fabs(src.height);
        float dstAspect = dst.width / dst.height;
        if (srcAspect > dstAspect) {
            float h = dst.width / srcAspect;
            dst.y += (dst.height - h) * 0.5f;
            dst.height = h;
        } else {
            float w = dst.height * srcAspect;
            dst.x += (dst.width - w) * 0.5f;
            dst.width = w;
        }
    }

    if (VideoSource) {
        VideoSource->DrawRegion(src, dst, WithOpacity(Tint, opacity));
        return;
    }
    Record([tex = Texture, src, dst, tint = WithOpacity(Tint, opacity)]() {
        DrawTexturePro(*tex, src, dst, Vector2{ 0.0f, 0.0f }, 0.0f, tint);
    });
}

// UIButton
UIButton::UIButton(uint64_t id, const std::string& name, UICanvas* canvas)
    : UIElement(UIElementType::Button, id, name, canvas)
{
    Focusable = true;
}

void UIButton::EnsureFitted()
{
    Font f = (Canvas && Canvas->Theme().HasDefaultFont) ? Canvas->Theme().DefaultFont : GetFontDefault();
    const UIStyle& resolvedStyle = CustomStyle ? *CustomStyle : (Canvas ? Canvas->Theme().Resolve(StyleClass.empty() ? "button" : StyleClass) : UIStyle{});
    float fontSize = resolvedStyle.FontSize;
    Rectangle r = ResolvedRect;
    Rectangle box = {
        r.x + resolvedStyle.PaddingLeft,
        r.y + resolvedStyle.PaddingTop,
        std::max(0.0f, r.width - resolvedStyle.PaddingLeft - resolvedStyle.PaddingRight),
        std::max(0.0f, r.height - resolvedStyle.PaddingTop - resolvedStyle.PaddingBottom)
    };
    bool hasIcon = (IconTexture || !Icon.empty());
    if (hasIcon) {
        float iconBoxSize = std::max(12.0f, std::min(box.height - 8.0f, 24.0f));
        float gap = 8.0f;
        box.x += iconBoxSize + gap;
        box.width = std::max(0.0f, box.width - (iconBoxSize + gap));
    }
    FitText(f, ResolvedText(), box, fontSize, 1.0f, Fit(), Memo);
}

float UIButton::TextHeightAt(float width)
{
    Font f = (Canvas && Canvas->Theme().HasDefaultFont) ? Canvas->Theme().DefaultFont : GetFontDefault();
    const UIStyle& style = CustomStyle ? *CustomStyle : (Canvas ? Canvas->Theme().Resolve(StyleClass.empty() ? "button" : StyleClass) : UIStyle{});
    const float icon = (IconTexture || !Icon.empty()) ? 32.0f : 0.0f;   // EnsureFitted's icon box + gap, at most
    return TextHeightIn(f, ResolvedText(), style, style.FontSize, width - icon, Fit(), FitMemo);
}

uint64_t UIButton::TextLayoutKey() const
{
    const UIStyle& style = CustomStyle ? *CustomStyle : (Canvas ? Canvas->Theme().Resolve(StyleClass.empty() ? "button" : StyleClass) : UIStyle{});
    return TextKey(ResolvedText(), style.FontSize, Wrap, StyleClass);
}

float UIButton::FittedFontSize()
{
    EnsureFitted();
    return Memo.FittedFontSize;
}

float UIButton::MeasuredWidth()
{
    EnsureFitted();
    return Memo.MeasuredWidth;
}

float UIButton::MeasuredHeight()
{
    EnsureFitted();
    return Memo.MeasuredHeight;
}

void UIButton::DrawContent(const UIStyle& resolvedStyle, float opacity)
{
    Rectangle r = ResolvedRect;
    Font f = (Canvas && Canvas->Theme().HasDefaultFont) ? Canvas->Theme().DefaultFont : GetFontDefault();
    float fontSize = resolvedStyle.FontSize;
    Color textColor = WithOpacity(TransitionState.CurrentText, opacity);
    float spacing = 1.0f;

    bool hasIcon = (IconTexture || !Icon.empty());
    bool hasText = !ResolvedText().empty();

    Rectangle box = {
        r.x + resolvedStyle.PaddingLeft,
        r.y + resolvedStyle.PaddingTop,
        std::max(0.0f, r.width - resolvedStyle.PaddingLeft - resolvedStyle.PaddingRight),
        std::max(0.0f, r.height - resolvedStyle.PaddingTop - resolvedStyle.PaddingBottom)
    };

    if (hasIcon && hasText) {
        float iconBoxSize = std::max(12.0f, std::min(box.height - 8.0f, 24.0f));
        float gap = 8.0f;

        Rectangle iconRect = { box.x, box.y + (box.height - iconBoxSize) * 0.5f, iconBoxSize, iconBoxSize };
        if (IconTexture) {
            Record([tex = IconTexture, iconRect, tint = WithOpacity(WHITE, opacity)]() {
                DrawTexturePro(*tex,
                    Rectangle{ 0.0f, 0.0f, static_cast<float>(tex->width), static_cast<float>(tex->height) },
                    iconRect, Vector2{ 0.0f, 0.0f }, 0.0f, tint);
            });
        } else {
            DrawIcon(Icon, iconRect, textColor);
        }

        Rectangle textBox = {
            box.x + iconBoxSize + gap,
            box.y,
            std::max(0.0f, box.width - (iconBoxSize + gap)),
            box.height
        };
        DrawTextFitted(f, ResolvedText(), textBox, fontSize, spacing, textColor, UIHAlign::Center, UIVAlign::Middle, Fit(), &Memo, this);
    } else if (hasIcon) {
        float iconBoxSize = std::max(12.0f, std::min(box.height - 8.0f, box.width - 8.0f));
        Rectangle iconRect = { box.x + (box.width - iconBoxSize) * 0.5f, box.y + (box.height - iconBoxSize) * 0.5f, iconBoxSize, iconBoxSize };
        if (IconTexture) {
            Record([tex = IconTexture, iconRect, tint = WithOpacity(WHITE, opacity)]() {
                DrawTexturePro(*tex,
                    Rectangle{ 0.0f, 0.0f, static_cast<float>(tex->width), static_cast<float>(tex->height) },
                    iconRect, Vector2{ 0.0f, 0.0f }, 0.0f, tint);
            });
        } else {
            DrawIcon(Icon, iconRect, textColor);
        }
    } else if (hasText) {
        DrawTextFitted(f, ResolvedText(), box, fontSize, spacing, textColor, UIHAlign::Center, UIVAlign::Middle, Fit(), &Memo, this);
    }
}

// UIProgressBar
UIProgressBar::UIProgressBar(uint64_t id, const std::string& name, UICanvas* canvas)
    : UIElement(UIElementType::ProgressBar, id, name, canvas)
{
}

void UIProgressBar::DrawContent(const UIStyle& /*resolvedStyle*/, float opacity)
{
    float val = std::clamp(Value, 0.0f, 1.0f);
    if (val <= 0.0f) return;

    Rectangle r = ResolvedRect;
    Rectangle fillR = r;

    switch (FillDirection) {
        case UIFillDirection::LeftToRight:
            fillR.width = r.width * val;
            break;
        case UIFillDirection::RightToLeft:
            fillR.x = r.x + r.width * (1.0f - val);
            fillR.width = r.width * val;
            break;
        case UIFillDirection::TopToBottom:
            fillR.height = r.height * val;
            break;
        case UIFillDirection::BottomToTop:
            fillR.y = r.y + r.height * (1.0f - val);
            fillR.height = r.height * val;
            break;
    }

    const UIStyle& fillStyle = Canvas ? Canvas->Theme().Resolve("progress_fill") : UIStyle{};
    if (fillStyle.CornerRadius > 0.0f) {
        float roundness = std::clamp(fillStyle.CornerRadius / (std::min(fillR.width, fillR.height) * 0.5f + 1e-4f), 0.0f, 1.0f);
        Record([=, c0 = WithOpacity(fillStyle.BackgroundColor, opacity)]() { DrawRectangleRounded(fillR, roundness, 8, c0); });
    } else {
        Record([=, c0 = WithOpacity(fillStyle.BackgroundColor, opacity)]() { DrawRectangleRec(fillR, c0); });
    }
}

// UISlider
UISlider::UISlider(uint64_t id, const std::string& name, UICanvas* canvas)
    : UIElement(UIElementType::Slider, id, name, canvas)
{
    Focusable = true;
}

void UISlider::DrawContent(const UIStyle& /*resolvedStyle*/, float opacity)
{
    Rectangle r = ResolvedRect;
    float range = MaxValue - MinValue;
    float t = (range > 1e-6f) ? std::clamp((Value - MinValue) / range, 0.0f, 1.0f) : 0.0f;

    const UIStyle& fillStyle = Canvas ? Canvas->Theme().Resolve("slider_fill") : UIStyle{};
    const UIStyle& knobStyle = Canvas ? Canvas->Theme().Resolve("slider_knob") : UIStyle{};

    float knobSize = knobStyle.CornerRadius > 0.0f ? knobStyle.CornerRadius * 2.0f : (Vertical ? r.width + 4.0f : r.height + 4.0f);
    knobSize = std::max(12.0f, knobSize);

    Rectangle fillR = r;
    Rectangle knobR {};

    if (Vertical) {
        float knobY = r.y + (1.0f - t) * (r.height - knobSize);
        knobR = Rectangle{ r.x + (r.width - knobSize) * 0.5f, knobY, knobSize, knobSize };
        fillR = Rectangle{ r.x, knobY + knobSize * 0.5f, r.width, (r.y + r.height) - (knobY + knobSize * 0.5f) };
    } else {
        float knobX = r.x + t * (r.width - knobSize);
        knobR = Rectangle{ knobX, r.y + (r.height - knobSize) * 0.5f, knobSize, knobSize };
        fillR = Rectangle{ r.x, r.y, (knobX + knobSize * 0.5f) - r.x, r.height };
    }

    // Fill
    if (fillStyle.CornerRadius > 0.0f) {
        float roundness = std::clamp(fillStyle.CornerRadius / (std::min(fillR.width, fillR.height) * 0.5f + 1e-4f), 0.0f, 1.0f);
        Record([=, c0 = WithOpacity(fillStyle.BackgroundColor, opacity)]() { DrawRectangleRounded(fillR, roundness, 8, c0); });
    } else {
        Record([=, c0 = WithOpacity(fillStyle.BackgroundColor, opacity)]() { DrawRectangleRec(fillR, c0); });
    }

    // Knob
    Color knobBg = knobStyle.BackgroundColor;
    if (Pressed && knobStyle.Pressed.HasBackground) knobBg = knobStyle.Pressed.Background;
    else if (Hovered && knobStyle.Hover.HasBackground) knobBg = knobStyle.Hover.Background;

    if (knobStyle.Beveled) {
        Record([=, c0 = WithOpacity(knobBg, opacity)]() { DrawRectangleRec(knobR, c0); });
        Record([=, c0 = WithOpacity(knobStyle.BevelLight, opacity)]() { DrawLineEx(Vector2{ knobR.x, knobR.y }, Vector2{ knobR.x + knobR.width, knobR.y }, 1.5f, c0); });
        Record([=, c0 = WithOpacity(knobStyle.BevelLight, opacity)]() { DrawLineEx(Vector2{ knobR.x, knobR.y }, Vector2{ knobR.x, knobR.y + knobR.height }, 1.5f, c0); });
        Record([=, c0 = WithOpacity(knobStyle.BevelDark, opacity)]() { DrawLineEx(Vector2{ knobR.x, knobR.y + knobR.height }, Vector2{ knobR.x + knobR.width, knobR.y + knobR.height }, 1.5f, c0); });
        Record([=, c0 = WithOpacity(knobStyle.BevelDark, opacity)]() { DrawLineEx(Vector2{ knobR.x + knobR.width, knobR.y }, Vector2{ knobR.x + knobR.width, knobR.y + knobR.height }, 1.5f, c0); });
    } else if (knobStyle.CornerRadius > 0.0f) {
        float roundness = std::clamp(knobStyle.CornerRadius / (std::min(knobR.width, knobR.height) * 0.5f + 1e-4f), 0.0f, 1.0f);
        Record([=, c0 = WithOpacity(knobBg, opacity)]() { DrawRectangleRounded(knobR, roundness, 8, c0); });
        if (knobStyle.BorderWidth > 0.0f && knobStyle.BorderColor.a > 0) {
            Record([=, c0 = WithOpacity(knobStyle.BorderColor, opacity)]() { DrawRectangleRoundedLines(knobR, roundness, 8, c0); });
        }
    } else {
        Record([=, c0 = WithOpacity(knobBg, opacity)]() { DrawRectangleRec(knobR, c0); });
        if (knobStyle.BorderWidth > 0.0f && knobStyle.BorderColor.a > 0) {
            Record([knobR, bw = knobStyle.BorderWidth, c0 = WithOpacity(knobStyle.BorderColor, opacity)]() { DrawRectangleLinesEx(knobR, bw, c0); });
        }
    }
}

// UIScrollView
UIScrollView::UIScrollView(uint64_t id, const std::string& name, UICanvas* canvas)
    : UIElement(UIElementType::ScrollView, id, name, canvas)
{
    ClipChildren = true;
}

Rectangle UIScrollView::GetThumbRect() const
{
    Rectangle r = ResolvedRect;
    if (r.height > 0.0f && ContentHeight > r.height) {
        float barW = ScrollbarWidth;  // 2x default thickness, easier to see/grab
        float barX = r.x + r.width - barW;
        float visibleRatio = r.height / ContentHeight;
        float thumbH = std::max(12.0f, r.height * visibleRatio);
        float maxScroll = ContentHeight - r.height;
        float scrollRatio = maxScroll > 0.0f ? (ScrollOffset / maxScroll) : 0.0f;
        float thumbY = r.y + scrollRatio * (r.height - thumbH);
        return Rectangle{ barX, thumbY, barW, thumbH };
    }
    return Rectangle{ 0.0f, 0.0f, 0.0f, 0.0f };
}

void UIScrollView::DrawContent(const UIStyle& /*resolvedStyle*/, float opacity)
{
    Rectangle thumb = GetThumbRect();
    if (thumb.width > 0.0f && thumb.height > 0.0f) {
        // Darker, fully-opaque gray: the old light/translucent color blended
        // into classic_theme's white scroll-view background almost invisibly.
        Record([=, c0 = WithOpacity(Color{ 110, 110, 110, 255 }, opacity)]() { DrawRectangleRounded(thumb, 1.0f, 4, c0); });
    }
}

// UITextInput
UITextInput::UITextInput(uint64_t id, const std::string& name, UICanvas* canvas)
    : UIElement(UIElementType::TextInput, id, name, canvas)
{
    Focusable = true;
}

void UITextInput::DrawContent(const UIStyle& resolvedStyle, float opacity)
{
    Rectangle r = ResolvedRect;
    Font f = (Canvas && Canvas->Theme().HasDefaultFont) ? Canvas->Theme().DefaultFont : GetFontDefault();
    float fontSize = resolvedStyle.FontSize;
    float spacing = 1.0f;

    Rectangle box = {
        r.x + resolvedStyle.PaddingLeft,
        r.y + resolvedStyle.PaddingTop,
        std::max(0.0f, r.width - resolvedStyle.PaddingLeft - resolvedStyle.PaddingRight),
        std::max(0.0f, r.height - resolvedStyle.PaddingTop - resolvedStyle.PaddingBottom)
    };

    if (box.width <= 0.0f || box.height <= 0.0f) {
        return;
    }

    Color textColor = WithOpacity(TransitionState.CurrentText, opacity);

    if (!Text.empty()) {
        DrawTextFitted(f, Text, box, fontSize, spacing, textColor, UIHAlign::Left, UIVAlign::Middle, Fit(), &Memo, this);
    } else if (!Placeholder.empty()) {
        Color placeholderColor = WithOpacity(textColor, 0.5f);
        DrawTextFitted(f, Placeholder, box, fontSize, spacing, placeholderColor, UIHAlign::Left, UIVAlign::Middle, Fit(), &Memo);
    }

    // Blinking cursor: thin vertical bar drawn after text ONLY when focused
    if (Canvas && Canvas->GetFocused() == this && Enabled && opacity > 0.01f) {
        float curFontSize = Memo.Valid ? Memo.FittedFontSize : fontSize;
        if (curFontSize <= 0.0f) curFontSize = fontSize;
        float curH = std::min(curFontSize, box.height);
        float curY = box.y + (box.height - curH) * 0.5f;
        float textW = Text.empty() ? 0.0f : (Memo.Valid ? Memo.MeasuredWidth : 0.0f);
        float curX = std::clamp(box.x + textW + 1.0f, box.x, box.x + box.width - 1.5f);
        if (CursorVisible) Record([=]() { DrawRectangleRec(Rectangle{ curX, curY, 1.5f, curH }, textColor); });
        Vector2 top = s_currentTransform.Apply(Vector2{curX, curY});
        Vector2 bottom = s_currentTransform.Apply(Vector2{curX, curY + curH});
        top.x *= s_currentScaleFactor; top.y *= s_currentScaleFactor;
        bottom.x *= s_currentScaleFactor; bottom.y *= s_currentScaleFactor;
        top = WarpedScreenPoint(top); bottom = WarpedScreenPoint(bottom);
        ImeRequest(this, top.x, top.y, std::hypot(bottom.x - top.x, bottom.y - top.y),
                   static_cast<float>(GameInput().ScreenWidth), static_cast<float>(GameInput().ScreenHeight));
    }
}

// Both per-frame passes (state transitions and drawing) resolve every element's style by
// class name. Returning a reference keeps them from allocating a name string per element
// per frame; an explicit StyleClass wins, otherwise the widget type's default class.
static const std::string& StyleClassOf(const UIElement* element)
{
    static const std::string kPanel = "panel", kText = "text", kImage = "image",
        kButton = "button", kProgress = "progress", kSlider = "slider",
        kScrollView = "scroll_view", kTextInput = "text_input", kNone;
    if (!element->StyleClass.empty()) return element->StyleClass;
    switch (element->Type) {
        case UIElementType::Panel: return kPanel;
        case UIElementType::Text: return kText;
        case UIElementType::Image: return kImage;
        case UIElementType::Button: return kButton;
        case UIElementType::ProgressBar: return kProgress;
        case UIElementType::Slider: return kSlider;
        case UIElementType::ScrollView: return kScrollView;
        case UIElementType::TextInput: return kTextInput;
        default: return kNone;
    }
}

void UICanvas::UpdateStateTransitions(UIElement* element, float dt)
{
    if (!element) return;

    const std::string& className = StyleClassOf(element);
    const UIStyle& baseStyle = element->CustomStyle ? *element->CustomStyle : m_theme.Resolve(className);

    Color targetBg = baseStyle.BackgroundColor;
    Color targetBorder = baseStyle.BorderColor;
    Color targetText = baseStyle.TextColor;
    float targetScaleMul = 1.0f;

    bool isHovered = false;
    bool isPressed = false;
    if (element->Type == UIElementType::Button) {
        UIButton* b = static_cast<UIButton*>(element);
        isHovered = b->Hovered;
        isPressed = b->Pressed;
    } else if (element->Type == UIElementType::Slider) {
        UISlider* s = static_cast<UISlider*>(element);
        isHovered = s->Hovered;
        isPressed = s->Pressed;
    } else if (element->Type == UIElementType::TextInput) {
        UITextInput* ti = static_cast<UITextInput*>(element);
        isHovered = ti->Hovered;
        isPressed = ti->Pressed;
    }

    if (!element->Enabled) {
        if (baseStyle.Disabled.HasBackground) targetBg = baseStyle.Disabled.Background;
        if (baseStyle.Disabled.HasBorder)     targetBorder = baseStyle.Disabled.Border;
        if (baseStyle.Disabled.HasText)       targetText = baseStyle.Disabled.Text;
        targetScaleMul = baseStyle.Disabled.ScaleMul;
    } else if (isPressed) {
        if (baseStyle.Pressed.HasBackground) targetBg = baseStyle.Pressed.Background;
        if (baseStyle.Pressed.HasBorder)     targetBorder = baseStyle.Pressed.Border;
        if (baseStyle.Pressed.HasText)       targetText = baseStyle.Pressed.Text;
        targetScaleMul = baseStyle.Pressed.ScaleMul;
    } else if (isHovered) {
        if (baseStyle.Hover.HasBackground) targetBg = baseStyle.Hover.Background;
        if (baseStyle.Hover.HasBorder)     targetBorder = baseStyle.Hover.Border;
        if (baseStyle.Hover.HasText)       targetText = baseStyle.Hover.Text;
        targetScaleMul = baseStyle.Hover.ScaleMul;
    }

    auto& ts = element->TransitionState;
    if (!ts.Initialized || baseStyle.TransitionDuration <= 0.0f) {
        ts.CurrentBg = targetBg;
        ts.CurrentBorder = targetBorder;
        ts.CurrentText = targetText;
        ts.CurrentScaleMul = targetScaleMul;
        ts.Initialized = true;
    } else {
        // Exponential approach: the same settle time at any frame rate (a dt/duration lerp settles
        // slower at high fps and snaps on a hitch).
        float t = 1.0f - std::exp(-std::max(dt, 0.0f) / baseStyle.TransitionDuration);
        auto lerpC = [](Color c1, Color c2, float factor) -> Color {
            return Color{
                static_cast<unsigned char>(std::round(static_cast<float>(c1.r) + static_cast<float>(c2.r - c1.r) * factor)),
                static_cast<unsigned char>(std::round(static_cast<float>(c1.g) + static_cast<float>(c2.g - c1.g) * factor)),
                static_cast<unsigned char>(std::round(static_cast<float>(c1.b) + static_cast<float>(c2.b - c1.b) * factor)),
                static_cast<unsigned char>(std::round(static_cast<float>(c1.a) + static_cast<float>(c2.a - c1.a) * factor))
            };
        };
        ts.CurrentBg = lerpC(ts.CurrentBg, targetBg, t);
        ts.CurrentBorder = lerpC(ts.CurrentBorder, targetBorder, t);
        ts.CurrentText = lerpC(ts.CurrentText, targetText, t);
        ts.CurrentScaleMul = ts.CurrentScaleMul + (targetScaleMul - ts.CurrentScaleMul) * t;
    }

    for (size_t i = 0; i < element->Children.size(); ++i) {
        UpdateStateTransitions(element->Children[i].get(), dt);
    }
}

void UICanvas::DrawElement(UIElement* element)
{
    if (!element || !element->Visible || element->ResolvedOpacity <= 0.001f) {
        return;
    }

    const std::string& className = StyleClassOf(element);
    const UIStyle& style = element->CustomStyle ? *element->CustomStyle : m_theme.Resolve(className);

    float opacity = element->ResolvedOpacity;
    Rectangle r = element->ResolvedRect;

    float pivotX = r.x + element->Rect.Pivot.x * r.width;
    float pivotY = r.y + element->Rect.Pivot.y * r.height;

    float scaleMul = element->TransitionState.CurrentScaleMul;
    Vector2 effScale = { element->Rect.Scale.x * scaleMul, element->Rect.Scale.y * scaleMul };

    // Most elements are neither scaled nor rotated: they need no matrix of their own.
    const float rotation = element->Rect.Rotation;
    const Vector2 shift = element->Rect.Translation;
    const bool transformed = effScale.x != 1.0f || effScale.y != 1.0f || rotation != 0.0f
        || shift.x != 0.0f || shift.y != 0.0f;
    UITransform2D prevTransform = s_currentTransform;
    if (transformed) {
        RecordState([=]() {
            rlPushMatrix();
            rlTranslatef(pivotX + shift.x, pivotY + shift.y, 0.0f);
            rlRotatef(rotation, 0.0f, 0.0f, 1.0f);
            rlScalef(effScale.x, effScale.y, 1.0f);
            rlTranslatef(-pivotX, -pivotY, 0.0f);
        });
        UITransform2D local = UITransform2D::ScaleRotateAround(Vector2{ pivotX, pivotY }, effScale, rotation);
        local.tx += shift.x;
        local.ty += shift.y;
        s_currentTransform = s_currentTransform.Compose(local);
    }

    float sf = ScaleFactor();
    // Outside the clip (a scrolled-away row, a far card of a panned tree) the element draws nothing,
    // and its children neither when it clips them to itself. A video is still drawn: drawing drives it.
    float pad = 16.0f + style.ShadowSize + std::max(std::fabs(style.ShadowOffset.x), std::fabs(style.ShadowOffset.y));
    if (element->Type == UIElementType::Slider) pad += std::max(r.height, 2.0f * style.CornerRadius);   // its knob outgrows a thin track
    const bool culled = element->Type != UIElementType::Root && OutsideClip(r, pad, sf)
        && !(element->Type == UIElementType::Image && static_cast<UIImage*>(element)->VideoSource);
    if (culled && element->ClipChildren) {
        s_currentTransform = prevTransform;
        if (transformed) RecordState([]() { rlPopMatrix(); });
        return;
    }

    if (element->Type != UIElementType::Root && !culled) {
        // Shadow
        if (style.ShadowColor.a > 0 && (style.ShadowSize > 0.0f || style.ShadowOffset.x != 0.0f || style.ShadowOffset.y != 0.0f)) {
            Rectangle shadowRect = {
                r.x + style.ShadowOffset.x - style.ShadowSize * 0.5f,
                r.y + style.ShadowOffset.y - style.ShadowSize * 0.5f,
                r.width + style.ShadowSize,
                r.height + style.ShadowSize
            };
            if (style.CornerRadius > 0.0f) {
                float roundness = std::clamp(style.CornerRadius / (std::min(shadowRect.width, shadowRect.height) * 0.5f + 1e-4f), 0.0f, 1.0f);
                Record([=, c0 = WithOpacity(style.ShadowColor, opacity)]() { DrawRectangleRounded(shadowRect, roundness, 8, c0); });
            } else {
                Record([=, c0 = WithOpacity(style.ShadowColor, opacity)]() { DrawRectangleRec(shadowRect, c0); });
            }
        }

        // Background
        Color bgCol = element->TransitionState.CurrentBg;
        if (style.UseNPatch && style.NPatchTexture) {
            NPatchInfo npatch;
            npatch.source = Rectangle{ 0.0f, 0.0f, static_cast<float>(style.NPatchTexture->width), static_cast<float>(style.NPatchTexture->height) };
            npatch.left = style.NPatchLeft;
            npatch.top = style.NPatchTop;
            npatch.right = style.NPatchRight;
            npatch.bottom = style.NPatchBottom;
            npatch.layout = NPATCH_NINE_PATCH;
            Record([tex = style.NPatchTexture, npatch, r, c0 = WithOpacity(bgCol, opacity)]() { DrawTextureNPatch(*tex, npatch, r, Vector2{ 0.0f, 0.0f }, 0.0f, c0); });
        } else if (style.Beveled) {
            Record([=, c0 = WithOpacity(bgCol, opacity)]() { DrawRectangleRec(r, c0); });

            bool isPressed = (element->Type == UIElementType::Button && static_cast<UIButton*>(element)->Pressed) ||
                             (element->Type == UIElementType::TextInput && static_cast<UITextInput*>(element)->Pressed);
            bool sunken = style.BevelSunken || isPressed;
            Color light = sunken ? style.BevelDark : style.BevelLight;
            Color dark = sunken ? style.BevelLight : style.BevelDark;

            Record([=, c0 = WithOpacity(light, opacity)]() { DrawLineEx(Vector2{ r.x, r.y }, Vector2{ r.x + r.width, r.y }, 1.5f, c0); });
            Record([=, c0 = WithOpacity(light, opacity)]() { DrawLineEx(Vector2{ r.x, r.y }, Vector2{ r.x, r.y + r.height }, 1.5f, c0); });
            Record([=, c0 = WithOpacity(dark, opacity)]() { DrawLineEx(Vector2{ r.x, r.y + r.height - 1.0f }, Vector2{ r.x + r.width, r.y + r.height - 1.0f }, 1.5f, c0); });
            Record([=, c0 = WithOpacity(dark, opacity)]() { DrawLineEx(Vector2{ r.x + r.width - 1.0f, r.y }, Vector2{ r.x + r.width - 1.0f, r.y + r.height }, 1.5f, c0); });
        } else if (style.CornerRadius > 0.0f && bgCol.a > 0) {
            float roundness = std::clamp(style.CornerRadius / (std::min(r.width, r.height) * 0.5f + 1e-4f), 0.0f, 1.0f);
            Record([=, c0 = WithOpacity(bgCol, opacity)]() { DrawRectangleRounded(r, roundness, 8, c0); });
        } else if (bgCol.a > 0) {
            Record([=, c0 = WithOpacity(bgCol, opacity)]() { DrawRectangleRec(r, c0); });
        }

        // Border
        Color borderCol = element->TransitionState.CurrentBorder;
        if (!style.Beveled && style.BorderWidth > 0.0f && borderCol.a > 0) {
            if (style.CornerRadius > 0.0f) {
                float roundness = std::clamp(style.CornerRadius / (std::min(r.width, r.height) * 0.5f + 1e-4f), 0.0f, 1.0f);
                Record([=, c0 = WithOpacity(borderCol, opacity)]() { DrawRectangleRoundedLines(r, roundness, 8, c0); });
            } else {
                // Centred on the edge, not inside it: two panels that touch share one line instead of a double one.
                const float bw = style.BorderWidth;
                const Rectangle edge{ r.x - bw * 0.5f, r.y - bw * 0.5f, r.width + bw, r.height + bw };
                Record([edge, bw, c0 = WithOpacity(borderCol, opacity)]() { DrawRectangleLinesEx(edge, bw, c0); });
            }
        }

        // Focus ring
        if (m_focusedElement == element) {
            Color focusRingCol = Color{ 100, 180, 255, 200 };
            Rectangle focusR = { r.x - 2.0f, r.y - 2.0f, r.width + 4.0f, r.height + 4.0f };
            if (style.CornerRadius > 0.0f) {
                float roundness = std::clamp((style.CornerRadius + 2.0f) / (std::min(focusR.width, focusR.height) * 0.5f + 1e-4f), 0.0f, 1.0f);
                Record([=, c0 = WithOpacity(focusRingCol, opacity)]() { DrawRectangleRoundedLines(focusR, roundness, 8, c0); });
            } else {
                Record([=, c0 = WithOpacity(focusRingCol, opacity)]() { DrawRectangleLinesEx(focusR, 2.0f, c0); });
            }
        }

        if (element->Type != UIElementType::ScrollView) {
            element->DrawContent(style, opacity);
        }
    }

    if (element->ClipChildren) {
        PushScissor(r, sf);
    }

    for (size_t i = 0; i < element->Children.size(); ++i) {
        DrawElement(element->Children[i].get());
    }

    if (element->ClipChildren) {
        PopScissor();
    }

    if (element->Type == UIElementType::ScrollView && !culled) {
        element->DrawContent(style, opacity);
    }

    s_currentTransform = prevTransform;
    if (transformed) RecordState([]() { rlPopMatrix(); });
}

void UICanvas::DrawDebugOverlay(UIElement* element, int depth)
{
    if (!element || !element->Visible) return;

    static const Color debugColors[] = {
        RED, GREEN, BLUE, YELLOW, ORANGE, PURPLE, SKYBLUE, LIME
    };
    Color col = debugColors[depth % 8];

    if (element->Type != UIElementType::Root) {
        Rectangle rr = element->ResolvedRect;
        std::string label = element->Name + " (" + element->TypeName() + ")";
        Record([rr, col, label = std::move(label)]() {
            DrawRectangleLinesEx(rr, 1.0f, col);
            DrawText(label.c_str(), static_cast<int>(rr.x + 2.0f), static_cast<int>(rr.y + 2.0f), 10, col);
        });
    }

    for (size_t i = 0; i < element->Children.size(); ++i) {
        DrawDebugOverlay(element->Children[i].get(), depth + 1);
    }
    if (depth != 0) return;
    for (const Problem& p : Problems()) {   // what the visual tests would flag, in red with its kind
        Rectangle rr = p.Element->ResolvedRect;
        std::string label = p.Kind + ": " + p.Detail;
        Record([rr, label = std::move(label)]() {
            DrawRectangleLinesEx(rr, 3.0f, RED);
            DrawRectangle(static_cast<int>(rr.x), static_cast<int>(rr.y + rr.height), MeasureText(label.c_str(), 10) + 4, 12, Color{ 0, 0, 0, 200 });
            DrawText(label.c_str(), static_cast<int>(rr.x + 2.0f), static_cast<int>(rr.y + rr.height + 1.0f), 10, RED);
        });
    }
}

std::string UICanvas::MissingGlyphs(const UIElement& e, const std::string& text) const
{
    const Font* font = e.Type == UIElementType::Text ? static_cast<const UIText&>(e).FontOverride : nullptr;
    if (!font) font = m_theme.HasDefaultFont ? &m_theme.DefaultFont : nullptr;
    const Font fallback = GetFontDefault();
    const Font& view = *CurrentApp().GetAssets().FontForText(font ? *font : fallback, text);
    std::string missing;
    for (size_t i = 0; i < text.size();) {
        int size = 0;
        const int cp = GetCodepointNext(text.c_str() + i, &size);
        const std::string ch = text.substr(i, std::max(1, size));
        i += std::max(1, size);
        if (cp <= 32 || missing.find(ch) != std::string::npos) continue;
        bool found = false;
        for (int g = 0; g < view.glyphCount && !found; ++g) {
            found = view.glyphs[g].value == cp && (view.glyphs[g].advanceX > 0 || view.recs[g].width > 0);
        }
        if (!found) missing += ch;
    }
    return missing;
}

static std::string Num(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

std::vector<UICanvas::Problem> UICanvas::Problems()
{
    ResolveLayoutTree();
    std::vector<Problem> out;
    struct Hit { UIElement* E; Rectangle R; int Order; };
    std::vector<Hit> buttons, blockers;   // the part a click reaches, in draw order; blocks_input_behind panels
    std::vector<Hit> inks;                // each text's drawn glyphs (TEXT_OVERLAP)
    int order = 0;
    const float cw = LogicalWidth(), ch = LogicalHeight();
    auto walk = [&](UIElement* e, auto&& self) -> void {
        if (!e->Visible) return;
        const int myOrder = order++;
        if (e->Type == UIElementType::Panel && static_cast<UIPanel*>(e)->BlocksInputBehind)
            blockers.push_back({ e, e->ResolvedRect, myOrder });
        for (auto& c : e->Children) self(c.get(), self);
        if (e->Type == UIElementType::Root) return;
        const Rectangle r = e->ResolvedRect;
        if (r.width <= 0.0f || r.height <= 0.0f) {
            // An empty panel (a row with nothing shown yet) draws nothing: only flag what has content.
            const bool empty = e->Type == UIElementType::Panel &&
                std::none_of(e->Children.begin(), e->Children.end(), [](const auto& c) { return c->Visible; });
            if (!empty) out.push_back({ "ZERO_SIZE", e, "size " + Num(r.width) + " x " + Num(r.height) });
            return;
        }
        Rectangle hit = r;
        bool clipped = false;
        for (const UIElement* up = e->Parent; up; up = up->Parent) {
            if (up->Type != UIElementType::ScrollView && !up->ClipChildren) continue;
            clipped = true;
            const Rectangle c = up->ResolvedRect;
            const float x0 = std::max(hit.x, c.x), y0 = std::max(hit.y, c.y);
            hit = { x0, y0, std::min(hit.x + hit.width, c.x + c.width) - x0, std::min(hit.y + hit.height, c.y + c.height) - y0 };
        }
        if (!clipped && (r.x + r.width <= 0.0f || r.y + r.height <= 0.0f || r.x >= cw || r.y >= ch))
            out.push_back({ "OFFSCREEN", e, "entirely outside the canvas " + Num(cw) + " x " + Num(ch) });
        if (!clipped && e->Parent && e->Parent->Type != UIElementType::Root) {
            const Rectangle p = e->Parent->ResolvedRect;
            const float over = std::max({ p.x - r.x, p.y - r.y, r.x + r.width - p.x - p.width, r.y + r.height - p.y - p.height });
            if (over > 0.5f) out.push_back({ "OVERFLOW", e, "extends " + Num(over) + " past parent '" + e->Parent->Name + "'" });
        }
        if (e->Type != UIElementType::Text && e->Type != UIElementType::Button) return;
        const bool isText = e->Type == UIElementType::Text;
        if (!isText && e->Enabled && hit.width > 1.0f && hit.height > 1.0f) buttons.push_back({ e, hit, myOrder });
        UITextMemo& memo = isText ? static_cast<UIText*>(e)->Memo : static_cast<UIButton*>(e)->Memo;
        if (isText) static_cast<UIText*>(e)->EnsureFitted(); else static_cast<UIButton*>(e)->EnsureFitted();
        if (memo.MeasuredWidth > r.width + 0.5f)
            out.push_back({ "TEXT_CLIP", e, "text " + Num(memo.MeasuredWidth) + " wide in a " + Num(r.width) + " box" });
        const float wanted = std::floor(memo.LastRequestedFontSize);   // as snapped for a pixel font
        if (memo.FittedFontSize < wanted - 0.01f)
            out.push_back({ "FONT_SHRUNK", e, "font " + Num(memo.FittedFontSize) + " below requested " + Num(wanted) });
        const std::string text = isText ? static_cast<UIText*>(e)->ResolvedText() : static_cast<UIButton*>(e)->ResolvedText();
        const std::string missing = text.empty() ? std::string() : MissingGlyphs(*e, text);
        if (!missing.empty()) out.push_back({ "UNSUPPORTED_GLYPH", e, "no glyph for '" + missing + "'" });
        if (isText && !text.empty() && e->Opacity > 0.0f && memo.MeasuredWidth > 0.0f) {
            const auto* t = static_cast<UIText*>(e);
            const UIStyle& st = t->CustomStyle ? *t->CustomStyle : Theme().Resolve(t->StyleClass.empty() ? "text" : t->StyleClass);
            const Rectangle box = { r.x + st.PaddingLeft, r.y + st.PaddingTop, std::max(0.0f, r.width - st.PaddingLeft - st.PaddingRight),
                                    std::max(0.0f, r.height - st.PaddingTop - st.PaddingBottom) };
            const float w = std::min(memo.MeasuredWidth, box.width), h = std::min(memo.MeasuredHeight, box.height);
            const float x = box.x + (t->HAlign == UIHAlign::Left ? 0.0f : t->HAlign == UIHAlign::Center ? (box.width - w) * 0.5f : box.width - w);
            const float y = box.y + (t->VAlign == UIVAlign::Top ? 0.0f : t->VAlign == UIVAlign::Bottom ? box.height - h : (box.height - h) * 0.5f);
            inks.push_back({ e, { x, y, w, h }, myOrder });
        }
    };
    walk(m_root.get(), walk);
    // Two texts of one parent whose glyphs cross (a long label running into its value): both are unreadable.
    for (size_t i = 0; i < inks.size(); ++i)
        for (size_t j = i + 1; j < inks.size(); ++j) {
            if (inks[i].E->Parent != inks[j].E->Parent) continue;
            const Rectangle a = inks[i].R, b = inks[j].R;
            const float ox = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
            const float oy = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
            if (ox > 1.0f && oy > 1.0f)
                out.push_back({ "TEXT_OVERLAP", inks[j].E, "text crosses '" + inks[i].E->Name + "' by " + Num(ox) + " px" });
        }
    // Two enabled buttons sharing clickable pixels: a player cannot tell which one a click hits.
    auto isAncestor = [](const UIElement* a, const UIElement* b) {
        for (const UIElement* up = b->Parent; up; up = up->Parent) if (up == a) return true;
        return false;
    };
    // A button under a blocks_input_behind panel drawn after it (a modal's backdrop) cannot be clicked.
    std::erase_if(buttons, [&](const Hit& h) {
        const Vector2 c = { h.R.x + h.R.width * 0.5f, h.R.y + h.R.height * 0.5f };
        return std::any_of(blockers.begin(), blockers.end(), [&](const Hit& b) { return b.Order > h.Order && CheckCollisionPointRec(c, b.R); });
    });
    for (size_t i = 0; i < buttons.size(); ++i) {
        for (size_t j = i + 1; j < buttons.size(); ++j) {
            const Rectangle a = buttons[i].R, b = buttons[j].R;
            const float ix = std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
            const float iy = std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
            if (ix > 1.0f && iy > 1.0f && !isAncestor(buttons[i].E, buttons[j].E) && !isAncestor(buttons[j].E, buttons[i].E))
                out.push_back({ "BUTTON_OVERLAP", buttons[i].E, "overlaps button '" + buttons[j].E->Name + "' by "
                                + Num(ix) + " x " + Num(iy) });
        }
    }
    return out;
}

float UICanvas::FontGroupSize(const std::string& group, float own)
{
    auto now = m_fontGroupsNow.try_emplace(group, own).first;
    now->second = std::min(now->second, own);
    auto last = m_fontGroupsLast.find(group);
    return last == m_fontGroupsLast.end() ? own : std::min(own, last->second);
}

void UICanvas::NextFontGroupFrame()
{
    m_fontGroupsLast.swap(m_fontGroupsNow);
    m_fontGroupsNow.clear();
}

void UICanvas::Drawing()
{
    ProfileBegin("ui");
    // Update() laid the tree out before the Python Update hook ran; lay it out again so whatever the
    // hook changed (position, size, opacity, a new element) draws this frame, in step with what draw
    // time reads directly (visible, scale, text). Unchanged trees restore from the layout cache (§20).
    ResolveLayoutTree();
    NextFontGroupFrame();
    float sf = ScaleFactor();
    s_currentScaleFactor = sf;
    s_currentTransform = UITransform2D::Identity();

    Record([sf]() {
        rlPushMatrix();
        rlScalef(sf, sf, 1.0f);
    });

    DrawElement(m_root.get());

    if (DebugDraw) DrawDebugOverlay(m_root.get(), 0);

    Record([]() { rlPopMatrix(); });
    assert(s_scissorStack.empty());
    ProfileEnd();
}

} // namespace IYI::UI
