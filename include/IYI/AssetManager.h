// Declares the resource manager responsible for loading, caching, and unloading raylib assets.
// Caches Texture2D, Font, Shader, Sound, and Music objects keyed by project-relative normalized paths.
// To keep the engine transparent and predictable, there is no reference counting, asset bundling, or
// asynchronous loading pipeline; duplicate requests return cached pointers, and missing files log a
// warning once and return nullptr rather than crashing.
//
// Connected to:
// - Owned by Application.
// - Invoked through Api.cpp (Api::Load*), which engine.load_*() and native games call.
// - Interacts directly with raylib C APIs for asset allocation and stream updates.
//
// Threads: the getters run on the game side. They read and decode the file at once (so sizes are known)
// and return a stable pointer into the cache, while the GL object (texture, font atlas, shader, render
// target) is made by a command in the frame packet's Setup pass (FramePacket.h), on the render side,
// before any draw of that frame. So a recorded draw takes the pointer and reads the GL id at replay;
// a GL id read on the game side may still be 0. UnloadAll runs on the render side at shutdown.
//
// Future evolution (per ROADMAP.md):
// Background asset streaming and complex asset databases are explicitly rejected.

#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "raylib.h"
#include "IYI/ResourceStore.h"
#include "IYI/SpriteStack.h"

namespace IYI {

// What the caches hold (agent channel health, SPEC §3.1). GPU size is an estimate: 4 bytes per
// texel, render textures with their 4-byte depth buffer, no mipmaps.
struct AssetStats
{
    int Textures = 0, Fonts = 0, Shaders = 0, Sounds = 0, Music = 0, SpriteStacks = 0;
    std::vector<std::string> RenderTextures;   // their keys, name#WxH
    double GpuMb = 0.0;
};

class AssetManager
{
public:
    AssetManager() = default;
    ~AssetManager();

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    void SetProjectDir(const std::string& dir, const std::vector<std::string>& libraryDirs = {});
    std::string ResolvePath(const std::string& relative) const;

    Texture2D* GetTexture(const std::string& relPath);
    // A texture made from pixels (not a file), cached under `key` like a loaded one: an existing key
    // returns its texture and ignores the pixels. rgba: width x height RGBA8. size > 0: resized to size x size.
    Texture2D* PutTexture(const std::string& key, const std::string& rgba, int width, int height, int size = 0);
    // pixel: a pixel font that is on its pixel grid at fontSize (raylib's size: ascender - descender in pixels),
    // point-filtered; UI text draws it only at whole multiples of that size or of the sizes AddPixelFontSize
    // gives it (DrawTextFitted, FontForText), so it stays sharp.
    Font*      GetFont(const std::string& relPath, int fontSize, const std::string& glyphText = "", bool pixel = false);
    // Another native size of a pixel font (its own file, drawn by the same text): more sharp sizes to pick.
    bool       AddPixelFontSize(const Font& font, const std::string& relPath, int fontSize);
    std::vector<int> PixelFontSizes(const Font& font);   // a pixel font's native sizes, ascending; empty otherwise
    // The cache's own font for `font` (a copy of a loaded font, or the default) with every glyph of
    // `text`: a stable pointer a recorded draw may keep. A missing glyph makes a new, larger version.
    // pixelSize > 0: the size it will be drawn at on screen. Well above the font's own size (a zoomed
    // or high-DPI draw) it returns a larger atlas of the same glyphs, made once, so text stays sharp.
    const Font* FontForText(const Font& font, const std::string& text, float pixelSize = 0.0f);
    // Grows whenever an answer of FontForText could change (a font loaded, an atlas grown or remade,
    // the cache cleared): a caller may keep an answer for the same font and text while it is unchanged.
    uint64_t FontGeneration() const { return m_fontGeneration.load(std::memory_order_relaxed); }
    Shader*    GetShader(const std::string& vsRelPath, const std::string& fsRelPath);
    Sound*     GetSound(const std::string& relPath);
    Music*     GetMusic(const std::string& relPath);
    // Named offscreen target, key = name + "#" + WxH. Engine-owned like every other cached resource.
    RenderTexture2D* GetRenderTexture(const std::string& name, int width, int height);
    // Layer images (bottom first, equal sizes) packed into one texture; key = the joined paths.
    SpriteStack* GetSpriteStack(const std::vector<std::string>& layerPaths);

    AssetStats Stats() const;

    void UpdateMusicStreams();
    void UnloadAll();
    // Let paths that failed to load be tried again (dev: a script hot reload may follow a new asset).
    void ForgetFailedPaths();

    ResourceStore& GetResourceStore() { return m_resources; }
    const ResourceStore& GetResourceStore() const { return m_resources; }

private:
    // The font's glyphs and atlas image; its texture is uploaded by a Setup command into `entry`.
    bool LoadFontAtlas(const std::string& path, int size, const std::vector<int>& codepoints, Font& entry, bool pixel = false);
    std::string NormalizeKey(const std::string& path) const;

    // The game side loads most assets (m_mutex), the audio side sounds and music (m_audioMutex, with
    // its own failure set): neither waits while the other reads and decodes a file.
    mutable std::mutex m_mutex;
    mutable std::mutex m_audioMutex;
    std::unordered_set<std::string> m_audioWarned;
    std::string m_projectDir;
    ResourceStore m_resources;
    std::unordered_map<std::string, Texture2D> m_textures;
    std::unordered_map<std::string, Font>      m_fonts;
    struct FontVariant { const Font* Atlas; std::size_t Glyphs; };   // made with Glyphs codepoints
    struct FontSource { std::string Path; int Size; std::unordered_set<int> Codepoints; const Font* Latest;
                        std::unordered_map<int, FontVariant> Variants;   // by atlas size
                        bool Pixel = false;
                        std::vector<const GlyphInfo*> Family; };   // AddPixelFontSize's sizes (m_fontSources keys)
    std::unordered_map<const GlyphInfo*, FontSource> m_fontSources;   // by the loaded font's glyphs
    // Larger versions FontForText made. A replaced one is freed on the render side after every draw of
    // the packet recorded when it was replaced; until then a packet may still draw with it.
    std::deque<Font> m_fontVersions;
    std::atomic<uint64_t> m_fontGeneration{ 0 };
    // The last two font files read: every atlas size/variant and glyph-set growth reloads the same file (m_mutex).
    std::vector<std::pair<std::string, std::vector<unsigned char>>> m_fontFiles;
    std::unordered_map<std::string, Shader>    m_shaders;
    std::unordered_map<std::string, Sound>     m_sounds;
    std::unordered_map<std::string, Music>     m_music;
    std::unordered_map<std::string, RenderTexture2D> m_renderTextures;
    std::unordered_map<std::string, SpriteStack> m_spriteStacks;
    // Shared pages the stacks' blocks are packed into, shelf by shelf (deque: stable for the uploads
    // RecordSetup runs later). The first page is small (a small game stays small), the rest large;
    // a block larger than a page keeps a texture of its own.
    struct StackPage { Texture2D Texture{}; int X = 0, Y = 0, ShelfH = 0; };
    std::deque<StackPage> m_stackPages;
    StackPage* PlaceStack(int w, int h, int& x, int& y);
    std::unordered_map<std::string, std::vector<unsigned char>> m_musicBuffers;
    // Keys that failed to load (missing or broken). Doubles as the don't-retry cache: a getter
    // called every frame for a missing file would otherwise stat the disk every frame.
    std::unordered_set<std::string>            m_warnedPaths;
};

} // namespace IYI
