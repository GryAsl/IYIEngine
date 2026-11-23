// Implements procedural 2D icon rendering using raylib drawing primitives.
// Renders scalable geometric vector icons cleanly into a specified destination rectangle.

#include "IYI/IYIUI/IYIUI.h"
#include "IYI/Log.h"
#include "IYI/FramePacket.h"
#include <unordered_set>
#include <algorithm>
#include <cmath>

namespace IYI::UI {

// Draw calls are recorded with Record() (FramePacket.h) and replayed on the render side.

const std::vector<std::string>& GetIconNames()
{
    static const std::vector<std::string> s_names = {
        "blank", "play", "pause", "stop", "close", "check", "checkbox", "checkbox_checked", "plus", "minus",
        "arrow_left", "arrow_right", "arrow_up", "arrow_down",
        "chevron_left", "chevron_right", "chevron_up", "chevron_down",
        "home", "menu", "search", "settings", "volume", "mute", "info", "warning",
        "sun", "moon"
    };
    return s_names;
}

void DrawIcon(const std::string& name, Rectangle area, Color color)
{
    if (area.width <= 0.0f || area.height <= 0.0f || color.a == 0) {
        return;
    }

    float pad = std::min(area.width, area.height) * 0.15f;
    float s = std::max(1.0f, std::min(area.width, area.height) - 2.0f * pad);
    float cx = area.x + area.width * 0.5f;
    float cy = area.y + area.height * 0.5f;

    float left   = cx - s * 0.5f;
    float right  = cx + s * 0.5f;
    float top    = cy - s * 0.5f;
    float bottom = cy + s * 0.5f;
    float thick  = std::max(1.5f, s * 0.12f);

    if (name == "play") {
        Record([=]() { DrawTriangle(Vector2{ left, top }, Vector2{ left, bottom }, Vector2{ right, cy }, color); });
    } else if (name == "pause") {
        float barW = s * 0.28f;
        Record([=]() { DrawRectangleRec(Rectangle{ left, top, barW, s }, color); });
        Record([=]() { DrawRectangleRec(Rectangle{ right - barW, top, barW, s }, color); });
    } else if (name == "stop") {
        Record([=]() { DrawRectangleRec(Rectangle{ left, top, s, s }, color); });
    } else if (name == "close") {
        Record([=]() { DrawLineEx(Vector2{ left, top }, Vector2{ right, bottom }, thick, color); });
        Record([=]() { DrawLineEx(Vector2{ left, bottom }, Vector2{ right, top }, thick, color); });
    } else if (name == "check") {
        Vector2 p1 = { left, cy };
        Vector2 p2 = { left + s * 0.35f, bottom - s * 0.1f };
        Vector2 p3 = { right, top + s * 0.1f };
        Record([=]() { DrawLineEx(p1, p2, thick, color); });
        Record([=]() { DrawLineEx(p2, p3, thick, color); });
    } else if (name == "checkbox" || name == "checkbox_checked") {
        // Square outline with the same check as "check" inside it, for Unity-style toggle rows.
        Record([=]() { DrawRectangleLinesEx(Rectangle{ left, top, s, s }, std::max(1.0f, thick * 0.75f), color); });
        if (name == "checkbox_checked") {
            float inset = s * 0.22f;
            Vector2 p1 = { left + inset, cy };
            Vector2 p2 = { left + s * 0.42f, bottom - inset };
            Vector2 p3 = { right - inset, top + inset };
            Record([=]() { DrawLineEx(p1, p2, thick, color); });
            Record([=]() { DrawLineEx(p2, p3, thick, color); });
        }
    } else if (name == "plus") {
        Record([=]() { DrawLineEx(Vector2{ left, cy }, Vector2{ right, cy }, thick, color); });
        Record([=]() { DrawLineEx(Vector2{ cx, top }, Vector2{ cx, bottom }, thick, color); });
    } else if (name == "minus") {
        Record([=]() { DrawLineEx(Vector2{ left, cy }, Vector2{ right, cy }, thick, color); });
    } else if (name == "arrow_left") {
        Record([=]() { DrawLineEx(Vector2{ left, cy }, Vector2{ right, cy }, thick, color); });
        Record([=]() { DrawLineEx(Vector2{ left, cy }, Vector2{ left + s * 0.4f, top + s * 0.1f }, thick, color); });
        Record([=]() { DrawLineEx(Vector2{ left, cy }, Vector2{ left + s * 0.4f, bottom - s * 0.1f }, thick, color); });
    } else if (name == "arrow_right") {
        Record([=]() { DrawLineEx(Vector2{ left, cy }, Vector2{ right, cy }, thick, color); });
        Record([=]() { DrawLineEx(Vector2{ right, cy }, Vector2{ right - s * 0.4f, top + s * 0.1f }, thick, color); });
        Record([=]() { DrawLineEx(Vector2{ right, cy }, Vector2{ right - s * 0.4f, bottom - s * 0.1f }, thick, color); });
    } else if (name == "arrow_up") {
        Record([=]() { DrawLineEx(Vector2{ cx, bottom }, Vector2{ cx, top }, thick, color); });
        Record([=]() { DrawLineEx(Vector2{ cx, top }, Vector2{ left + s * 0.1f, top + s * 0.4f }, thick, color); });
        Record([=]() { DrawLineEx(Vector2{ cx, top }, Vector2{ right - s * 0.1f, top + s * 0.4f }, thick, color); });
    } else if (name == "arrow_down") {
        Record([=]() { DrawLineEx(Vector2{ cx, top }, Vector2{ cx, bottom }, thick, color); });
        Record([=]() { DrawLineEx(Vector2{ cx, bottom }, Vector2{ left + s * 0.1f, bottom - s * 0.4f }, thick, color); });
        Record([=]() { DrawLineEx(Vector2{ cx, bottom }, Vector2{ right - s * 0.1f, bottom - s * 0.4f }, thick, color); });
    } else if (name == "chevron_left") {
        Record([=]() { DrawLineEx(Vector2{ right - s * 0.15f, top }, Vector2{ left + s * 0.15f, cy }, thick, color); });
        Record([=]() { DrawLineEx(Vector2{ left + s * 0.15f, cy }, Vector2{ right - s * 0.15f, bottom }, thick, color); });
    } else if (name == "chevron_right") {
        Record([=]() { DrawLineEx(Vector2{ left + s * 0.15f, top }, Vector2{ right - s * 0.15f, cy }, thick, color); });
        Record([=]() { DrawLineEx(Vector2{ right - s * 0.15f, cy }, Vector2{ left + s * 0.15f, bottom }, thick, color); });
    } else if (name == "chevron_up") {
        Record([=]() { DrawLineEx(Vector2{ left, bottom - s * 0.15f }, Vector2{ cx, top + s * 0.15f }, thick, color); });
        Record([=]() { DrawLineEx(Vector2{ cx, top + s * 0.15f }, Vector2{ right, bottom - s * 0.15f }, thick, color); });
    } else if (name == "chevron_down") {
        Record([=]() { DrawLineEx(Vector2{ left, top + s * 0.15f }, Vector2{ cx, bottom - s * 0.15f }, thick, color); });
        Record([=]() { DrawLineEx(Vector2{ cx, bottom - s * 0.15f }, Vector2{ right, top + s * 0.15f }, thick, color); });
    } else if (name == "home") {
        Record([=]() { DrawTriangle(Vector2{ left, cy }, Vector2{ right, cy }, Vector2{ cx, top }, color); });
        Record([=]() { DrawRectangleRec(Rectangle{ left + s * 0.15f, cy, s * 0.7f, s * 0.5f }, color); });
    } else if (name == "menu") {
        float lineH = std::max(1.5f, s * 0.15f);
        Record([=]() { DrawRectangleRec(Rectangle{ left, top + s * 0.05f, s, lineH }, color); });
        Record([=]() { DrawRectangleRec(Rectangle{ left, cy - lineH * 0.5f, s, lineH }, color); });
        Record([=]() { DrawRectangleRec(Rectangle{ left, bottom - s * 0.05f - lineH, s, lineH }, color); });
    } else if (name == "search") {
        float r = s * 0.28f;
        Vector2 lensCenter = { left + r + s * 0.05f, top + r + s * 0.05f };
        Record([=]() { DrawCircleLines(static_cast<int>(lensCenter.x), static_cast<int>(lensCenter.y), r, color); });
        Record([=]() { DrawLineEx(Vector2{ lensCenter.x + r * 0.707f, lensCenter.y + r * 0.707f }, Vector2{ right, bottom }, thick * 1.2f, color); });
    } else if (name == "settings") {
        float rOuter = s * 0.42f;
        float rInner = s * 0.22f;
        Record([=]() { DrawCircleLines(static_cast<int>(cx), static_cast<int>(cy), rOuter, color); });
        Record([=]() { DrawCircleLines(static_cast<int>(cx), static_cast<int>(cy), rInner, color); });
        for (int i = 0; i < 4; ++i) {
            float angle = i * 45.0f * (3.14159265f / 180.0f);
            float cosA = std::cos(angle);
            float sinA = std::sin(angle);
            Record([=]() { DrawLineEx(Vector2{ cx - cosA * (rOuter + s * 0.08f), cy - sinA * (rOuter + s * 0.08f) },
                       Vector2{ cx + cosA * (rOuter + s * 0.08f), cy + sinA * (rOuter + s * 0.08f) },
                       thick, color); });
        }
    } else if (name == "volume") {
        float spkR = s * 0.22f;
        Record([=]() { DrawRectangleRec(Rectangle{ left, cy - spkR * 0.5f, spkR, spkR }, color); });
        Record([=]() { DrawTriangle(Vector2{ left + spkR, cy - spkR * 0.5f }, Vector2{ left + spkR, cy + spkR * 0.5f }, Vector2{ left + s * 0.55f, top }, color); });
        Record([=]() { DrawTriangle(Vector2{ left + spkR, cy + spkR * 0.5f }, Vector2{ left + s * 0.55f, bottom }, Vector2{ left + s * 0.55f, top }, color); });
        Record([=]() { DrawLineEx(Vector2{ cx + s * 0.15f, cy - s * 0.25f }, Vector2{ cx + s * 0.3f, cy }, thick, color); });
        Record([=]() { DrawLineEx(Vector2{ cx + s * 0.3f, cy }, Vector2{ cx + s * 0.15f, cy + s * 0.25f }, thick, color); });
    } else if (name == "mute") {
        float spkR = s * 0.22f;
        Record([=]() { DrawRectangleRec(Rectangle{ left, cy - spkR * 0.5f, spkR, spkR }, color); });
        Record([=]() { DrawTriangle(Vector2{ left + spkR, cy - spkR * 0.5f }, Vector2{ left + spkR, cy + spkR * 0.5f }, Vector2{ left + s * 0.55f, top }, color); });
        Record([=]() { DrawTriangle(Vector2{ left + spkR, cy + spkR * 0.5f }, Vector2{ left + s * 0.55f, bottom }, Vector2{ left + s * 0.55f, top }, color); });
        Record([=]() { DrawLineEx(Vector2{ cx + s * 0.15f, cy - s * 0.2f }, Vector2{ right, cy + s * 0.2f }, thick, color); });
        Record([=]() { DrawLineEx(Vector2{ cx + s * 0.15f, cy + s * 0.2f }, Vector2{ right, cy - s * 0.2f }, thick, color); });
    } else if (name == "info") {
        Record([=]() { DrawCircleLines(static_cast<int>(cx), static_cast<int>(cy), s * 0.45f, color); });
        Record([=]() { DrawCircle(static_cast<int>(cx), static_cast<int>(cy - s * 0.2f), thick * 0.8f, color); });
        Record([=]() { DrawRectangleRec(Rectangle{ cx - thick * 0.5f, cy - s * 0.05f, thick, s * 0.35f }, color); });
    } else if (name == "warning") {
        Record([=]() { DrawTriangleLines(Vector2{ left, bottom }, Vector2{ right, bottom }, Vector2{ cx, top }, color); });
        Record([=]() { DrawRectangleRec(Rectangle{ cx - thick * 0.5f, cy - s * 0.15f, thick, s * 0.3f }, color); });
        Record([=]() { DrawCircle(static_cast<int>(cx), static_cast<int>(bottom - s * 0.15f), thick * 0.6f, color); });
    } else if (name == "sun") {
        float rDisc = s * 0.17f;
        Record([=]() { DrawCircle(static_cast<int>(cx), static_cast<int>(cy), rDisc, color); });
        float rRay1 = s * 0.28f;
        float rRay2 = s * 0.44f;
        for (int i = 0; i < 8; ++i) {
            float angle = i * (3.14159265f / 4.0f);
            float cosA = std::cos(angle);
            float sinA = std::sin(angle);
            Record([=]() { DrawLineEx(Vector2{ cx + cosA * rRay1, cy + sinA * rRay1 },
                       Vector2{ cx + cosA * rRay2, cy + sinA * rRay2 },
                       thick, color); });
        }
    } else if (name == "moon") {
        // Classic crescent: a disc minus an equal-radius disc offset to the right.
        // Scanline difference, so the icon never needs to know its background color.
        const float s    = std::min(area.width, area.height);
        const float cx   = area.x + area.width  * 0.5f;
        const float cy   = area.y + area.height * 0.5f;
        const float r    = s * 0.42f;
        const float bite = cx + r * 0.72f;   // subtracted disc, same radius
        const int yTop = (int)std::floor(cy - r);
        const int yBot = (int)std::ceil (cy + r);
        for (int y = yTop; y <= yBot; ++y)
        {
            const float dy = (float)y + 0.5f - cy;
            if (std::fabs(dy) >= r) continue;
            const float half = std::sqrt(r * r - dy * dy);
            const float xL = cx - half;
            const float xR = std::min(cx + half, bite - half);   // taper the two tips
            const int xs = (int)std::floor(xL + 0.5f);
            const int xe = (int)std::floor(xR + 0.5f);
            if (xe > xs) Record([=]() { DrawRectangle(xs, y, xe - xs, 1, color); });
        }
    } else if (name == "blank") {
        // Draws nothing on purpose: reserves the icon slot so a button's label sits in the same
        // place whether or not it currently shows an icon (checked/unchecked rows of a list).
        (void)thick;
    } else {
        static std::unordered_set<std::string> s_warnedUnknownIcons;
        if (s_warnedUnknownIcons.insert(name).second) {
            LogWrite(LogLevel::Warn, "ui", "Unknown icon name: " + name);
        }
    }
}

} // namespace IYI::UI
