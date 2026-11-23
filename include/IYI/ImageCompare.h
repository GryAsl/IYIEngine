#pragma once
// Compares two frames of the same size for visual regression tests (docs/VISUAL-TESTS.md): which pixels
// moved past a tolerance, grouped into changed regions (lone antialiasing pixels are noise), plus a
// block SSIM on luma for structure. Masked rectangles (dynamic content) count for nothing. CPU only:
// no window, no GL. `IYIEngine --compare-images <job.json>` runs it on PNG files (development builds).

#include <raylib.h>

#include <vector>

namespace IYI {

struct ImageCompareOptions {
    int tolerance = 12;              // a pixel changed when its largest channel moved more than this
    int tile = 16;                   // changed pixels are grouped in tiles of this side (pixels)
    int tileMinPixels = 4;           // a tile with fewer changed pixels is noise, not a region
    std::vector<Rectangle> masks;    // ignored areas, image pixels
};

struct ImageCompareRegion {
    Rectangle box;                   // bounds of its changed pixels
    int pixels = 0;                  // changed pixels inside
};

struct ImageCompareResult {
    long long changedPixels = 0;     // past the tolerance, outside the masks, noise included
    double changedPct = 0.0;         // of the unmasked pixels
    double meanDiff = 0.0;           // mean channel difference, 0..255
    double ssim = 1.0;               // mean of the 8x8 block SSIMs on luma
    double minSsim = 1.0;            // the worst block
    std::vector<ImageCompareRegion> regions;   // connected non-noise tiles
};

// Both images RGBA8 and the same size. `diff` (may be null) becomes an image of that size: a dark copy
// of `current`, changed pixels red (noise dim red), masks blue, each region outlined yellow.
ImageCompareResult CompareImages(const Image& base, const Image& current, const ImageCompareOptions& options,
                                 Image* diff);

// --compare-images: {"pairs": [{"baseline", "current", "diff", "masks": [[x, y, w, h]], "tolerance",
// "tile", "tile_min"}], "out": "<result.json>"}. Writes {"results": [...]} in pair order; returns 0, or 1
// when the job could not be read or the result not written.
int CompareImagesCli(const char* jobPath);

} // namespace IYI
