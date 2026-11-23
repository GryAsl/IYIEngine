// Implements synchronous asset loading, path normalization, caching, and stream updates for raylib assets.
// Resources (Texture2D, Font, Shader, Sound, Music) are stored in hash maps keyed by normalized paths.
// Normalization converts backslashes to forward slashes and lowercases paths so lookups are consistent.
// Missing or invalid assets log an informative warning to stderr exactly once and return nullptr, ensuring
// the engine remains resilient during active development rather than crashing on missing files.
//
// Connected to:
// - Owned and updated by Application (which calls UpdateMusicStreams every frame and UnloadAll on shutdown).
// - Invoked by Python bindings in EngineBindings.cpp when scripts request asset handles.
// - Calls raylib C APIs to load, stream, and free GPU/audio resources.
//
// Future evolution (per ROADMAP.md):
// Remains strictly synchronous and single-threaded. Sprite sheet helpers may be added in the future,
// while asynchronous asset streaming and complex virtual filesystem layers are deliberately excluded.

#include "IYI/AssetManager.h"
#include "IYI/FramePacket.h"
#include "IYI/Log.h"
#include "IYI/ResourcePath.h"
#include "ShaderSource.h"
#include "rlgl.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace IYI {

namespace fs = std::filesystem;

// Texels per side of a shared stack page (256 MB RGBA). One page holds a whole town's stacks: a list
// drawn back to front switches texture at every page change, so two pages cost thousands of draw calls.
constexpr int kStackPageSide = 8192;

AssetManager::~AssetManager()
{
    UnloadAll();
}

void AssetManager::SetProjectDir(const std::string& dir, const std::vector<std::string>& libraryDirs)
{
    m_projectDir = dir;
    for (char& c : m_projectDir) {
        if (c == '\\') c = '/';
    }
#ifndef IYI_SHIPPING_BUILD
    m_resources.SetBackend(std::make_unique<DevResourceBackend>(m_projectDir, libraryDirs));
#else
    (void)libraryDirs;
#endif
}

std::string AssetManager::NormalizeKey(const std::string& path) const
{
    return ResourceFsPath(path);
}

std::string AssetManager::ResolvePath(const std::string& relative) const
{
    std::string phys = m_resources.GetPhysicalPath(relative);
    if (!phys.empty()) {
        return phys;
    }
    // Not an existing dev file (missing, or packaged): the would-be path, under the backend's own
    // containment rule, so engine.project_path() cannot hand out a path outside the project either.
    fs::path p = ResourceContainedPath(m_projectDir, ResourceFsPath(relative));
    if (p.empty()) {
        return "";
    }
    std::string resolved = p.string();
    for (char& c : resolved) {
        if (c == '\\') c = '/';
    }
    return resolved;
}

