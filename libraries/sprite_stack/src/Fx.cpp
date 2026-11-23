// Fx.h: the runtime of fx.py in C++ (same shaders, same loading order, same marks bookkeeping).

#include "Fx.h"

#include <cmath>
#include <stdexcept>
#include "IYI/Api.h"

namespace IYI {

Fx::Fx(std::unordered_map<std::string, const SpriteStack*> lights, const FxOptions& o)
{
    const std::string dir = "assets/sprite_stack/shaders/";
    auto load = [&dir](const char* vs, const char* fs) { return Api::LoadShader(*vs ? dir + vs : "", dir + fs); };
    if (o.SunMap) TerrainShader = load("terrain.vs", "terrain.fs");
    if (o.LightTops && TerrainShader) Api::SetShaderValue(*TerrainShader, "uLightTops", { 1.0f });
    StackLightingSettings settings;
    settings.GroundHalf = o.GroundHalf;
    settings.ShadowScale = o.ShadowScale;
    settings.ContactScale = o.ContactScale;
    settings.LightHalf = o.LightHalf;
    settings.LightScale = o.LightScale;
    settings.ContactStrength = o.ContactStrength;
    settings.SunMapSize = o.SunMap;
    settings.ContactMapSize = o.ContactMap;
    settings.LightMapSize = o.LightMap;
    settings.MarkMapSize = o.MarkMap;
    StackLightingShaders shaders;
    shaders.ShadowFs = load("", "soft_shadow.fs");
    shaders.ContactFs = load("", "contact_ao.fs");
    shaders.LightFs = load("", "lightmap.fs");
    shaders.Sprite = load("lit_sprite.vs", "lit_sprite.fs");
    shaders.Blur = load("", "blur.fs");
    shaders.Wet = load("wet_ground.vs", "wet_ground.fs");
    shaders.Glow = load("", "glow.fs");
    if (o.MarkMap) shaders.Marks = load("", "marks.fs");
    if (o.SunMap) shaders.SunDepth = load("lit_sprite.vs", "sun_depth.fs");   // instanced casters
    shaders.Terrain = TerrainShader;
    Lighting = std::make_unique<StackLightingHandle>(settings, shaders, std::move(lights));
}

void Fx::SetGroundDetail(float strength, float wobble)
{
    if (!TerrainShader) throw std::invalid_argument("set_ground_detail needs a sun map (load)");
    Api::SetShaderValue(*TerrainShader, "uDetail", { strength, wobble });
}

void Fx::SetMarks(int gen, size_t count, const std::function<StackMark(size_t)>& mark)
{
    if (HasMarksSeen && MarksGen == gen && MarksCount == count) return;
    const bool grow = HasMarksSeen && MarksGen == gen && MarksCount < count;
    std::vector<StackMark> list;
    for (size_t i = grow ? MarksCount : 0; i < count; ++i) list.push_back(mark(i));
    Lighting->SetMarks(std::move(list), !grow);
    HasMarksSeen = true;
    MarksGen = gen;
    MarksCount = count;
}

void Fx::SetMarks(const std::vector<StackMark>& marks, int gen)
{
    SetMarks(gen, marks.size(), [&marks](size_t i) { return marks[i]; });
}

std::unordered_map<std::string, const SpriteStack*> Fx::LightSprites()
{
    std::unordered_map<std::string, const SpriteStack*> out;
    for (const auto& [name, pivot] : { std::pair<const char*, Vector3>{ "radial", { 32.0f, 32.0f, 0.0f } },
                                       std::pair<const char*, Vector3>{ "cone", { 0.0f, 24.0f, 0.0f } } }) {
        SpriteStack* stack = Api::LoadSpriteStack({ std::string("assets/sprite_stack/lights/") + name + ".png" });
        if (!stack) throw std::runtime_error(std::string("sprite_stack: light sprite missing: ") + name);
        stack->Origin = pivot;
        out[name] = stack;
    }
    return out;
}

std::pair<double, double> Fx::At(double x, double y, double rotation, double lx, double ly)
{
    const double r = rotation * 3.14159265358979323846 / 180.0;
    return { x + lx * std::cos(r) - ly * std::sin(r), y + lx * std::sin(r) + ly * std::cos(r) };
}

} // namespace IYI
