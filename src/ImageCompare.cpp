// Visual regression image comparison (ImageCompare.h, docs/VISUAL-TESTS.md).

#include "IYI/ImageCompare.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>

namespace IYI {

namespace {

double Luma(const unsigned char* p)
{
    return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
}

} // namespace

ImageCompareResult CompareImages(const Image& base, const Image& current, const ImageCompareOptions& o, Image* diff)
{
    ImageCompareResult r;
    const int w = current.width, h = current.height;
    const auto* a = static_cast<const unsigned char*>(base.data);
    const auto* b = static_cast<const unsigned char*>(current.data);
    std::vector<char> masked(static_cast<size_t>(w) * h, 0);
    for (const Rectangle& m : o.masks) {
        const int x0 = std::clamp(static_cast<int>(m.x), 0, w), y0 = std::max(0, static_cast<int>(m.y));
        const int x1 = std::min(w, static_cast<int>(std::ceil(m.x + m.width)));
        const int y1 = std::min(h, static_cast<int>(std::ceil(m.y + m.height)));
        for (int y = y0; y < y1; ++y) std::fill(masked.begin() + y * w + x0, masked.begin() + y * w + std::max(x0, x1), 1);
    }

    // Changed pixels, counted per tile.
    const int tile = std::max(1, o.tile), tw = (w + tile - 1) / tile, th = (h + tile - 1) / tile;
    std::vector<int> tileCount(static_cast<size_t>(tw) * th, 0);
    std::vector<char> changed(static_cast<size_t>(w) * h, 0);
    long long open = 0;
    double total = 0.0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (masked[i]) continue;
            ++open;
            int most = 0;
            for (int k = 0; k < 3; ++k) {
                const int d = std::abs(a[i * 4 + k] - b[i * 4 + k]);
                most = std::max(most, d);
                total += d;
            }
            if (most > o.tolerance) {
                changed[i] = 1;
                ++r.changedPixels;
                ++tileCount[(y / tile) * tw + x / tile];
            }
        }
    }
    r.changedPct = open ? 100.0 * r.changedPixels / open : 0.0;
    r.meanDiff = open ? total / (3.0 * open) : 0.0;

    // Regions: 8-connected groups of tiles that hold more than noise.
    std::vector<char> seen(tileCount.size(), 0);
    for (int start = 0; start < tw * th; ++start) {
        if (seen[start] || tileCount[start] < o.tileMinPixels) continue;
        ImageCompareRegion region;
        int x0 = w, y0 = h, x1 = -1, y1 = -1;
        std::vector<int> stack = { start };
        seen[start] = 1;
        while (!stack.empty()) {
            const int t = stack.back();
            stack.pop_back();
            const int tx = t % tw, ty = t / tw;
            region.pixels += tileCount[t];
            for (int y = ty * tile; y < std::min(h, (ty + 1) * tile); ++y) {
                for (int x = tx * tile; x < std::min(w, (tx + 1) * tile); ++x) {
                    if (!changed[static_cast<size_t>(y) * w + x]) continue;
                    x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
                }
            }
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = tx + dx, ny = ty + dy, n = ny * tw + nx;
                    if (nx < 0 || ny < 0 || nx >= tw || ny >= th || seen[n] || tileCount[n] < o.tileMinPixels) continue;
                    seen[n] = 1;
                    stack.push_back(n);
                }
            }
        }
        region.box = { static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1 - x0 + 1), static_cast<float>(y1 - y0 + 1) };
        r.regions.push_back(region);
    }

    // Block SSIM on luma over the unmasked pixels of each 8x8 block.
    const double c1 = 6.5025, c2 = 58.5225;   // (0.01 * 255)^2, (0.03 * 255)^2
    double ssimSum = 0.0;
    int blocks = 0;
    for (int by = 0; by < h; by += 8) {
        for (int bx = 0; bx < w; bx += 8) {
            double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
            int n = 0;
            for (int y = by; y < std::min(h, by + 8); ++y) {
                for (int x = bx; x < std::min(w, bx + 8); ++x) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    if (masked[i]) continue;
                    const double la = Luma(a + i * 4), lb = Luma(b + i * 4);
                    sa += la; sb += lb; saa += la * la; sbb += lb * lb; sab += la * lb;
                    ++n;
                }
            }
            if (n == 0) continue;
            const double ma = sa / n, mb = sb / n;
            const double va = saa / n - ma * ma, vb = sbb / n - mb * mb, cov = sab / n - ma * mb;
            const double s = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
            ssimSum += s;
            r.minSsim = std::min(r.minSsim, s);
            ++blocks;
        }
    }
    r.ssim = blocks ? ssimSum / blocks : 1.0;

    if (diff != nullptr) {
        *diff = GenImageColor(w, h, BLACK);
        auto* d = static_cast<unsigned char*>(diff->data);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = static_cast<size_t>(y) * w + x;
                unsigned char* p = d + i * 4;
                p[0] = p[1] = p[2] = static_cast<unsigned char>(Luma(b + i * 4) * 0.35);
                if (masked[i]) {
                    p[2] = static_cast<unsigned char>(std::min(255, p[2] + 90));
                } else if (changed[i]) {
                    const bool noise = tileCount[(y / tile) * tw + x / tile] < o.tileMinPixels;
                    p[0] = noise ? 130 : 255;
                    p[1] = p[2] = 0;
                }
            }
        }
        for (const ImageCompareRegion& region : r.regions) {
            const Rectangle box = { region.box.x - 2, region.box.y - 2, region.box.width + 4, region.box.height + 4 };
            ImageDrawRectangleLinesEx(diff, box, 2, YELLOW);
        }
    }
    return r;
}