static std::string ExtractExtension(const std::string& path)
{
    fs::path p(path);
    std::string ext = p.extension().generic_string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

// Asset cache keys are normalized to forward-slash paths (case preserved) (with '#fontSize' for fonts and
// 'vs|fs' for shaders). If an asset file is missing or invalid, a warning is printed to stderr exactly
// once per unique path, and nullptr is returned. The engine never crashes on missing assets.
Texture2D* AssetManager::GetTexture(const std::string& relPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string key = NormalizeKey(relPath);
    auto it = m_textures.find(key);
    if (it != m_textures.end()) {
        return &it->second;
    }
    if (m_warnedPaths.count(key)) {
        return nullptr;  // missing or broken: already warned, do not hit the disk again
    }

    if (!m_resources.Exists(relPath)) {
        if (m_warnedPaths.find(key) == m_warnedPaths.end()) {
            LogWrite(LogLevel::Warn, "asset", "Asset not found: " + relPath);
            m_warnedPaths.insert(key);
        }
        return nullptr;
    }

    std::string physPath = m_resources.GetPhysicalPath(relPath);
    Image img = { 0 };
    if (!physPath.empty()) {
        img = LoadImage(physPath.c_str());
    } else {
        std::vector<unsigned char> bytes;
        if (m_resources.ReadBytes(relPath, bytes) && !bytes.empty()) {
            img = LoadImageFromMemory(ExtractExtension(relPath).c_str(), bytes.data(), static_cast<int>(bytes.size()));
        }
    }

    if (!IsImageValid(img)) {
        if (m_warnedPaths.find(key) == m_warnedPaths.end()) {
            LogWrite(LogLevel::Warn, "asset", "Failed to load texture: " + relPath);
            m_warnedPaths.insert(key);
        }
        return nullptr;
    }

    Texture2D* tex = &(m_textures[key] = Texture2D{ 0, img.width, img.height, img.mipmaps, img.format });
    RecordSetup([tex, img]() {
        tex->id = LoadTextureFromImage(img).id;
        UnloadImage(img);
    });
    return tex;
}

Texture2D* AssetManager::PutTexture(const std::string& key, const std::string& rgba, int width, int height, int size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_textures.find(key);
    if (it != m_textures.end()) {
        return &it->second;
    }
    if (width <= 0 || height <= 0 || rgba.size() != size_t(width) * height * 4) {
        return nullptr;
    }
    Image img = { RL_MALLOC(rgba.size()), width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    memcpy(img.data, rgba.data(), rgba.size());
    if (size > 0) {
        ImageResize(&img, size, size);
    }
    Texture2D* tex = &(m_textures[key] = Texture2D{ 0, img.width, img.height, img.mipmaps, img.format });
    RecordSetup([tex, img]() {
        tex->id = LoadTextureFromImage(img).id;
        UnloadImage(img);
    });
    return tex;
}

static auto s_fontCodepoints = []() {
    std::array<int, (0x007E - 0x0020 + 1) + (0x00FF - 0x00A0 + 1) + (0x017F - 0x0100 + 1) + (0x04FF - 0x0400 + 1)> arr{};
    size_t idx = 0;
    for (int c = 0x0020; c <= 0x007E; ++c) arr[idx++] = c;
    for (int c = 0x00A0; c <= 0x00FF; ++c) arr[idx++] = c;
    for (int c = 0x0100; c <= 0x017F; ++c) arr[idx++] = c;
    for (int c = 0x0400; c <= 0x04FF; ++c) arr[idx++] = c;
    return arr;
}();

Font* AssetManager::GetFont(const std::string& relPath, int fontSize, const std::string& glyphText, bool pixel)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // A sparse UTF-8 repertoire supports CJK without allocating tens of thousands of unused glyphs.
    // Canonicalize the set: callers with reordered/repeated text share one immutable atlas.
    std::vector<int> codepoints(s_fontCodepoints.begin(), s_fontCodepoints.end());
    codepoints.push_back(0x2026); // the UI fitting pipeline always needs an ellipsis
    int count = 0;
    int* extra = LoadCodepoints(glyphText.c_str(), &count);
    for (int i = 0; i < count; ++i) {
        int cp = extra[i];
        if (cp >= 0x20 && cp != 0x7f && !(cp >= 0x80 && cp <= 0x9f) &&
            cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff)) codepoints.push_back(cp);
    }
    UnloadCodepoints(extra);
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
    std::string key = NormalizeKey(relPath) + "#" + std::to_string(fontSize) + (pixel ? "px" : "");
    // Store the exact set rather than a hash so two repertoires can never alias.
    for (int cp : codepoints) key += ":" + std::to_string(cp);
    auto it = m_fonts.find(key);
    if (it != m_fonts.end()) {
        return &it->second;
    }
    if (m_warnedPaths.count(key)) {
        return nullptr;  // missing or broken: already warned, do not hit the disk again
    }

    if (!m_resources.Exists(relPath)) {
        if (m_warnedPaths.find(key) == m_warnedPaths.end()) {
            LogWrite(LogLevel::Warn, "asset", "Asset not found: " + relPath);
            m_warnedPaths.insert(key);
        }
        return nullptr;
    }

    Font& font = m_fonts[key];
    if (!LoadFontAtlas(relPath, fontSize, codepoints, font, pixel)) {
        m_fonts.erase(key);
        if (m_warnedPaths.find(key) == m_warnedPaths.end()) {
            LogWrite(LogLevel::Warn, "asset", "Failed to load font: " + relPath);
            m_warnedPaths.insert(key);
        }
        return nullptr;
    }
    m_fontSources[font.glyphs] = {relPath, fontSize, {codepoints.begin(), codepoints.end()}, &font, {}, pixel};
    ++m_fontGeneration;
    return &font;
}

