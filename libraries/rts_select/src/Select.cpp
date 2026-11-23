#include "Select.h"

#include <algorithm>
#include <cmath>
#include "IYI/Api.h"

namespace IYI::Select {

std::optional<Hit> Drag(State& st, Vector2 mouse, bool pressed, bool down, bool released)
{
    if (pressed) st.DragFrom = mouse;
    if (!st.DragFrom) return std::nullopt;
    const Vector2 start = *st.DragFrom;
    st.DragBox.reset();
    if (std::fabs(mouse.x - start.x) > DRAG || std::fabs(mouse.y - start.y) > DRAG) {
        st.DragBox = Box{ std::min(start.x, mouse.x), std::min(start.y, mouse.y), std::max(start.x, mouse.x), std::max(start.y, mouse.y) };
    }
    if (released || !down) {
        const std::optional<Box> box = st.DragBox;
        st.DragFrom.reset();
        st.DragBox.reset();
        return box ? Hit{ true, start, *box } : Hit{ false, start, {} };
    }
    return std::nullopt;
}

std::vector<Item> Project(const Camera3D& camera, const std::vector<Vector3>& points, const std::vector<int>& tiers,
                          const std::vector<bool>& boxable, std::vector<int>& index)
{
    std::vector<Item> out;
    index.clear();
    for (size_t i = 0; i < points.size(); ++i) {
        if (const auto p = Api::GetWorldToScreen(points[i], camera)) {
            out.push_back({ p->x, p->y, tiers[i], boxable[i] });
            index.push_back(static_cast<int>(i));
        }
    }
    return out;
}

int Pick(const std::vector<Item>& items, Vector2 at, float reach)
{
    int best = -1;
    double bestD = 0.0;
    for (size_t i = 0; i < items.size(); ++i) {
        const double dx = items[i].X - at.x, dy = items[i].Y - at.y, d = dx * dx + dy * dy;
        if (d > static_cast<double>(reach) * reach) continue;
        if (best < 0 || items[i].Tier < items[best].Tier || (items[i].Tier == items[best].Tier && d < bestD)) {
            best = static_cast<int>(i);
            bestD = d;
        }
    }
    return best;
}

std::vector<int> InBox(const std::vector<Item>& items, Box box)
{
    std::vector<int> out;
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& t = items[i];
        if (t.Boxable && box[0] <= t.X && t.X <= box[2] && box[1] <= t.Y && t.Y <= box[3]) out.push_back(static_cast<int>(i));
    }
    return out;
}

void Apply(State& st, const std::vector<std::string>& ids, bool add)
{
    std::vector<std::string> unique;   // ids once each, first seen first
    for (const std::string& id : ids) {
        if (std::find(unique.begin(), unique.end(), id) == unique.end()) unique.push_back(id);
    }
    if (!add) {
        st.Selected = unique;
        return;
    }
    for (const std::string& id : unique) {
        auto it = std::find(st.Selected.begin(), st.Selected.end(), id);
        if (it != st.Selected.end()) st.Selected.erase(it);
        else st.Selected.push_back(id);
    }
}

bool Group(State& st, int n, bool assign, double now)
{
    if (assign) {
        st.Groups[n] = st.Selected;
        return false;
    }
    auto it = st.Groups.find(n);
    if (it == st.Groups.end()) return false;
    st.Selected = it->second;
    const bool again = st.Recalled == n && now - st.RecalledAt < DOUBLE;
    st.Recalled = n;
    st.RecalledAt = now;
    return again;
}

} // namespace IYI::Select
