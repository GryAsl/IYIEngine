// Post.h: the runtime of post.py in C++ (the same uniforms of world_post.fs).

#include "Post.h"

#include <vector>
#include "IYI/Api.h"

namespace IYI {

Post::Post()
{
    Shader_ = Api::LoadShader("", "assets/sprite_stack/shaders/world_post.fs");
    Api::SetPostprocessShader(Shader_, 1.0f, 0.0f, true);
}

void Post::Apply(const PostLook& look)
{
    if (!Shader_) return;
    auto set = [this](const char* name, std::vector<float> value) { Api::SetShaderValue(*Shader_, name, value); };
    set("uExposure", { look.Exposure });
    set("uContrast", { look.Contrast });
    set("uSaturation", { look.Saturation });
    set("uLift", { look.Lift.begin(), look.Lift.end() });
    set("uGain", { look.Gain.begin(), look.Gain.end() });
    set("uMist", { look.Mist });
    set("uMistColour", { look.MistColour.begin(), look.MistColour.end() });
    set("uTilt", { look.Tilt });
    set("uTiltCentre", { look.TiltCentre });
    set("uTiltBand", { look.TiltBand });
    set("uRain", { look.Rain });
    set("uVignette", { look.Vignette });
    set("uGrain", { look.Grain });
    set("uHeat", { look.Heat });
}

void Post::Unload()
{
    Api::SetPostprocessShader(nullptr);
}

} // namespace IYI