const Font* AssetManager::FontForText(const Font& font, const std::string& text, float pixelSize)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // Public fonts/themes are immutable copied handles. Internal versions are cumulative and never
    // exposed through load_font, so typed prefixes/names cannot retain a full atlas per string.
    static const Font s_default = GetFontDefault();   // raylib's, made with the window: stable
    auto source = m_fontSources.find(font.glyphs);
    if (source == m_fontSources.end()) return &s_default;
    if (source->second.Pixel && pixelSize > 0.0f) {   // the largest native size pixelSize is a whole multiple of
        auto best = source;
        int bestSize = 0;
        auto consider = [&](decltype(source) it) {
            const float multiple = pixelSize / it->second.Size;
            if (std::fabs(multiple - std::round(multiple)) < 0.01f && it->second.Size > bestSize) { best = it; bestSize = it->second.Size; }
        };
        consider(source);
        for (const GlyphInfo* key : source->second.Family) {
            auto it = m_fontSources.find(key);
            if (it != m_fontSources.end()) consider(it);
        }
        source = best;
    }
    int count = 0;
    int* cps = LoadCodepoints(text.c_str(), &count);
    bool missing = false;
    for (int i = 0; i < count; ++i) {
        if (cps[i] < 32) continue;
        if (!source->second.Codepoints.count(cps[i])) { missing = true; break; }
    }
    UnloadCodepoints(cps);
    if (missing) {
        std::string repertoire = text;
        for (int cp : source->second.Codepoints) {
            int bytes = 0;
            const char* encoded = CodepointToUTF8(cp, &bytes);
            repertoire.append(encoded, bytes);
        }
        int n = 0;
        int* values = LoadCodepoints(repertoire.c_str(), &n);
        std::vector<int> set(values, values + n);
        UnloadCodepoints(values);
        std::sort(set.begin(), set.end());
        set.erase(std::unique(set.begin(), set.end()), set.end());
        Font& expanded = m_fontVersions.emplace_back();
        if (LoadFontAtlas(source->second.Path, source->second.Size, set, expanded, source->second.Pixel)) {
            source->second.Codepoints = {set.begin(), set.end()};
            Font* replaced = const_cast<Font*>(source->second.Latest);
            source->second.Latest = &expanded;
            ++m_fontGeneration;
            if (replaced->glyphs != source->first) {   // an earlier version, not the loaded font itself
                RecordTo(RecordPass::Screen, [replaced]() { UnloadFont(*replaced); *replaced = Font{}; });
            }
        } else {
            m_fontVersions.pop_back();
        }
    }
    // A glyph drawn more than 1.5x its atlas size blurs (bilinear upscale): use an atlas 2x, 4x the
    // size, made once per size and remade when the glyph set grew. Up to 4x keeps memory bounded.
    FontSource& src = source->second;
    if (src.Pixel || pixelSize <= src.Size * 1.5f) return src.Latest;   // a pixel font scales by whole multiples
    int size = src.Size * 2;
    if (pixelSize > size * 1.5f) size = src.Size * 4;
    FontVariant& variant = src.Variants[size];
    if (!variant.Atlas || variant.Glyphs != src.Codepoints.size()) {
        std::vector<int> set(src.Codepoints.begin(), src.Codepoints.end());
        std::sort(set.begin(), set.end());
        Font& larger = m_fontVersions.emplace_back();
        if (!LoadFontAtlas(src.Path, size, set, larger)) {
            m_fontVersions.pop_back();
            return src.Latest;
        }
        if (const Font* old = variant.Atlas) {
            RecordTo(RecordPass::Screen, [old]() { UnloadFont(*old); *const_cast<Font*>(old) = Font{}; });
        }
        variant = { &larger, src.Codepoints.size() };
        ++m_fontGeneration;
    }
    return variant.Atlas;
}

// raylib's LoadFontFromMemory (TTF/OTF) without its texture upload, which a Setup command does.
bool AssetManager::AddPixelFontSize(const Font& font, const std::string& relPath, int fontSize)
{
    const Font* member = GetFont(relPath, fontSize, "", true);   // takes the lock itself
    std::lock_guard<std::mutex> lock(m_mutex);
    auto source = m_fontSources.find(font.glyphs);
    if (!member || source == m_fontSources.end() || !source->second.Pixel) return false;
    auto& family = source->second.Family;
    if (member->glyphs != font.glyphs && std::find(family.begin(), family.end(), member->glyphs) == family.end()) {
        family.push_back(member->glyphs);
        ++m_fontGeneration;
    }
    return true;
}

