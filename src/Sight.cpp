// Implements SightGrid (Sight.h): bilinear ground, per-cell tops, sampled sight lines and ray marches.

#include "IYI/Sight.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace IYI {

void SightGrid::SetGround(int columns, int rows, float cell, Vector2 origin, std::vector<float> heights)
{
    if (columns <= 0 || rows <= 0 || cell <= 0.0f || heights.size() != static_cast<size_t>(columns + 1) * (rows + 1))
        throw std::invalid_argument("SightGrid.SetGround: (columns + 1) * (rows + 1) heights, cell > 0");
    m_columns = columns; m_rows = rows; m_cell = cell; m_origin = origin;
    m_ground = std::move(heights);
    m_top.clear();
    m_step = cell * 0.5f;
    const auto [lo, hi] = std::minmax_element(m_ground.begin(), m_ground.end());
    m_lowest = *lo; m_highest = *hi;
}

void SightGrid::SetBlockers(int columns, int rows, float cell, Vector2 origin, const std::vector<float>& heights)
{
    if (m_ground.empty()) throw std::logic_error("SightGrid.SetBlockers: set the ground first");
    if (columns <= 0 || rows <= 0 || cell <= 0.0f || heights.size() != static_cast<size_t>(columns) * rows)
        throw std::invalid_argument("SightGrid.SetBlockers: columns * rows heights, cell > 0");
    m_bColumns = columns; m_bRows = rows; m_bCell = cell; m_bOrigin = origin;
    m_top.resize(heights.size());
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c) {
            const size_t i = static_cast<size_t>(r) * columns + c;
            m_top[i] = Ground(origin.x + (c + 0.5f) * cell, origin.y + (r + 0.5f) * cell) + heights[i];
            m_highest = std::max(m_highest, m_top[i]);
        }
    m_step = std::min(m_cell, cell) * 0.5f;
}

void SightGrid::SetBlockerCells(const std::vector<int>& cells, float height)
{
    for (int i : cells) {
        if (i < 0 || i >= static_cast<int>(m_top.size())) throw std::out_of_range("SightGrid.SetBlockerCells: cell off the grid");
        const int r = i / m_bColumns, c = i % m_bColumns;
        m_top[i] = Ground(m_bOrigin.x + (c + 0.5f) * m_bCell, m_bOrigin.y + (r + 0.5f) * m_bCell) + height;
        m_highest = std::max(m_highest, m_top[i]);
    }
}

void SightGrid::SetFoliage(const std::vector<float>& lows, const std::vector<float>& highs)
{
    m_leafLow.clear(); m_leafHigh.clear();
    if (lows.empty() && highs.empty()) return;
    const size_t n = static_cast<size_t>(m_bColumns) * m_bRows;
    if (n == 0 || lows.size() != n || highs.size() != n)
        throw std::invalid_argument("SightGrid.SetFoliage: set the blockers first, then columns * rows lows and highs");
    m_leafLow.resize(n); m_leafHigh.resize(n);
    for (int r = 0; r < m_bRows; ++r)
        for (int c = 0; c < m_bColumns; ++c) {
            const size_t i = static_cast<size_t>(r) * m_bColumns + c;
            const float g = Ground(m_bOrigin.x + (c + 0.5f) * m_bCell, m_bOrigin.y + (r + 0.5f) * m_bCell);
            m_leafLow[i] = g + lows[i];
            m_leafHigh[i] = highs[i] > lows[i] ? g + highs[i] : -1e30f;   // none: no height is inside
        }
}