int CompareImagesCli(const char* jobPath)
{
    std::ifstream in(jobPath);
    const nlohmann::json job = nlohmann::json::parse(in, nullptr, false);
    if (job.is_discarded() || !job.contains("out") || !job["pairs"].is_array()) {
        TraceLog(LOG_ERROR, "--compare-images: %s is not a job (pairs, out)", jobPath);
        return 1;
    }
    nlohmann::json results = nlohmann::json::array();
    for (const nlohmann::json& pair : job["pairs"]) {
        nlohmann::json out = nlohmann::json::object();
        Image base = LoadImage(pair.value("baseline", "").c_str());
        Image cur = LoadImage(pair.value("current", "").c_str());
        if (!base.data || !cur.data) {
            out["error"] = !base.data ? "baseline not readable" : "current not readable";
        } else if (base.width != cur.width || base.height != cur.height) {
            out["size_match"] = false;
            out["baseline_size"] = { base.width, base.height };
            out["current_size"] = { cur.width, cur.height };
        } else {
            ImageFormat(&base, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            ImageFormat(&cur, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            ImageCompareOptions o;
            o.tolerance = pair.value("tolerance", o.tolerance);
            o.tile = pair.value("tile", o.tile);
            o.tileMinPixels = pair.value("tile_min", o.tileMinPixels);
            for (const nlohmann::json& m : pair.value("masks", nlohmann::json::array())) {
                o.masks.push_back({ m[0].get<float>(), m[1].get<float>(), m[2].get<float>(), m[3].get<float>() });
            }
            const std::string diffPath = pair.value("diff", "");
            Image diff = {};
            const ImageCompareResult r = CompareImages(base, cur, o, diffPath.empty() ? nullptr : &diff);
            if (diff.data) {
                ExportImage(diff, diffPath.c_str());
                UnloadImage(diff);
            }
            nlohmann::json regions = nlohmann::json::array();
            for (const ImageCompareRegion& g : r.regions) {
                regions.push_back({ { "box", { g.box.x, g.box.y, g.box.width, g.box.height } }, { "pixels", g.pixels } });
            }
            out = { { "size_match", true }, { "size", { cur.width, cur.height } }, { "changed_pixels", r.changedPixels },
                    { "changed_pct", r.changedPct }, { "mean_diff", r.meanDiff }, { "ssim", r.ssim },
                    { "min_ssim", r.minSsim }, { "regions", regions } };
        }
        if (base.data) UnloadImage(base);
        if (cur.data) UnloadImage(cur);
        results.push_back(out);
    }
    std::ofstream file(job["out"].get<std::string>());
    file << nlohmann::json({ { "results", results } }).dump(1);
    return file.good() ? 0 : 1;
}

} // namespace IYI