std::vector<int> AssetManager::PixelFontSizes(const Font& font)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<int> sizes;
    auto source = m_fontSources.find(font.glyphs);
    if (source == m_fontSources.end() || !source->second.Pixel) return sizes;
    sizes.push_back(source->second.Size);
    for (const GlyphInfo* key : source->second.Family) {
        auto it = m_fontSources.find(key);
        if (it != m_fontSources.end()) sizes.push_back(it->second.Size);
    }
    std::sort(sizes.begin(), sizes.end());
    return sizes;
}

bool AssetManager::LoadFontAtlas(const std::string& path, int size, const std::vector<int>& codepoints, Font& entry, bool pixel)
{
    auto file = std::find_if(m_fontFiles.begin(), m_fontFiles.end(), [&](const auto& f) { return f.first == path; });
    if (file == m_fontFiles.end()) {
        if (m_fontFiles.size() >= 2) m_fontFiles.erase(m_fontFiles.begin());
        file = m_fontFiles.emplace(m_fontFiles.end(), path, std::vector<unsigned char>{});
        if (!m_resources.ReadBytes(path, file->second) || file->second.empty()) { m_fontFiles.pop_back(); return false; }
    }
    const std::vector<unsigned char>& bytes = file->second;
    Font font = {};
    font.baseSize = size;
    font.glyphPadding = 4;   // raylib's FONT_TTF_DEFAULT_CHARS_PADDING
#if RAYLIB_VERSION_MAJOR >= 6
    // raylib 6.0 leaves out codepoints the font lacks and reports how many glyphs it returned, so a missing
    // character draws as raylib's '?' fallback (5.5 kept an empty glyph). The source keeps the requested set, so
    // FontForText does not reload for them. SPEC §0.1.
    font.glyphs = LoadFontData(bytes.data(), static_cast<int>(bytes.size()), size, codepoints.data(),
                               static_cast<int>(codepoints.size()), FONT_DEFAULT, &font.glyphCount);
#else
    font.glyphCount = static_cast<int>(codepoints.size());
    font.glyphs = LoadFontData(bytes.data(), static_cast<int>(bytes.size()), size, const_cast<int*>(codepoints.data()),
                               font.glyphCount, FONT_DEFAULT);
#endif
    if (!font.glyphs) return false;
    Image atlas = GenImageFontAtlas(font.glyphs, &font.recs, font.glyphCount, size, font.glyphPadding, 0);
    for (int i = 0; i < font.glyphCount; ++i) {   // nothing draws or measures from glyph images (a glyph's size is recs[i]); skip raylib's second atlas copy
        UnloadImage(font.glyphs[i].image);
        font.glyphs[i].image = Image{};
    }
    font.texture = Texture2D{ 0, atlas.width, atlas.height, 1, atlas.format };
    entry = font;
    Font* target = &entry;
    RecordSetup([target, atlas, pixel]() {
        target->texture.id = LoadTextureFromImage(atlas).id;
        // Glyph atlases are drawn at fractional sizes by DrawTextFitted; point filtering makes stems uneven.
        // A pixel font is drawn only at whole multiples of its size, where point filtering keeps it sharp.
        SetTextureFilter(target->texture, pixel ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_BILINEAR);
        UnloadImage(atlas);
    });
    return true;
}

RenderTexture2D* AssetManager::GetRenderTexture(const std::string& name, int width, int height)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string key = name + "#" + std::to_string(width) + "x" + std::to_string(height);
    auto it = m_renderTextures.find(key);
    if (it != m_renderTextures.end()) {
        return &it->second;
    }
    if (width <= 0 || height <= 0) {
        if (m_warnedPaths.insert(key).second) {
            LogWrite(LogLevel::Warn, "asset", "Invalid render texture size: " + key);
        }
        return nullptr;
    }
    RenderTexture2D* target = &(m_renderTextures[key] = RenderTexture2D{ 0,
        Texture2D{ 0, width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 }, Texture2D{ 0, width, height, 1, 19 } });   // 19: raylib's depth format
    RecordSetup([target, key]() {
        const RenderTexture2D made = LoadRenderTexture(target->texture.width, target->texture.height);
        if (!IsRenderTextureValid(made)) LogWrite(LogLevel::Warn, "asset", "Failed to create render texture: " + key);
        target->id = made.id;
        target->texture.id = made.texture.id;
        target->depth.id = made.depth.id;
    });
    return target;
}