float SightGrid::Ground(float x, float y) const
{
    const float fx = std::clamp((x - m_origin.x) / m_cell, 0.0f, static_cast<float>(m_columns));
    const float fy = std::clamp((y - m_origin.y) / m_cell, 0.0f, static_cast<float>(m_rows));
    const int c = std::min(static_cast<int>(fx), m_columns - 1), r = std::min(static_cast<int>(fy), m_rows - 1);
    const float u = fx - c, v = fy - r;
    const size_t i = static_cast<size_t>(r) * (m_columns + 1) + c;
    const float top = m_ground[i] + (m_ground[i + 1] - m_ground[i]) * u;
    const float bottom = m_ground[i + m_columns + 1] + (m_ground[i + m_columns + 2] - m_ground[i + m_columns + 1]) * u;
    return top + (bottom - top) * v;
}

float SightGrid::Top(float x, float y) const
{
    if (m_top.empty()) return Ground(x, y);
    const int c = static_cast<int>(std::floor((x - m_bOrigin.x) / m_bCell)), r = static_cast<int>(std::floor((y - m_bOrigin.y) / m_bCell));
    if (c < 0 || r < 0 || c >= m_bColumns || r >= m_bRows) return Ground(x, y);
    return m_top[static_cast<size_t>(r) * m_bColumns + c];
}

bool SightGrid::Visible(Vector3 a, Vector3 b, float* foliage) const
{
    const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    const float length = std::sqrt(dx * dx + dy * dy);
    const int samples = static_cast<int>((length - 0.5f) / m_step);
    const bool leaves = foliage && !m_leafLow.empty();
    if (foliage) *foliage = 0.0f;
    for (int k = 1; k <= samples; ++k) {
        const float t = k * m_step / length;
        const float x = a.x + dx * t, y = a.y + dy * t, z = a.z + dz * t;
        if (z < Top(x, y)) return false;
        if (leaves) {
            const int c = static_cast<int>(std::floor((x - m_bOrigin.x) / m_bCell)), r = static_cast<int>(std::floor((y - m_bOrigin.y) / m_bCell));
            if (c >= 0 && r >= 0 && c < m_bColumns && r < m_bRows) {
                const size_t i = static_cast<size_t>(r) * m_bColumns + c;
                if (z >= m_leafLow[i] && z <= m_leafHigh[i]) *foliage += m_step;
            }
        }
    }
    return true;
}

bool SightGrid::Ray(Vector3 origin, Vector3 direction, float maxDistance, Vector3* hit) const
{
    const float len = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
    if (len <= 0.0f) return false;
    const Vector3 d { direction.x / len, direction.y / len, direction.z / len };
    float t0 = 0.0f, t1 = maxDistance;
    if (d.z < 0.0f) {                               // only the slab between the highest top and the lowest ground matters
        t0 = std::max(t0, (origin.z - m_highest) / -d.z);
        t1 = std::min(t1, (origin.z - m_lowest) / -d.z);
    }
    const auto under = [&](float t) { return origin.z + d.z * t <= Top(origin.x + d.x * t, origin.y + d.y * t); };
    float lo = t0;
    for (float t = t0 + m_step; lo < t1; t += m_step) {
        float hi = std::min(t, t1);
        if (under(hi)) {
            for (int k = 0; k < 10; ++k) {
                const float mid = (lo + hi) * 0.5f;
                (under(mid) ? hi : lo) = mid;
            }
            *hit = { origin.x + d.x * hi, origin.y + d.y * hi, origin.z + d.z * hi };
            return true;
        }
        lo = hi;
    }
    return false;
}

std::vector<SightPair> SightGrid::VisiblePairs(const std::vector<Vector4>& observers, const std::vector<Vector3>& targets) const
{
    std::vector<SightPair> out;
    for (int i = 0; i < static_cast<int>(observers.size()); ++i) {
        const Vector4& o = observers[i];
        for (int j = 0; j < static_cast<int>(targets.size()); ++j) {
            const Vector3& t = targets[j];
            const float dist = std::hypot(t.x - o.x, t.y - o.y);
            if (dist <= o.w && Visible({ o.x, o.y, o.z }, t)) out.push_back({ i, j, dist });
        }
    }
    return out;
}

} // namespace IYI
