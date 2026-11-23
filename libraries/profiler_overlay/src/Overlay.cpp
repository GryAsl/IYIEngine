#include "Overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include "IYI/Api.h"

namespace IYI::ProfilerOverlay {

static constexpr int MARGIN = 12;              // pixels kept free below the table
static constexpr float MIN_MS = 0.25f;         // rows below this average are hidden (a thread's root row always shows)
static constexpr int WIDTH = 760, ROW = 24, FONT = 20;   // raylib's default font is crisp at multiples of 10
static constexpr int NUM_X = 360, BAR_X = 520, BAR_W = 220;
static constexpr Color PANEL{ 16, 18, 22, 210 }, TEXT{ 230, 232, 236, 255 }, DIM{ 150, 155, 165, 255 };
static constexpr Color BAR{ 90, 170, 250, 255 }, HOT{ 250, 120, 90, 255 }, WORST_TICK{ 250, 220, 120, 255 };

static std::string Root(const std::string& name) { return name.substr(0, name.find('/')); }
static const char* ThreadLabel(const std::string& name)
{
    return name == "game" ? "game thread" : name == "render" ? "render thread" : nullptr;
}

std::vector<ProfileSection> Summary(const std::vector<ProfileSection>& profile)
{
    std::vector<ProfileSection> out;
    for (const ProfileSection& r : profile) {
        if (ThreadLabel(Root(r.Name)) && std::count(r.Name.begin(), r.Name.end(), '/') <= 1) out.push_back(r);
    }
    return out;
}

std::pair<std::vector<ProfileSection>, std::vector<ProfileSection>> Split(const std::vector<ProfileSection>& profile)
{
    std::pair<std::vector<ProfileSection>, std::vector<ProfileSection>> out;
    for (const ProfileSection& r : profile) {
        const std::string root = Root(r.Name);
        if (root == "game") out.first.push_back(r);
        if (root == "render" || root == "gpu") out.second.push_back(r);
    }
    return out;
}

std::pair<std::vector<Row>, int> Rows(const std::vector<ProfileSection>& profile, float minMs, int maxRows)
{
    std::vector<Row> out;
    std::vector<std::string> parents;
    for (const ProfileSection& r : profile) {
        const char* thread = ThreadLabel(r.Name);
        if (r.AvgMs < minMs && !thread) continue;
        while (!parents.empty() && r.Name.rfind(parents.back() + "/", 0) != 0) parents.pop_back();
        std::string label = parents.empty() ? r.Name : r.Name.substr(parents.back().size() + 1);
        if (thread) label = thread;
        else if (label.rfind("scripts/", 0) == 0) label = label.substr(8);
        out.push_back({ label, static_cast<int>(parents.size()), r.AvgMs, r.MaxMs, r.Calls });
        parents.push_back(r.Name);
    }
    int hidden = 0;
    while (static_cast<int>(out.size()) > std::max(1, maxRows)) {   // the cheapest leaf goes, one at a time
        size_t cheapest = out.size();
        for (size_t i = 0; i < out.size(); ++i) {
            const bool leaf = i + 1 == out.size() || out[i + 1].Depth <= out[i].Depth;
            if (leaf && (cheapest == out.size() || out[i].Avg < out[cheapest].Avg)) cheapest = i;
        }
        out.erase(out.begin() + static_cast<std::ptrdiff_t>(cheapest));
        ++hidden;
    }
    return { out, hidden };
}

void Update(int& mode, bool& gpu)
{
    if (Api::IsKeyPressed(KEY_F3)) mode = (mode + 1) % 3;
    ProfileSetDetail(mode != 0);   // the engine's fine sub-sections, only while shown
    if (mode && Api::IsKeyPressed(KEY_F4)) gpu = ProfileSetGpu(!gpu);
}

static int Round(float v) { return static_cast<int>(std::nearbyint(v)); }   // half to even

static void Window(const std::vector<ProfileSection>& profile, int x0, int y0, int width, bool gpu)
{
    const float k = static_cast<float>(width) / WIDTH;
    const int numX = Round(NUM_X * k), barX = Round(BAR_X * k), barW = Round(BAR_W * k);
    const int fit = static_cast<int>(std::floor((Api::GetScreenHeight() - y0 - MARGIN - 12) / static_cast<float>(ROW))) - 2;
    const auto [rows, hidden] = Rows(profile, MIN_MS, fit);
    const int fps = Api::GetFPS();
    const float budget = fps > 0 ? 1000.0f / fps : 16.7f;
    Api::DrawRectangle(x0, y0, width, static_cast<int>(rows.size() + 2) * ROW + 12, PANEL);
    const std::string more = hidden ? "   " + std::to_string(hidden) + " cheapest hidden" : "";
    char line[200];
    std::snprintf(line, sizeof line, "%d fps  %.2f ms per frame   (F3, %s)%s", fps, budget, gpu ? "GPU on, F4" : "F4 GPU", more.c_str());
    Api::DrawText(line, x0 + 10, y0 + 6, FONT, TEXT);
    Api::DrawText("section", x0 + 10, y0 + 6 + ROW, FONT, DIM);
    Api::DrawText("avg  max ms", x0 + numX, y0 + 6 + ROW, FONT, DIM);
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        const int y = y0 + 6 + static_cast<int>(i + 2) * ROW;
        std::string label = r.Label;
        if (r.Calls >= 1.5f) label += "  x" + std::to_string(Round(r.Calls));
        Api::DrawText(label, x0 + 10 + r.Depth * 16, y, FONT, r.Depth == 0 ? TEXT : DIM);
        std::snprintf(line, sizeof line, "%5.2f %5.2f", r.Avg, r.Worst);
        Api::DrawText(line, x0 + numX, y, FONT, TEXT);
        const float f = std::min(1.0f, r.Avg / budget);
        Api::DrawRectangle(x0 + barX, y + 2, std::max(1, Round(barW * f)), ROW - 8, f > 0.5f ? HOT : BAR);
        Api::DrawRectangle(x0 + barX + std::min(barW, Round(barW * r.Worst / budget)), y, 2, ROW - 4, WORST_TICK);
    }
}

void Draw(int mode, int x, int y, bool gpu)
{
    if (!mode) return;
    const std::vector<ProfileSection> profile = ProfileResults();
    if (mode == 1) {
        Window(Summary(profile), x, y, WIDTH, gpu);
        return;
    }
    const int width = std::min(WIDTH, Api::GetScreenWidth() / 2 - x - 6);
    const auto [game, render] = Split(profile);
    Window(game, x, y, width, gpu);
    Window(render, Api::GetScreenWidth() - width - x, y, width, gpu);
}

} // namespace IYI::ProfilerOverlay