SpriteStack* AssetManager::GetSpriteStack(const std::vector<std::string>& layerPaths)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string key;
    for (const std::string& path : layerPaths) key += NormalizeKey(path) + "|";
    auto it = m_spriteStacks.find(key);
    if (it != m_spriteStacks.end()) {
        return &it->second;
    }
    if (m_warnedPaths.count(key)) {
        return nullptr;
    }

    std::vector<Image> layers;
    for (const std::string& path : layerPaths) {
        std::vector<unsigned char> bytes;
        Image img = { 0 };
        if (m_resources.ReadBytes(path, bytes) && !bytes.empty()) {
            img = LoadImageFromMemory(ExtractExtension(path).c_str(), bytes.data(), static_cast<int>(bytes.size()));
        }
        if (!IsImageValid(img)) {
            LogWrite(LogLevel::Warn, "asset", "Sprite stack layer not found or unreadable: " + path);
            break;
        }
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        layers.push_back(img);
    }
    SpriteStack stack;
    Image atlas = { 0 };
    const bool ok = layers.size() == layerPaths.size() && BuildSpriteStack(layers, stack, atlas);
    for (Image& img : layers) UnloadImage(img);
    if (!ok) {
        if (m_warnedPaths.insert(key).second) {
            LogWrite(LogLevel::Warn, "asset", "Failed to build sprite stack (" + std::to_string(layerPaths.size()) +
                     " layers, first: " + (layerPaths.empty() ? std::string("none") : layerPaths[0]) +
                     "); layers must exist and share one size");
        }
        return nullptr;
    }
    SpriteStack* made = &(m_spriteStacks[key] = stack);
    StackPage* page = PlaceStack(atlas.width, atlas.height, made->AtlasX, made->AtlasY);
    if (!page) {
        RecordSetup([made, atlas]() {
            made->Texture.id = LoadTextureFromImage(atlas).id;
            UnloadImage(atlas);
        });
        return made;
    }
    made->Texture.width = made->Texture.height = kStackPageSide;
    RecordSetup([made, atlas, page]() {
        if (page->Texture.id == 0) {
            Image blank = GenImageColor(kStackPageSide, kStackPageSide, BLANK);
            page->Texture = LoadTextureFromImage(blank);
            UnloadImage(blank);
        }
        UpdateTextureRec(page->Texture, Rectangle{ static_cast<float>(made->AtlasX), static_cast<float>(made->AtlasY),
                                                   static_cast<float>(atlas.width), static_cast<float>(atlas.height) }, atlas.data);
        made->Texture.id = page->Texture.id;
        UnloadImage(atlas);
    });
    return made;
}

// Next free spot for a w x h block: the last page's current shelf, its next shelf, or a new page.
AssetManager::StackPage* AssetManager::PlaceStack(int w, int h, int& x, int& y)
{
    if (w > kStackPageSide || h > kStackPageSide) return nullptr;
    if (!m_stackPages.empty()) {
        StackPage& p = m_stackPages.back();
        if (p.X + w > kStackPageSide) { p.X = 0; p.Y += p.ShelfH + 1; p.ShelfH = 0; }
        if (p.Y + h <= kStackPageSide) {
            x = p.X; y = p.Y;
            p.X += w + 1;
            p.ShelfH = std::max(p.ShelfH, h);
            return &p;
        }
    }
    StackPage& p = m_stackPages.emplace_back();
    x = y = 0;
    p.X = w + 1;
    p.ShelfH = h;
    return &p;
}

