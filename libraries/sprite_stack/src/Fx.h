// The sprite_stack library's lighting, game side: loads the library's shaders and light sprites, owns the
// StackLightingHandle (the engine's lighting, StackLighting.h) and the few things the library adds on top
// (ground detail, marks sent as they grow, the light-sprite helpers). A game owns one Fx; everything else
// is called on `Lighting` (Render, DrawGround, DrawStacks, the look setters...).
// One implementation for native games and Python (scripts/sprite_stack/fx.py is a thin wrapper over
// engine.sprite_stack, src/LibraryBindings.cpp).

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "IYI/StackLighting.h"

namespace IYI {

// What Fx's constructor takes besides the light sprites (see fx.py load for what each does).
struct FxOptions
{
    float GroundHalf = 640.0f, LightHalf = 1536.0f, LightScale = 0.5f, ContactStrength = 0.6f;
    float ShadowScale = 1.0f, ContactScale = 1.0f;
    int SunMap = 0, ContactMap = 0, LightMap = 0, MarkMap = 0;   // texels square; 0 = off
    bool LightTops = false;          // DrawTerrain lights its tops with the night light map too
};

class Fx
{
public:
    // lights: name -> one-layer light sprite (LightSprites()). Loads the shaders (Api::LoadShader).
    Fx(std::unordered_map<std::string, const SpriteStack*> lights, const FxOptions& options);

    std::unique_ptr<StackLightingHandle> Lighting;
    const Shader* TerrainShader = nullptr;      // terrain.vs/fs; null without a sun map

    // draw_terrain's tops get grain finer than the ground picture (strength: brightness change) and the
    // picture's edges wobble up to `wobble` world units. Needs a sun map (std::invalid_argument without).
    void SetGroundDetail(float strength = 0.15f, float wobble = 0.5f);
    // Ground marks: `count` marks of list generation `gen`, a list that only grows while gen stays the
    // same. Call every frame: only what changed is sent, and mark(i) is asked only for those.
    void SetMarks(int gen, size_t count, const std::function<StackMark(size_t)>& mark);
    void SetMarks(const std::vector<StackMark>& marks, int gen);

    // The library's light sprites: "radial" (a round pool, pivot in its middle) and "cone" (a beam, pivot
    // at its apex, shining toward +x at rotation 0).
    static std::unordered_map<std::string, const SpriteStack*> LightSprites();
    // World point of a spot given in an object's own unrotated space (lx, ly) around its pivot (x, y).
    static std::pair<double, double> At(double x, double y, double rotation, double lx, double ly);

private:
    bool HasMarksSeen = false;
    int MarksGen = 0;
    size_t MarksCount = 0;
};

} // namespace IYI
