// Gate 1 smoke: canvas, resize, text, texture, render texture, mouse/keyboard, audio after a click.
// Every check prints one "SMOKE <name> <value>" line to the browser console.
#include <raylib.h>
#include <emscripten/emscripten.h>
#include <cmath>
#include <cstdio>
#include <vector>

static Texture2D tex;
static RenderTexture2D rt;
static Sound beep;
static int keys = 0, clicks = 0, frames = 0;

static void Frame()
{
    if (int k = GetKeyPressed()) { ++keys; printf("SMOKE key %d\n", k); }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        ++clicks;
        PlaySound(beep);
        printf("SMOKE click %d at %.0f,%.0f audio_ready=%d\n", clicks, GetMousePosition().x, GetMousePosition().y, IsAudioDeviceReady());
    }
    if (IsWindowResized()) printf("SMOKE resize %dx%d\n", GetScreenWidth(), GetScreenHeight());

    BeginTextureMode(rt);
    ClearBackground(DARKBLUE);
    DrawCircle(64, 64, 40 + 10 * sinf(frames * 0.1f), ORANGE);
    EndTextureMode();

    BeginDrawing();
    ClearBackground(RAYWHITE);
    DrawTexture(tex, 20, 60, WHITE);
    DrawTextureRec(rt.texture, {0, 0, 128, -128}, {200, 60}, WHITE);
    DrawText(TextFormat("frame %d keys %d clicks %d  %dx%d", frames, keys, clicks, GetScreenWidth(), GetScreenHeight()), 20, 20, 20, BLACK);
    DrawCircleV(GetMousePosition(), 8, RED);
    EndDrawing();

    if (++frames == 60) {
        Image shot = LoadImageFromTexture(rt.texture);
        Color c = GetImageColor(shot, 64, 64);
        printf("SMOKE rendertexture centre %d,%d,%d\n", c.r, c.g, c.b);
        UnloadImage(shot);
    }
}

int main()
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(800, 450, "iyi web smoke");
    Image img = GenImageChecked(128, 128, 16, 16, GREEN, DARKGREEN);
    tex = LoadTextureFromImage(img);
    UnloadImage(img);
    rt = LoadRenderTexture(128, 128);
    printf("SMOKE texture %d rendertexture %d\n", IsTextureValid(tex), IsRenderTextureValid(rt));

    InitAudioDevice();
    std::vector<short> pcm(22050);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = short(8000 * sinf(i * 2 * PI * 440 / 44100.0f));
    Wave w{(unsigned)pcm.size(), 44100, 16, 1, pcm.data()};
    beep = LoadSoundFromWave(w);
    printf("SMOKE audio_device %d sound %d\n", IsAudioDeviceReady(), IsSoundValid(beep));
    emscripten_set_main_loop(Frame, 0, 1);
}