Shader* AssetManager::GetShader(const std::string& vsRelPath, const std::string& fsRelPath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string key = NormalizeKey(vsRelPath) + "|" + NormalizeKey(fsRelPath);
    auto it = m_shaders.find(key);
    if (it != m_shaders.end()) {
        return &it->second;
    }
    if (m_warnedPaths.count(key)) {
        return nullptr;  // missing or broken: already warned, do not hit the disk again
    }

    if (!vsRelPath.empty() && !m_resources.Exists(vsRelPath)) {
        if (m_warnedPaths.find(key) == m_warnedPaths.end()) {
            LogWrite(LogLevel::Warn, "asset", "Vertex shader not found: " + vsRelPath);
            m_warnedPaths.insert(key);
        }
        return nullptr;
    }
    if (!fsRelPath.empty() && !m_resources.Exists(fsRelPath)) {
        if (m_warnedPaths.find(key) == m_warnedPaths.end()) {
            LogWrite(LogLevel::Warn, "asset", "Fragment shader not found: " + fsRelPath);
            m_warnedPaths.insert(key);
        }
        return nullptr;
    }

    std::string vsCode;
    std::string fsCode;
    if ((!vsRelPath.empty() && !m_resources.ReadText(vsRelPath, vsCode)) ||
        (!fsRelPath.empty() && !m_resources.ReadText(fsRelPath, fsCode)) || (vsRelPath.empty() && fsRelPath.empty())) {
        if (m_warnedPaths.insert(key).second) {
            LogWrite(LogLevel::Warn, "asset", "Failed to load shader: vs='" + vsRelPath + "', fs='" + fsRelPath + "'");
        }
        return nullptr;
    }
    auto read = [this](const std::string& p, std::string& out) { return m_resources.ReadText(p, out); };
    std::string missing = ExpandShaderIncludes(vsCode, vsRelPath, read);
    if (missing.empty()) missing = ExpandShaderIncludes(fsCode, fsRelPath, read);
    if (!missing.empty()) {
        if (m_warnedPaths.insert(key).second) LogWrite(LogLevel::Warn, "asset", "Shader include not found: " + missing);
        return nullptr;
    }
#if defined(__EMSCRIPTEN__) || defined(IYI_MOBILE)
    for (std::string* code : { &vsCode, &fsCode }) {
#ifdef __EMSCRIPTEN__
        RewriteGlesShader(*code, true);
#else
        RewriteGlesShader(*code, false);
#endif
    }
#endif
    Shader* shader = &(m_shaders[key] = Shader{ 0, nullptr });
    RecordSetup([shader, vsCode, fsCode, vsRelPath, fsRelPath]() {   // compiled where the GL context is
        *shader = LoadShaderFromMemory(vsCode.empty() ? nullptr : vsCode.c_str(), fsCode.empty() ? nullptr : fsCode.c_str());
        if (!IsShaderValid(*shader)) {
            LogWrite(LogLevel::Warn, "asset", "Failed to load shader: vs='" + vsRelPath + "', fs='" + fsRelPath + "'");
        }
    });
    return shader;
}

Sound* AssetManager::GetSound(const std::string& relPath)
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    std::string key = NormalizeKey(relPath);
    auto it = m_sounds.find(key);
    if (it != m_sounds.end()) {
        return &it->second;
    }
    if (m_audioWarned.count(key)) {
        return nullptr;  // missing or broken: already warned, do not hit the disk again
    }

    if (!m_resources.Exists(relPath)) {
        if (m_audioWarned.find(key) == m_audioWarned.end()) {
            LogWrite(LogLevel::Warn, "audio", "Asset not found: " + relPath);
            m_audioWarned.insert(key);
        }
        return nullptr;
    }

    std::string physPath = m_resources.GetPhysicalPath(relPath);
    Sound sound = { 0 };

    if (!physPath.empty()) {
        sound = LoadSound(physPath.c_str());
    } else {
        std::vector<unsigned char> bytes;
        if (m_resources.ReadBytes(relPath, bytes) && !bytes.empty()) {
            std::string ext = ExtractExtension(relPath);
            Wave wave = LoadWaveFromMemory(ext.c_str(), bytes.data(), static_cast<int>(bytes.size()));
            if (IsWaveValid(wave)) {
                sound = LoadSoundFromWave(wave);
                UnloadWave(wave);
            }
        }
    }

    if (!IsSoundValid(sound)) {
        if (m_audioWarned.find(key) == m_audioWarned.end()) {
            LogWrite(LogLevel::Warn, "audio", "Failed to load sound: " + relPath);
            m_audioWarned.insert(key);
        }
        return nullptr;
    }

    m_sounds[key] = sound;
    return &m_sounds[key];
}

