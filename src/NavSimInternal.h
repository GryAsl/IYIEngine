#pragma once

#include "IYI/NavSim.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace IYI {

namespace {

constexpr int BLOCKED = 0, DEBRIS = 1, HAZARD = 2;   // extra-layer bits: what play put on the ground
constexpr int ROAD_MIDDLE = 3;                       // planner-only bit: a road cell with road all round (not the kerb)
constexpr double BUILDING_BUCKET = 64.0;             // geo_world's building bucket
constexpr double K_PI = 3.141592653589793;

// drive
constexpr double SAMPLE = 0.1, LOOK = 70.0, SCAN_EVERY = 0.25, TOUCH = 0.5;
constexpr int FAR_STRIDE = 4;
int64_t Key(int64_t i, int64_t j) { return i * 4294967296LL + j; }

double Hyp(double x, double y) { return Py::hypot(x, y); }

NavValue V(const std::string& s) { NavValue v; v.kind = NavValue::Text; v.text = s; return v; }
NavValue V(const char* s) { return V(std::string(s)); }
NavValue V(const std::optional<std::string>& s) { return s ? V(*s) : NavValue{}; }
NavValue V(NavXY p) { NavValue v; v.kind = NavValue::Reals; v.reals = { p.x, p.y }; return v; }
NavValue R(double x, int digits = -1) { NavValue v; v.kind = NavValue::Real; v.real = x; v.digits = digits; return v; }

bool StaticState(const std::string& s) { return s == "disabled" || s == "parked"; }
bool Has(const std::optional<std::string>& s) { return s && !s->empty(); }

// core._box_corners
std::array<NavXY, 4> BoxCorners(const std::array<double, 5>& box, double grow = 0.0)
{
    const double a = Py::radians(box[4]);
    const double ux = std::cos(a), uy = std::sin(a), hl = box[2] / 2 + grow, hw = box[3] / 2 + grow;
    static const double signs[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    std::array<NavXY, 4> out;
    for (int k = 0; k < 4; ++k) {
        const double sx = signs[k][0], sy = signs[k][1];
        out[k] = { box[0] + ux * sx * hl - uy * sy * hw, box[1] + uy * sx * hl + ux * sy * hw };
    }
    return out;
}

// core._seg_dist
double SegDist(double x, double y, NavXY a, NavXY b)
{
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double d2 = dx * dx + dy * dy;
    const double t = d2 < 1e-9 ? 0.0 : std::min(std::max(((x - a.x) * dx + (y - a.y) * dy) / d2, 0.0), 1.0);
    return Hyp(a.x + dx * t - x, a.y + dy * t - y);
}

// core._sub: the polyline between distances s0 and s1 (s1 < s0: reversed).
std::vector<NavXY> Sub(const std::vector<NavXY>& points, double s0, double s1)
{
    if (s1 < s0) {
        std::vector<NavXY> out = Sub(points, s1, s0);
        std::reverse(out.begin(), out.end());
        return out;
    }
    std::vector<NavXY> out{ NavPointAt(points, s0).first };
    double along = 0.0;
    for (size_t k = 0; k + 1 < points.size(); ++k) {
        along += Hyp(points[k + 1].x - points[k].x, points[k + 1].y - points[k].y);
        if (s0 < along && along < s1) out.push_back(points[k + 1]);
    }
    out.push_back(NavPointAt(points, s1).first);
    return out;
}

std::array<double, 5> Box(const NavVehicle& v) { return { v.X, v.Y, v.Len, v.Wid, Py::degrees(v.H) }; }


} // namespace

} // namespace IYI