Music* AssetManager::GetMusic(const std::string& relPath)
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    std::string key = NormalizeKey(relPath);
    auto it = m_music.find(key);
    if (it != m_music.end()) {
        return &it->second;
    }
    if (m_audioWarned.count(key)) {
        return nullptr;  // missing or broken: already warned, do not hit the disk again
    }

    if (!m_resources.Exists(relPath)) {
        if (m_audioWarned.find(key) == m_audioWarned.end()) {
            LogWrite(LogLevel::Warn, "audio", "Asset not found: " + relPath);
            m_audioWarned.insert(key);
        }
        return nullptr;
    }

    std::string physPath = m_resources.GetPhysicalPath(relPath);
    Music music = { 0 };

    if (!physPath.empty()) {
        music = LoadMusicStream(physPath.c_str());
    } else {
        std::vector<unsigned char> bytes;
        if (m_resources.ReadBytes(relPath, bytes) && !bytes.empty()) {
            std::string ext = ExtractExtension(relPath);
            m_musicBuffers[key] = std::move(bytes);
            const auto& buf = m_musicBuffers[key];
            music = LoadMusicStreamFromMemory(ext.c_str(), buf.data(), static_cast<int>(buf.size()));
            if (!IsMusicValid(music)) {
                m_musicBuffers.erase(key);
            }
        }
    }

    if (!IsMusicValid(music)) {
        if (m_audioWarned.find(key) == m_audioWarned.end()) {
            LogWrite(LogLevel::Warn, "audio", "Failed to load music: " + relPath);
            m_audioWarned.insert(key);
        }
        return nullptr;
    }

    m_music[key] = music;
    return &m_music[key];
}

void AssetManager::UpdateMusicStreams()
{
    std::lock_guard<std::mutex> lock(m_audioMutex);
    for (auto& [key, music] : m_music) {
        if (IsMusicValid(music) && IsMusicStreamPlaying(music)) {
            UpdateMusicStream(music);
        }
    }
}

void AssetManager::ForgetFailedPaths()
{
    std::scoped_lock lock(m_mutex, m_audioMutex);
    m_warnedPaths.clear();
    m_audioWarned.clear();
}

AssetStats AssetManager::Stats() const
{
    std::scoped_lock lock(m_mutex, m_audioMutex);
    AssetStats stats;
    auto mb = [](int w, int h, int bytesPerTexel) { return double(w) * h * bytesPerTexel / (1024.0 * 1024.0); };
    stats.Textures = static_cast<int>(m_textures.size());
    stats.Fonts = static_cast<int>(m_fonts.size());
    stats.Shaders = static_cast<int>(m_shaders.size());
    stats.Sounds = static_cast<int>(m_sounds.size());
    stats.Music = static_cast<int>(m_music.size());
    stats.SpriteStacks = static_cast<int>(m_spriteStacks.size());
    for (const auto& [key, tex] : m_textures) stats.GpuMb += mb(tex.width, tex.height, 4);
    std::unordered_set<unsigned int> counted;   // pages are shared
    for (const auto& [key, stack] : m_spriteStacks) {
        if (counted.insert(stack.Texture.id).second) stats.GpuMb += mb(stack.Texture.width, stack.Texture.height, 4);
    }
    for (const auto& [key, target] : m_renderTextures) {
        stats.RenderTextures.push_back(key);
        stats.GpuMb += mb(target.texture.width, target.texture.height, 8);
    }
    return stats;
}

void AssetManager::UnloadAll()
{
    std::scoped_lock lock(m_mutex, m_audioMutex);
    for (auto& [key, tex] : m_textures) {
        if (IsTextureValid(tex)) {
            UnloadTexture(tex);
        }
    }
    m_textures.clear();

    for (auto& [key, font] : m_fonts) {
        if (IsFontValid(font)) {
            UnloadFont(font);
        }
    }
    m_fonts.clear();
    m_fontFiles.clear();
    for (Font& version : m_fontVersions) UnloadFont(version);
    m_fontVersions.clear();
    m_fontSources.clear();
    ++m_fontGeneration;

    for (auto& [key, shader] : m_shaders) {
        if (IsShaderValid(shader)) {
            UnloadShader(shader);
        }
    }
    m_shaders.clear();

    for (auto& [key, sound] : m_sounds) {
        if (IsSoundValid(sound)) {
            UnloadSound(sound);
        }
    }
    m_sounds.clear();

    for (auto& [key, music] : m_music) {
        if (IsMusicValid(music)) {
            UnloadMusicStream(music);
        }
    }
    m_music.clear();
    m_musicBuffers.clear();

    for (auto& [key, target] : m_renderTextures) {
        if (IsRenderTextureValid(target)) {
            UnloadRenderTexture(target);
        }
    }
    m_renderTextures.clear();

    std::unordered_set<unsigned int> unloaded;   // pages are shared
    for (auto& [key, stack] : m_spriteStacks) {
        if (IsTextureValid(stack.Texture) && unloaded.insert(stack.Texture.id).second) {
            UnloadTexture(stack.Texture);
        }
    }
    m_spriteStacks.clear();
    m_stackPages.clear();

    m_warnedPaths.clear();
}

} // namespace IYI
