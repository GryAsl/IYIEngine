// Small native regression suite for engine invariants that need no window or game project.
// Plain checks, no framework: each failure prints file:line and the process exits non-zero (CTest).
// Covers: resource path/key handling, dev-path containment and packaged lookup, fixed-step clamping,
// tween/sequence lifecycle, retired UIElement safety, UTF-8 text editing helpers, Steam score clamping
// and handle allocation, atomic helper-module hot reload (embedded Python, no window), library roots
// and project-native roots under the dev backend, the frame profiler, sprite-stack packing and the UI
// pointer query.

#include "IYI/Application.h"
#include "IYI/AssetManager.h"
#include "../src/ShaderSource.h"
#include "IYI/CrashReport.h"
#include "IYI/FixedStep.h"
#include "IYI/FramePacket.h"
#include "IYI/Log.h"
#include "IYI/Nav.h"
#include "geo_world/src/World.h"
#include "IYI/Sight.h"
#include "IYI/Profiler.h"
#include "IYI/ProjectConfig.h"
#ifdef IYI_WITH_PYTHON
#include "IYI/PythonRuntime.h"
#endif
#include "IYI/ResourcePath.h"
#include "IYI/ResourceStore.h"
#include "IYI/SpriteStack.h"
#include "IYI/Particles.h"
#include "IYI/SteamBridge.h"
#ifndef IYI_SHIPPING_BUILD
#include "IYI/Trailer.h"
#endif
#include "IYI/Utf8Text.h"
#include "IYI/Video.h"
#include "pl_mpeg/pl_mpeg.h"   // declarations only: the implementation is in Video.cpp
#include "IYI/IYIUI/IYIUI.h"
#include "IYI/IYIUI/IYIUITween.h"
#include "sprite_stack/src/Daylight.h"
#include "rts_select/src/Select.h"
#include "profiler_overlay/src/Overlay.h"
#include "raymath.h"
#include "rres.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

static int g_failures = 0;

#define CHECK(cond) \
    do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

using namespace IYI;
namespace fs = std::filesystem;

#ifdef IYI_NET
int RunNetTests();   // tests/net_tests.cpp (libraries/net)
#endif

static void TestGlesShaderSource()
{
    std::string web = "#version 330\nvoid main() {}";
    RewriteGlesShader(web, true);
    CHECK(web == "#version 300 es\nprecision highp float;\nprecision highp int;\nvoid main() {}");
    std::string mobile = "#version 330\nprecision highp float;\nvoid main() {}";
    RewriteGlesShader(mobile, false);
    CHECK(mobile == "#version 300 es\nprecision highp int;\nprecision highp float;\nvoid main() {}");
    std::string unchanged = "#version 300 es\nvoid main() {}";
    RewriteGlesShader(unchanged, false);
    CHECK(unchanged == "#version 300 es\nvoid main() {}");

    auto read = [](const std::string& path, std::string& out) {
        out = "float f();";
        return path == "assets/s/f.glsl";
    };
    std::string included = "#version 330\n#include \"f.glsl\"\n// #include \"f.glsl\"\n#include \"f.glsl\"\nvoid main() {}";
    CHECK(ExpandShaderIncludes(included, "assets/s/a.fs", read).empty());
    CHECK(included == "#version 330\nfloat f();\n// #include \"f.glsl\"\nfloat f();\nvoid main() {}");
    std::string missing = "#include \"g.glsl\"\n";
    CHECK(ExpandShaderIncludes(missing, "assets/s/a.fs", read) == "assets/s/g.glsl");
}

// ---------------------------------------------------------------------------
static void TestFontGroupSize()
{
    IYI::UI::UICanvas canvas;
    CHECK(canvas.FontGroupSize("speed", 24.0f) == 24.0f);   // first frame: no group size yet, its own
    CHECK(canvas.FontGroupSize("speed", 16.0f) == 16.0f);
    canvas.NextFontGroupFrame();                             // next frame: the group's smallest caps all
    CHECK(canvas.FontGroupSize("speed", 24.0f) == 16.0f);
    CHECK(canvas.FontGroupSize("other", 24.0f) == 24.0f);    // groups are apart
    canvas.NextFontGroupFrame();                             // the small member hid: the group grows back
    CHECK(canvas.FontGroupSize("speed", 24.0f) == 24.0f);
}

static void TestFitHeightWrapAndStyles()
{
    using namespace IYI::UI;
    UICanvas canvas;
    auto place = [](UIElement* e, float x, float y, float w, float h) {
        e->Rect.AnchorMin = e->Rect.AnchorMax = e->Rect.Pivot = { 0.0f, 0.0f };
        e->Rect.AnchoredPosition = { x, y };
        e->Rect.SizeDelta = { w, h };
        return e;
    };
    // a vertical stack that fits its children: 10 + 30 + 5 + 50 + 10
    UIElement* col = place(canvas.Add(UIElementType::Panel, "col", nullptr), 0, 0, 200, 999);
    col->Layout = UILayoutMode::Vertical;
    col->FitHeight = true;
    col->Spacing = 5.0f;
    col->PaddingTop = col->PaddingBottom = 10.0f;
    place(canvas.Add(UIElementType::Panel, "a", col), 0, 0, 0, 30);
    place(canvas.Add(UIElementType::Panel, "b", col), 0, 0, 0, 50);
    // a wrapping row: three 80-wide chips in a 200-wide box make two rows (20 + 4 + 20)
    UIElement* chips = place(canvas.Add(UIElementType::Panel, "chips", nullptr), 300, 0, 200, 1);
    chips->Layout = UILayoutMode::Horizontal;
    chips->WrapChildren = chips->FitHeight = true;
    chips->Spacing = 4.0f;
    for (const char* n : { "c1", "c2", "c3" }) place(canvas.Add(UIElementType::Panel, n, chips), 0, 0, 80, 20);
    // a plain panel fits its lowest child; a bottom-pivot one grows upward
    UIElement* box = place(canvas.Add(UIElementType::Panel, "box", nullptr), 600, 500, 100, 10);
    box->Rect.Pivot = { 0.0f, 1.0f };
    box->FitHeight = true;
    place(canvas.Add(UIElementType::Panel, "inner", box), 0, 40, 10, 20);
    canvas.ResolveLayoutTree();
    CHECK(std::fabs(col->ResolvedRect.height - 105.0f) < 0.01f);
    CHECK(std::fabs(canvas.Find("b")->ResolvedRect.y - 45.0f) < 0.01f);
    CHECK(std::fabs(chips->ResolvedRect.height - 44.0f) < 0.01f);
    CHECK(canvas.Find("c3")->ResolvedRect.y == 24.0f && canvas.Find("c3")->ResolvedRect.x == 300.0f);
    CHECK(box->ResolvedRect.height == 60.0f && box->ResolvedRect.y == 440.0f);
    // a child added later re-fits (the layout cache sees the change)
    place(canvas.Add(UIElementType::Panel, "c4", chips), 0, 0, 80, 20);
    canvas.ResolveLayoutTree();
    CHECK(std::fabs(chips->ResolvedRect.height - 44.0f) < 0.01f);
    place(canvas.Add(UIElementType::Panel, "c5", chips), 0, 0, 80, 20);
    canvas.ResolveLayoutTree();
    CHECK(std::fabs(chips->ResolvedRect.height - 68.0f) < 0.01f);

    // nested fit stacks (each level's subtree is resolved once, after its container places it): 5 + 10 + 20 + 7 + 5
    UIElement* outer = place(canvas.Add(UIElementType::Panel, "outer", nullptr), 0, 600, 300, 1);
    UIElement* card = place(canvas.Add(UIElementType::Panel, "card", outer), 0, 0, 0, 1);
    place(canvas.Add(UIElementType::Panel, "l2", card), 0, 0, 0, 20);
    place(canvas.Add(UIElementType::Panel, "l1", card), 0, 0, 0, 10);
    UIElement* deep = place(canvas.Add(UIElementType::Panel, "deep", card), 0, 0, 0, 1);
    for (UIElement* e : { outer, card, deep }) {
        e->Layout = UILayoutMode::Vertical;
        e->FitHeight = e->StretchChildrenCrossAxis = true;
    }
    card->PaddingTop = card->PaddingBottom = card->PaddingLeft = card->PaddingRight = 5.0f;
    UIElement* d1 = place(canvas.Add(UIElementType::Panel, "d1", deep), 0, 0, 0, 7);
    canvas.ResolveLayoutTree();
    CHECK(outer->ResolvedRect.height == 47.0f && card->ResolvedRect.height == 47.0f);
    CHECK(deep->ResolvedRect.y == 635.0f && deep->ResolvedRect.width == 290.0f && d1->ResolvedRect.y == 635.0f);

    // Problems: a child past its parent, and nothing else
    place(canvas.Add(UIElementType::Panel, "wide", box), 0, 0, 150, 10);
    bool overflow = false;
    for (const auto& p : canvas.Problems()) overflow |= p.Kind == "OVERFLOW" && p.Element->Name == "wide";
    CHECK(overflow);

    // translation moves drawing and hit testing, never the layout
    UIElement* tb = place(canvas.Add(UIElementType::Button, "tb", nullptr), 800, 0, 20, 20);
    canvas.ResolveLayoutTree();
    CHECK(canvas.PointerOverUIAt(Vector2{ 805.0f, 10.0f }));
    tb->Rect.Translation = { 100.0f, 0.0f };
    canvas.ResolveLayoutTree();
    CHECK(tb->ResolvedRect.x == 800.0f && tb->ResolvedTransform.Apply(Vector2{ 800.0f, 0.0f }).x == 900.0f);
    CHECK(canvas.PointerOverUIAt(Vector2{ 905.0f, 10.0f }) && !canvas.PointerOverUIAt(Vector2{ 805.0f, 10.0f }));

    // Extend copies; the type scale sets role sizes times the text scale
    UITheme theme;
    theme.Get("chip").BorderWidth = 3.0f;
    theme.Get("chip").FontRole = "small";
    theme.Extend("chip_on", "chip").BackgroundColor = Color{ 1, 2, 3, 255 };
    theme.Get("chip").BorderWidth = 1.0f;
    CHECK(theme.Resolve("chip_on").BorderWidth == 3.0f && theme.Resolve("chip_on").BackgroundColor.r == 1);
    theme.TypeScale = { { "small", 16.0f } };
    canvas.SetTheme(theme);
    CHECK(canvas.Theme().Resolve("chip_on").FontSize == 16.0f);
    canvas.SetTextScale(1.5f);
    CHECK(canvas.Theme().Resolve("chip").FontSize == 24.0f);
    bool threw = false;
    try { theme.Extend("x", "missing"); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

static void TestPixelFontSnap()
{
    using IYI::UI::SnapPixelFontSize;
    const std::vector<int> none, one{ 16 }, family{ 12, 14, 16 };
    CHECK(SnapPixelFontSize(19.0f, 1.0f, none, true) == 19.0f);      // not a pixel font
    CHECK(SnapPixelFontSize(23.0f, 1.0f, one, true) == 16.0f);       // nearest multiple of 16
    CHECK(SnapPixelFontSize(25.0f, 1.0f, one, true) == 32.0f);
    CHECK(SnapPixelFontSize(10.0f, 1.0f, family, true) == 12.0f);    // never below the smallest size
    CHECK(SnapPixelFontSize(27.0f, 1.0f, family, true) == 28.0f);    // 2 x 14 is the closest
    CHECK(SnapPixelFontSize(24.0f, 1.0f, family, true) == 24.0f);    // 2 x 12 exact
    CHECK(SnapPixelFontSize(31.0f, 1.0f, family, false) == 32.0f);   // floor with slack: 2 x 16
    CHECK(SnapPixelFontSize(30.0f, 1.0f, family, false) == 28.0f);   // a shrunk fit goes to the next size down
    CHECK(std::fabs(SnapPixelFontSize(28.0f, 1.2f, one, true) * 1.2f - 32.0f) < 1e-4f);   // 33.6 screen px -> 32
}

static void TestResourcePaths()
{
    CHECK(ResourceFsPath("") == "");
    CHECK(ResourceFsPath(".") == "");
    CHECK(ResourceFsPath("IYIEngineLOGO.png") == "IYIEngineLOGO.png");                 // case preserved
    CHECK(ResourceFsPath("./Assets\\Sub/../IYIEngineLOGO.png") == "Assets/IYIEngineLOGO.png");
    CHECK(ResourceFsPath("/Assets//a.png") == "Assets/a.png");
    CHECK(ResourceKey("./Assets\\Sub/../IYIEngineLOGO.png") == "assets/iyienginelogo.png");
    CHECK(ResourceKey("assets/IYIENGINELOGO.PNG") == ResourceKey("Assets\\IYIEngineLOGO.png"));
}

static void WriteFile(const fs::path& p, const std::string& bytes)
{
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

static void TestDevBackendKeepsCase()
{
    fs::path dir = fs::temp_directory_path() / ("iyi_tests_dev_" + std::to_string(std::rand()));
    WriteFile(dir / "Assets" / "IYIEngineLOGO.png", "PNGDATA");

    ResourceStore store(std::make_unique<DevResourceBackend>(dir.generic_string()));
    CHECK(store.Exists("Assets/IYIEngineLOGO.png"));
    CHECK(store.Exists("Assets\\IYIEngineLOGO.png"));
    std::vector<unsigned char> bytes;
    CHECK(store.ReadBytes("./Assets/IYIEngineLOGO.png", bytes) && bytes.size() == 7);
    std::string phys = store.GetPhysicalPath("Assets/IYIEngineLOGO.png");
    const std::string tail = "Assets/IYIEngineLOGO.png";
    CHECK(phys.size() >= tail.size() && phys.substr(phys.size() - tail.size()) == tail); // exact case reaches the filesystem
    CHECK(!store.Exists("Assets/missing.png"));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

static bool EndsWith(const std::string& s, const std::string& tail)
{
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// Dev resources must never resolve outside the project root, through any entry point.
static void TestDevBackendContainment()
{
    fs::path dir = fs::temp_directory_path() / ("iyi_tests_contain_" + std::to_string(std::rand()));
    fs::path proj = dir / "proj";
    WriteFile(proj / "assets" / "a.png", "A");
    WriteFile(dir / "secret.txt", "S");
    WriteFile(dir / "proj2" / "secret.txt", "S");    // shares the root's name as a string prefix

    ResourceStore store(std::make_unique<DevResourceBackend>(proj.generic_string()));
    std::vector<unsigned char> bytes;
    std::string text;
    CHECK(store.Exists("assets/a.png") && store.ReadText("assets/a.png", text) && text == "A");
    CHECK(store.Exists("assets/../assets/a.png") && store.ReadBytes("assets/sub/../a.png", bytes) && bytes.size() == 1);
    CHECK(store.Exists("assets\\a.png") && EndsWith(store.GetPhysicalPath("assets\\..\\assets\\a.png"), "proj/assets/a.png"));

    const std::string outside[] = {
        "../secret.txt", "..\\secret.txt", "assets/../../secret.txt", "assets\\..\\..\\secret.txt",
        "a/b/c/../../../../secret.txt", "../proj2/secret.txt", "../../../../../../../../secret.txt",
        (dir / "secret.txt").generic_string(), (dir / "secret.txt").string(),
    };
    for (const std::string& path : outside) {
        CHECK(!store.Exists(path));
        CHECK(!store.ReadBytes(path, bytes) && bytes.empty());
        CHECK(!store.ReadText(path, text) && text.empty());
        CHECK(store.GetPhysicalPath(path).empty());
    }

    DevResourceBackend raw(proj.generic_string() + "/");  // called directly: no NormalizePath in front
    CHECK(raw.Exists("assets/a.png") && raw.Exists("./assets/a.png"));
    CHECK(!raw.Exists("/" + (dir / "secret.txt").relative_path().generic_string()));
    CHECK(!raw.Exists((dir / "secret.txt").generic_string()) && !raw.Exists("../secret.txt"));
    CHECK(DevResourceBackend(proj.string() + "\\").Exists("assets/a.png"));

    CHECK(ResourceContainedPath("proj", "").generic_string() == "proj/");
    CHECK(ResourceContainedPath("proj", "..").empty() && ResourceContainedPath("proj", "a/../..").empty());
    CHECK(ResourceContainedPath(".", "a.png").generic_string() == "a.png" && ResourceContainedPath(".", "../a.png").empty());
    CHECK(ResourceContainedPath("../HR", "../HR/a.png").generic_string() == "../HR/a.png");  // climbs out and back in
    CHECK(ResourceContainedPath("../HR", "../HR2/a.png").empty());
#ifdef _WIN32
    CHECK(ResourceContainedPath("C:/proj", "C:a.png").empty() && ResourceContainedPath("C:/proj", "D:/a.png").empty());
    CHECK(ResourceContainedPath("C:/", "a.png").generic_string() == "C:/a.png");
    CHECK(DevResourceBackend("C:/").GetProjectDir() == "C:/");                    // a drive root keeps its slash
#endif

    AssetManager assets;                    // engine.project_path(): the would-be path, still contained
    assets.SetProjectDir(proj.generic_string());
    CHECK(EndsWith(assets.ResolvePath("assets/a.png"), "proj/assets/a.png"));
    CHECK(EndsWith(assets.ResolvePath("saves/new.json"), "proj/saves/new.json"));   // missing but inside
    CHECK(assets.ResolvePath("../secret.txt").empty() && assets.ResolvePath("saves/../../x.json").empty());

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Hand-built rres archive: header + one empty chunk per resource, IDs hashed from ResourceKey exactly as iyipack does.
static void TestPackagedLookup()
{
    fs::path archive = fs::temp_directory_path() / ("iyi_tests_pack_" + std::to_string(std::rand()) + ".rres");
    const std::vector<std::string> names = { "Assets/IYIEngineLOGO.png", "scripts/Main.py" };
    {
        std::ofstream f(archive, std::ios::binary);
        rresFileHeader header = {};
        header.id[0] = 'r'; header.id[1] = 'r'; header.id[2] = 'e'; header.id[3] = 's';
        header.version = 100;
        header.chunkCount = static_cast<unsigned short>(names.size());
        f.write(reinterpret_cast<const char*>(&header), sizeof(header));
        for (const auto& n : names) {
            std::string key = ResourceKey(n);
            rresResourceChunkInfo info = {};
            info.id = rresComputeCRC32(reinterpret_cast<const unsigned char*>(key.data()), static_cast<int>(key.size()));
            info.packedSize = 0;
            f.write(reinterpret_cast<const char*>(&info), sizeof(info));
        }
    }

    ResourceStore store(std::make_unique<PackagedResourceBackend>(archive.generic_string()));
    CHECK(static_cast<PackagedResourceBackend*>(store.GetBackend())->IsValid());
    CHECK(store.Exists("Assets/IYIEngineLOGO.png"));
    CHECK(store.Exists("assets\\iyienginelogo.PNG"));   // any casing resolves to the same packaged ID
    CHECK(store.Exists("./scripts/main.py"));
    CHECK(!store.Exists("assets/other.png"));

    std::error_code ec;
    fs::remove(archive, ec);
}

// ---------------------------------------------------------------------------
static void TestFixedStep()
{
    CHECK(ClampFrameDelta(-1.0f) == 0.0f);
    CHECK(ClampFrameDelta(std::nanf("")) == 0.0f);
    CHECK(ClampFrameDelta(0.016f) == 0.016f);
    CHECK(ClampFrameDelta(0.2f) == kFixedMaxFrameDelta);

    // Normal 60 fps for one second: ~60 and ~30 steps.
    float a60 = 0.0f, a30 = 0.0f;
    int n60 = 0, n30 = 0;
    for (int i = 0; i < 60; ++i) {
        float dt = ClampFrameDelta(1.0f / 60.0f);
        n60 += AdvanceFixedClock(a60, dt, kFixedStep60);
        n30 += AdvanceFixedClock(a30, dt, kFixedStep30);
    }
    CHECK(n60 >= 59 && n60 <= 60);
    CHECK(n30 >= 29 && n30 <= 30);

    // Stalls (200 ms, 5 s) are clamped once; both clocks then consume the same simulated time.
    for (float stall : { 0.2f, 5.0f }) {
        float b60 = 0.0f, b30 = 0.0f;
        float dt = ClampFrameDelta(stall);
        int s60 = AdvanceFixedClock(b60, dt, kFixedStep60);
        int s30 = AdvanceFixedClock(b30, dt, kFixedStep30);
        CHECK(s60 == kFixedMaxCatchUpSteps);
        CHECK(s30 == kFixedMaxCatchUpSteps / 2);
        CHECK(std::fabs(s60 * kFixedStep60 - s30 * kFixedStep30) < 1e-4f);
        CHECK(b60 < kFixedStep60 + 1e-4f && b30 < kFixedStep30 + 1e-4f);
    }
}

// ---------------------------------------------------------------------------
using namespace IYI::UI;

static void TestTweensAndSequences()
{
    UICanvas canvas;
    UIElement* panel = canvas.AddPanel("P");

    UITweenHandle t = panel->MoveTo(100.0f, 50.0f, 1.0f);
    CHECK(t.Active() && !t.Completed());
    canvas.UpdateTweens(0.05f);
    CHECK(t.Progress() > 0.0f && t.Progress() < 1.0f);
    for (int i = 0; i < 40; ++i) canvas.UpdateTweens(0.05f);
    CHECK(t.Completed() && !t.Active());
    CHECK(panel->Rect.AnchoredPosition.x == 100.0f && panel->Rect.AnchoredPosition.y == 50.0f);
    UITweenHandle slide = panel->TranslateTo(-30.0f, 8.0f, 0.5f);
    for (int i = 0; i < 20; ++i) canvas.UpdateTweens(0.05f);
    CHECK(slide.Completed() && panel->Rect.Translation.x == -30.0f && panel->Rect.Translation.y == 8.0f);
    CHECK(panel->Rect.AnchoredPosition.x == 100.0f);   // a translation tween never touches the layout position

    UITweenHandle k = panel->FadeTo(0.0f, 5.0f);
    CHECK(k.Active());
    k.Kill();
    CHECK(!k.Active() && !k.Completed());

    UISequenceHandle seq = canvas.Sequence();
    seq.AppendMove(panel, 10.0f, 10.0f, 0.2f).AppendFade(panel, 0.5f, 0.2f);
    seq.Play();
    CHECK(seq.Active());
    for (int i = 0; i < 20; ++i) { canvas.UpdateSequences(0.05f); canvas.UpdateTweens(0.05f); }
    CHECK(seq.Completed());
    CHECK(panel->Rect.AnchoredPosition.x == 10.0f && panel->Opacity == 0.5f);

    UISequenceHandle killed = canvas.Sequence();
    killed.AppendFade(panel, 0.0f, 5.0f);
    killed.Play();
    killed.Kill();
    CHECK(!killed.Active());

    // Removing an element kills its tweens (Hard rule 12).
    UIElement* doomed = canvas.AddPanel("Doomed");
    UITweenHandle dt = doomed->ScaleTo(2.0f, 2.0f, 5.0f);
    CHECK(dt.Active());
    doomed->Remove();
    CHECK(!dt.Active());
}

// ---------------------------------------------------------------------------
static void TestRetiredElements()
{
    UICanvas canvas;
    UIElement* parent = canvas.AddPanel("Parent");
    UIElement* child = canvas.AddPanel("Child", parent);
    uint64_t childId = child->Id;

    // remove(): retained pointer stays readable, is dead, unfindable, and inert.
    parent->Remove();
    CHECK(!parent->Alive && !child->Alive);
    CHECK(canvas.Find("Parent") == nullptr && canvas.Find("Child") == nullptr);
    CHECK(canvas.FindById(childId) == nullptr);
    CHECK(canvas.AddPanel("X", parent) == nullptr);            // cannot grow a dead subtree
    CHECK(!parent->MoveTo(1.0f, 1.0f, 1.0f).Active());          // tweening a dead element is a no-op
    parent->Remove();                                           // double remove
    parent->ClearChildren();
    child->BringToFront();
    canvas.SetFocused(child);
    CHECK(canvas.GetFocused() == nullptr);
    CHECK(child->Name == "Child");                              // memory still valid

    // clear_children()
    UIElement* holder = canvas.AddPanel("Holder");
    UIElement* kid = canvas.AddPanel("Kid", holder);
    holder->ClearChildren();
    CHECK(holder->Alive && !kid->Alive && holder->Children.empty());
    CHECK(canvas.Find("Kid") == nullptr);
    CHECK(kid->Name == "Kid");
    UIElement* again = canvas.AddPanel("Kid", holder);          // name is free again, new object
    CHECK(again && again != kid && again->Alive);

    // clear_ui()
    UIElement* a = canvas.AddPanel("A");
    UIElement* b = canvas.AddPanel("B", a);
    canvas.ClearUI();
    CHECK(!a->Alive && !b->Alive && !holder->Alive && !again->Alive);
    CHECK(canvas.Find("A") == nullptr && canvas.Find("B") == nullptr);
    CHECK(canvas.Root()->Children.empty());
    a->Remove();
    b->ClearChildren();
    CHECK(a->Name == "A");
}

// ---------------------------------------------------------------------------
static const UIElement* s_heldByWrapper = nullptr;
static bool FakeWrapperHook(const UIElement* e) { return e == s_heldByWrapper; }

static void TestReclaimRetired()
{
    UICanvas canvas;
    UIElement* parent = canvas.AddPanel("RParent");
    UIElement* child = canvas.AddPanel("RChild", parent);
    uint64_t childId = child->Id;
    UISequenceHandle seq = canvas.Sequence();

    SetUIElementReferencedHook(&FakeWrapperHook);
    s_heldByWrapper = child;                      // a wrapper on a *child* keeps the whole subtree
    parent->Remove();
    canvas.ReclaimRetired();
    CHECK(!child->Alive && child->Name == "RChild" && parent->Name == "RParent");   // still valid memory
    CHECK(canvas.RetiredCount() == 1);

    // find never returns a retired element, tweens/sequence steps on it are refused.
    CHECK(canvas.Find("RChild") == nullptr && canvas.FindById(childId) == nullptr);
    UITweenHandle t = child->FadeTo(0.0f, 1.0f);
    CHECK(!t.Active() && t.TweenId == 0);
    seq.AppendFade(child, 0.0f, 1.0f).JoinMove(child, 1.0f, 1.0f, 1.0f).Play();
    for (int i = 0; i < 5; ++i) { canvas.UpdateSequences(0.5f); canvas.UpdateTweens(0.5f); }
    CHECK(child->Opacity == 1.0f && child->Rect.AnchoredPosition.x == 0.0f);        // nothing ran

    s_heldByWrapper = nullptr;                    // wrapper dropped: the next boundary frees it
    canvas.ReclaimRetired();
    CHECK(canvas.RetiredCount() == 0);
    UIElement* p2 = canvas.AddPanel("P2");         // no hook: freed at the first boundary
    SetUIElementReferencedHook(nullptr);
    p2->Remove();
    CHECK(canvas.RetiredCount() == 1);
    canvas.ReclaimRetired();
    CHECK(canvas.RetiredCount() == 0);
}

static void TestPasteFilter()
{
    CHECK(FilterPaste(std::string(5000, 'a'), true, true, true).size() == static_cast<size_t>(kMaxPasteCodepoints));  // ASCII: bytes == codepoints
    CHECK(FilterPaste("ab1 -\n\r\t\x01", true, true, true) == "ab1 -");
    CHECK(FilterPaste("ab1 -", true, false, false) == "ab");
    CHECK(FilterPaste("ab1 -", false, true, false) == "1");
    CHECK(FilterPaste("ab1 -", false, false, true) == " -");
    CHECK(FilterPaste("\xC3\xBC\xC5\x9F\xE2\x82\xAC", true, true, false) == "\xC3\xBC\xC5\x9F");   // u-umlaut, s-cedilla; euro is Symbol
    CHECK(FilterPaste("a\xFF" "z\xC3", true, true, true) == "az");                                  // invalid UTF-8 dropped
    CHECK(FilterPaste("", true, true, true).empty());
}

// ---------------------------------------------------------------------------
static void TestUtf8Text()
{
    std::string s;
    for (int cp : { 0x11F, 0xFC, 0x15F, 0x130, 0xF6, 0xE7 }) AppendUtf8(s, cp);   // Turkish
    CHECK(s == "\xC4\x9F\xC3\xBC\xC5\x9F\xC4\xB0\xC3\xB6\xC3\xA7");
    CHECK(CountCodepoints(s) == 6 && s.size() == 12);

    // An event line stays valid UTF-8 whatever bytes a field carries: stray, cut and overlong become U+FFFD.
    const int mark = LogEventCount();
    LogEvent("utf8", { { "v", s + "\xFF" + "\xC4" + "\xC0\xAF" + "ok\"" } });
    const std::vector<std::string> lines = LogEventsSince(mark);
    CHECK(lines.size() == 1 && lines[0].find(s + "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBDok\\\"") != std::string::npos);

    std::string de;
    for (int cp : { 0xE4, 0xF6, 0xFC, 0xDF }) AppendUtf8(de, cp);                  // German
    CHECK(de == "\xC3\xA4\xC3\xB6\xC3\xBC\xC3\x9F");

    std::string ru;
    for (int cp : { 0x41F, 0x440, 0x438, 0x432, 0x435, 0x442 }) AppendUtf8(ru, cp); // Cyrillic
    CHECK(ru == "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82" && CountCodepoints(ru) == 6);

    std::string e = "a";
    AppendUtf8(e, 0x20AC);   // 3 bytes
    AppendUtf8(e, 0x1F600);  // 4 bytes
    CHECK(CountCodepoints(e) == 3);
    CHECK(PopLastCodepoint(e) && e.size() == 4 && CountCodepoints(e) == 2);
    CHECK(PopLastCodepoint(e) && e == "a");
    CHECK(PopLastCodepoint(e) && e.empty());
    CHECK(!PopLastCodepoint(e));

    s = "x"; AppendUtf8(s, 0x11F);
    CHECK(PopLastCodepoint(s) && s == "x");                                        // never half a codepoint

    CHECK(ClassifyCodepoint('a') == CharClass::Letter && ClassifyCodepoint('7') == CharClass::Digit);
    CHECK(ClassifyCodepoint('-') == CharClass::Symbol && ClassifyCodepoint(' ') == CharClass::Symbol);
    for (int cp : { 0x11F, 0xFC, 0x15F, 0x130, 0xF6, 0xE7, 0xE4, 0xDF, 0x41F, 0x44F }) CHECK(ClassifyCodepoint(cp) == CharClass::Letter);
    CHECK(ClassifyCodepoint(0xD7) == CharClass::Symbol && ClassifyCodepoint(0x20AC) == CharClass::Symbol);
    CHECK(ClassifyCodepoint(0) == CharClass::Reject && ClassifyCodepoint(10) == CharClass::Reject);
    CHECK(ClassifyCodepoint(127) == CharClass::Reject && ClassifyCodepoint(0x85) == CharClass::Reject);
    CHECK(ClassifyCodepoint(0xD800) == CharClass::Reject && ClassifyCodepoint(0x110000) == CharClass::Reject);
    for (int cp : {0x4E2D, 0x3042, 0x30A2, 0xAC00, 0x1100, 0x3131})
        CHECK(ClassifyCodepoint(cp) == CharClass::Letter);
    std::string cjk;
    for (int cp : {0x4E2D, 0x3042, 0xAC00}) AppendUtf8(cjk, cp);
    CHECK(CountCodepoints(cjk) == 3);
    CHECK(FilterPaste(cjk, true, false, false) == cjk);
    CHECK(PopLastCodepoint(cjk) && CountCodepoints(cjk) == 2 && cjk.size() == 6);

    Font a = {}; a.baseSize = 38; a.texture.id = 10;
    UITextMemo memo;
    UITextFit fit;
    memo.UpdateInputs("123", 100, 40, 30, &a, fit);
    Font copied = a;
    CHECK(!memo.NeedsUpdate("123", 100, 40, 30, &copied, fit));
    copied.texture.id = 11;
    CHECK(memo.NeedsUpdate("123", 100, 40, 30, &copied, fit));
}

// ---------------------------------------------------------------------------
static void TestSteamHelpers()
{
    CHECK(ClampToInt32(0) == 0);
    CHECK(ClampToInt32(INT32_MAX) == INT32_MAX && ClampToInt32(INT32_MIN) == INT32_MIN);
    CHECK(ClampToInt32(static_cast<int64_t>(INT32_MAX) + 1) == INT32_MAX);
    CHECK(ClampToInt32(static_cast<int64_t>(INT32_MIN) - 1) == INT32_MIN);
    CHECK(ClampToInt32(INT64_MAX) == INT32_MAX && ClampToInt32(INT64_MIN) == INT32_MIN);
    CHECK(ClampToInt32(4294967296LL + 5) == INT32_MAX);   // a wrapping cast would give 5

    // Handle allocation with a fake live set.
    std::unordered_set<int> live;
    auto inUse = [&](int h) { return live.count(h) != 0; };
    int counter = 0;
    int h1 = NextFreeHandle(counter, inUse); live.insert(h1);
    int h2 = NextFreeHandle(counter, inUse); live.insert(h2);
    CHECK(h1 == 1 && h2 == 2);
    live.erase(h1);                                   // release
    counter = 0;                                      // wrap-around: must skip live 2, reuse released 1
    CHECK(NextFreeHandle(counter, inUse) == 1);
    live.insert(1);
    CHECK(NextFreeHandle(counter, inUse) == 3);
    counter = INT32_MAX;                              // overflow wraps to a positive handle
    int w = NextFreeHandle(counter, [](int) { return false; });
    CHECK(w == 1);
}

// ---------------------------------------------------------------------------
#ifdef IYI_WITH_PYTHON
// A helper module whose reload raises (invalid Python, or an exception halfway through) must keep
// running exactly as before; fixing the file reloads it. Runs last: it owns the embedded interpreter.
static void TestHelperReloadIsAtomic()
{
    fs::path dir = fs::temp_directory_path() / ("iyi_tests_reload_" + std::to_string(std::rand()));
    fs::path helper = dir / "scripts" / "helper_mod.py";
    fs::path out = dir / "probe.txt";
    WriteFile(dir / "scripts" / "main.py", "");
    WriteFile(helper, "VALUE = 1\ndef get():\n    return VALUE\n");
    WriteFile(dir / "scripts" / "probe.py",
        "import sys\nimport helper_mod\n"
        "def SceneStart():\n"
        "    with open(r'" + out.string() + "', 'w') as f:\n"
        "        f.write('%s %s %s' % (helper_mod.get(), helper_mod.VALUE, helper_mod is sys.modules['helper_mod']))\n");

    auto edit = [&](const std::string& source, int bumpSeconds) {
        WriteFile(helper, source);
        fs::last_write_time(helper, fs::last_write_time(helper) + std::chrono::seconds(bumpSeconds));
    };
    {
        PythonRuntime rt;
        auto probe = [&]() {
            if (!rt.CallSceneStart("scripts/probe.py")) return std::string("SceneStart failed");
            std::ifstream f(out);
            std::string line;
            std::getline(f, line);
            return line;
        };
        CHECK(rt.Initialize(dir.generic_string(), "scripts/main.py", true));
        CHECK(probe() == "1 1 True");

        edit("VALUE = 2\ndef get(:\n", 10);                                            // invalid Python
        int errors = LogErrorCount();
        rt.PollHotReload(1.0f);
        CHECK(LogErrorCount() > errors && rt.TakeReloadedScripts().empty());
        CHECK(probe() == "1 1 True");

        edit("VALUE = 2\ndef get():\n    return VALUE * 100\nraise RuntimeError('broken halfway')\n", 20);
        errors = LogErrorCount();
        rt.PollHotReload(1.0f);
        CHECK(LogErrorCount() > errors && rt.TakeReloadedScripts().empty());
        CHECK(probe() == "1 1 True");                     // without the rollback: "200 2 True"

        errors = LogErrorCount();
        rt.PollHotReload(1.0f);                           // the same broken file is not retried
        CHECK(LogErrorCount() == errors);

        edit("VALUE = 3\ndef get():\n    return VALUE\n", 30);
        rt.PollHotReload(1.0f);
        std::vector<std::string> reloaded = rt.TakeReloadedScripts();
        CHECK(reloaded.size() == 1 && reloaded[0] == "scripts/helper_mod.py");
        CHECK(probe() == "3 3 True");                     // same module object, new code
        rt.Shutdown();
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}
#endif

// The tilted camera: identical to raylib at scale 1; otherwise a pure screen-vertical squash
// around the offset, applied after rotation and zoom (what the sprite-stack pitch relies on).
static void TestTiltedCameraMatrix()
{
    const Camera2D cam{ Vector2{ 960.0f, 540.0f }, Vector2{ -20.0f, 40.0f }, 30.0f, 2.0f };
    const Matrix plain = GetCameraMatrix2D(cam);
    const Matrix same = CameraMatrix2D(cam, 1.0f);
    CHECK(std::memcmp(&plain, &same, sizeof(Matrix)) == 0);

    const float s = 0.5f;
    const Matrix tilted = CameraMatrix2D(cam, s);
    const Vector3 points[] = { { -20.0f, 40.0f, 0.0f }, { 100.0f, -35.0f, 0.0f }, { -300.0f, 220.0f, 0.0f } };
    for (const Vector3& p : points) {
        const Vector3 a = Vector3Transform(p, plain);
        const Vector3 b = Vector3Transform(p, tilted);
        CHECK(std::fabs(b.x - a.x) < 1e-3f);                                       // x untouched
        CHECK(std::fabs((b.y - cam.offset.y) - s * (a.y - cam.offset.y)) < 1e-3f);  // y squashed around offset
        const Vector3 back = Vector3Transform(b, MatrixInvert(tilted));
        CHECK(std::fabs(back.x - p.x) < 1e-2f && std::fabs(back.y - p.y) < 1e-2f);  // invertible round trip
    }
}

// project.json "libraries": the project wins, a library fills in what the project lacks, and every
// root keeps its own containment (no "../" escape through a library either).
static void TestLibraryRoots()
{
    const fs::path base = fs::temp_directory_path() / "iyi_library_roots";
    fs::remove_all(base);
    fs::create_directories(base / "game" / "assets");
    fs::create_directories(base / "lib" / "assets");
    std::ofstream(base / "game" / "assets" / "both.txt") << "game";
    std::ofstream(base / "lib" / "assets" / "both.txt") << "lib";
    std::ofstream(base / "lib" / "assets" / "only_lib.txt") << "lib";
    std::ofstream(base / "secret.txt") << "outside";

    DevResourceBackend backend(base.generic_string() + "/game", { (base / "lib").generic_string() });
    std::string text;
    CHECK(backend.ReadText("assets/both.txt", text) && text == "game");
    CHECK(backend.ReadText("assets/only_lib.txt", text) && text == "lib");
    CHECK(backend.GetPhysicalPath("assets/only_lib.txt").find("/lib/assets/") != std::string::npos);
    CHECK(!backend.Exists("assets/missing.txt"));
    CHECK(!backend.Exists("../secret.txt"));
    fs::remove_all(base);
}

// project.json "native" (SPEC §2b): an optional array of project-relative folders holding modules the
// game built. Resolved against the project dir like "libraries", but a folder that is not there yet is
// not a load error — the engine reports it later, when Python goes to import from it. Malformed input
// ("native" not an array, an entry not a string) fails the load exactly as "libraries" does.
static void TestNativeDirs()
{
    const fs::path base = fs::temp_directory_path() / "iyi_native_dirs";
    fs::remove_all(base);
    fs::create_directories(base);

    WriteFile(base / "project.json", "{ \"name\": \"T\" }");
    ProjectConfig plain;
    CHECK(plain.Load(base.generic_string()));
    CHECK(plain.NativeDirs.empty());                       // absent key -> empty default

    const std::string root = fs::canonical(base).generic_string();
    fs::create_directories(base / "native" / "bin");
    WriteFile(base / "project.json",
              "{ \"name\": \"T\", \"native\": [\"native/bin\", \"native/missing\"] }");
    ProjectConfig cfg;
    CHECK(cfg.Load(base.generic_string()));
    CHECK(cfg.NativeDirs.size() == 2);
    CHECK(cfg.NativeDirs[0] == root + "/native/bin");      // relative to the project dir, normalized
    CHECK(cfg.NativeDirs[1] == root + "/native/missing");  // kept even though it does not exist

    WriteFile(base / "project.json", "{ \"name\": \"T\", \"native\": \"native/bin\" }");
    ProjectConfig notArray;
    CHECK(!notArray.Load(base.generic_string()));

    WriteFile(base / "project.json", "{ \"name\": \"T\", \"native\": [7] }");
    ProjectConfig notString;
    CHECK(!notString.Load(base.generic_string()));

    fs::remove_all(base);
}

// Sections nest under the open one, average over frames, and a script cannot close engine sections.
// The engine libraries' runtime C++ (E7): the self-checks their Python modules had before they became wrappers.
static void TestDaylight()
{
    using namespace Daylight;
    CHECK(SunAt(12.0).Direction == 0.0 && std::fabs(SunAt(6.0).Direction - 90.0) < 1e-9 && std::fabs(SunAt(18.0).Direction + 90.0) < 1e-9);
    CHECK(SunAt(12.0).Length < SunAt(8.0).Length && SunAt(8.0).Length < SunAt(6.5).Length);   // shadows lengthen toward dusk
    CHECK(SunAt(0.0).Strength == 0.0 && std::fabs(SunAt(12.0).Strength - SHADOW_STRENGTH) < 1e-9);
    CHECK(Ambient(12.0) == 1.0 && std::fabs(Ambient(0.0) - NIGHT_AMBIENT) < 1e-9);
    CHECK(Lamps(12.0) == 0.0 && Lamps(0.0) == 1.0 && Lamps(18.5) > 0.0 && Lamps(18.5) <= 1.0);
    for (int i = 0; i < 240; ++i) {
        const double h = i / 10.0;
        CHECK(Ambient(h) >= NIGHT_AMBIENT && Ambient(h) <= 1.0 && Lamps(h) >= 0.0 && Lamps(h) <= 1.0);
        for (double c : AmbientColour(h)) CHECK(c >= 0.0 && c <= 1.0);
    }
    CHECK((AmbientColour(12.0) == Rgb{ 1.0, 1.0, 1.0 }));   // daylight: no light map at all
    const Rgb night = AmbientColour(0.0), dusk = AmbientColour(17.5);
    CHECK(night[2] > night[1] && night[1] > night[0] && std::fabs((night[0] + night[1] + night[2]) / 3 - NIGHT_AMBIENT) < 0.05);
    CHECK(dusk[0] > dusk[1] && std::max({ dusk[0], dusk[1], dusk[2] }) < 1.0);   // warm twilight, light map on
    CHECK(ShadowTint(12.0)[2] > ShadowTint(12.0)[0] && ShadowTint(7.0)[0] > ShadowTint(12.0)[0]);
    CHECK(Mist(6.5) > 0.5 && Mist(12.0) == 0.0 && Mist(2.0) == 0.0);
    const Grade day = GradeAt(12.0);
    CHECK(day.Exposure == 1.0 && day.Contrast == 1.0 && day.Saturation == 1.0 && (day.Gain == Rgb{ 1.0, 1.0, 1.0 }));   // day untouched
    CHECK(GradeAt(0.0).Gain[2] > 1.0 && 1.0 > GradeAt(0.0).Saturation);
}

static void TestRtsSelect()
{
    using namespace Select;
    State st;
    CHECK(!Drag(st, { 10, 10 }, true, true, false));
    auto hit = Drag(st, { 12, 11 }, false, false, true);
    CHECK(hit && !hit->IsBox && hit->At.x == 10 && hit->At.y == 10);
    Drag(st, { 10, 10 }, true, true, false);
    CHECK(!Drag(st, { 50, 40 }, false, true, false) && st.DragBox && (*st.DragBox == Box{ 10, 10, 50, 40 }));
    hit = Drag(st, { 50, 40 }, false, false, true);
    CHECK(hit && hit->IsBox && (hit->Area == Box{ 10, 10, 50, 40 }));
    const std::vector<Item> items{ { 100, 100, 0, true }, { 101, 100, 2, false }, { 100, 100, 3, false }, { 300, 300, 0, true } };
    CHECK(Pick(items, { 101, 100 }, 12) == 0 && Pick(items, { 500, 500 }, 12) == -1);
    CHECK(Pick(std::vector<Item>(items.begin() + 1, items.end()), { 100, 100 }, 12) == 0);   // the civilian over the house
    CHECK((InBox(items, { 90, 90, 310, 310 }) == std::vector<int>{ 0, 3 }));
    Apply(st, { "a", "b" });
    Apply(st, { "b", "c" }, true);
    CHECK((st.Selected == std::vector<std::string>{ "a", "c" }));
    Group(st, 1, true, 0.0);
    Apply(st, { "x" });
    CHECK(!Group(st, 1, false, 1.0) && (st.Selected == std::vector<std::string>{ "a", "c" }));
    CHECK(Group(st, 1, false, 1.2) && !Group(st, 2, false, 1.3));
}

static void TestProfilerOverlay()
{
    using namespace ProfilerOverlay;
    auto labels = [](const std::vector<Row>& rows) {
        std::vector<std::string> out;
        for (const Row& r : rows) out.push_back(r.Label + ":" + std::to_string(r.Depth));
        return out;
    };
    const std::vector<ProfileSection> profile{ { "update", 2.0f, 3.0f, 1.0f, {} }, { "update/scripts/actors/world.py", 1.8f, 2.5f, 1.0f, {} },
        { "update/scripts/actors/world.py/sim", 0.5f, 0.9f, 1.0f, {} }, { "draw", 0.6f, 0.8f, 1.0f, {} }, { "draw/ui", 0.01f, 0.02f, 1.0f, {} } };
    auto [shown, hidden] = Rows(profile, 0.02f, 28);
    CHECK((labels(shown) == std::vector<std::string>{ "update:0", "actors/world.py:1", "sim:2", "draw:0" }) && hidden == 0);
    std::vector<ProfileSection> deep(profile.begin(), profile.begin() + 4);
    deep.push_back({ "draw/world", 0.5f, 0.7f, 1.0f, {} });
    deep.push_back({ "draw/world/sprite_stacks", 0.4f, 0.5f, 1.0f, {} });
    deep.push_back({ "draw/world/sprite_stacks/emit", 0.3f, 0.4f, 1.0f, {} });
    deep.push_back({ "draw/world/sprite_stacks/cull", 0.05f, 0.1f, 1.0f, {} });
    std::tie(shown, hidden) = Rows(deep, 0.02f, 6);                // two too many: the cheapest leaves go, not a level
    CHECK(hidden == 2 && (labels(shown) == std::vector<std::string>{ "update:0", "actors/world.py:1", "sim:2", "draw:0", "world:1", "sprite_stacks:2" }));
    const std::vector<ProfileSection> threads{ { "game", 4.0f, 5.0f, 1.0f, {} }, { "game/update", 3.0f, 4.0f, 1.0f, {} },
        { "game/update/scripts/a.py", 2.0f, 3.0f, 1.0f, {} }, { "game/draw", 0.1f, 0.2f, 1.0f, {} }, { "game/wait", 0.9f, 1.5f, 1.0f, {} },
        { "render", 0.2f, 0.3f, 1.0f, {} }, { "render/draw", 5.0f, 6.0f, 1.0f, {} }, { "render/draw/world", 4.0f, 5.0f, 1.0f, {} },
        { "render/wait", 0.01f, 0.02f, 1.0f, {} }, { "gpu/render/draw", 3.0f, 3.5f, 1.0f, {} } };
    std::tie(shown, hidden) = Rows(Summary(threads), 0.25f, 28);   // roots + direct children, cheap ones out, cheap root kept
    CHECK((labels(shown) == std::vector<std::string>{ "game thread:0", "update:1", "wait:1", "render thread:0", "draw:1" }));
    const auto [game, render] = Split(threads);
    std::tie(shown, hidden) = Rows(game, 0.25f, 28);
    CHECK((labels(shown) == std::vector<std::string>{ "game thread:0", "update:1", "a.py:2", "wait:1" }));
    std::tie(shown, hidden) = Rows(render, 0.25f, 28);
    CHECK((labels(shown) == std::vector<std::string>{ "render thread:0", "draw:1", "world:2", "gpu/render/draw:0" }));
}

static void TestProfiler()
{
    for (int frame = 0; frame < 3; ++frame) {
        ProfileBegin("stage");
        ProfileBegin("hook", false);
        const size_t depth = ProfileDepth();
        ProfileBegin("script_part", true);
        ProfileEnd(true);
        ProfileEnd(true);                       // stray script end: must not close "hook"
        CHECK(ProfileDepth() == depth);
        ProfileBegin("left_open", true);         // closed by the hook's caller, as after an exception
        ProfileEndTo(depth - 1);
        ProfileEnd();
        ProfileEndFrame();
    }
    ProfileBegin("late");                       // a stage first seen after "stage"'s children ...
    ProfileEnd();
    ProfileBegin("stage");
    ProfileBegin("late_child");                 // ... and a child of "stage" first seen after it
    ProfileEnd();
    ProfileEnd();
    ProfileEndFrame();
    ProfilePublishPartial();
    std::vector<std::string> names;
    for (const ProfileSection& s : ProfileResults()) names.push_back(s.Name);
    const auto at = [&](const char* n) { return std::find(names.begin(), names.end(), n) - names.begin(); };
    CHECK(at("stage/late_child") < at("late"));   // tree order: stage's whole subtree comes first
    CHECK(at("stage") < at("stage/hook") && at("stage/hook") < at("stage/hook/script_part"));
    bool stage = false, hook = false, part = false;
    for (const ProfileSection& s : ProfileResults()) {
        stage |= s.Name == "stage" && s.Calls > 0.0f;
        hook |= s.Name == "stage/hook" && s.Calls > 0.0f;
        part |= s.Name == "stage/hook/script_part";
    }
    CHECK(stage && hook && part);

    // A C++ scope nests under what is open and closes on every way out, an exception included,
    // taking anything left open inside it along.
    ProfileBegin("hook2", false);
    const size_t before = ProfileDepth();
    try {
        ProfileScope scope("engine_work");
        ProfileBegin("inner_left_open");
        throw 1;
    } catch (int) {
    }
    CHECK(ProfileDepth() == before);
    { ProfileScope again("engine_work"); }            // reopened: found again, not duplicated
    ProfileEnd();
    ProfileEndFrame();
    ProfilePublishPartial();
    int work = 0;
    for (const ProfileSection& s : ProfileResults()) work += s.Name == "hook2/engine_work";
    CHECK(work <= 1);
}

// Each thread role has its own tree: two threads timing at once never mix sections, and the
// published results list the game tree, then the render tree. Both threads log at once too.
static void TestProfilerThreads()
{
    auto role = [](ProfileThread thread, const char* root) {
        ProfileSetThread(thread);
        // Past one window (1 s), so each tree publishes on its own thread, as in the engine.
        const auto start = std::chrono::steady_clock::now();
        for (int frame = 0; std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1100); ++frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            ProfileBegin(root);
            ProfileBegin("work");
            ProfileCount("items", 1);
            ProfileEnd();
            if (frame % 50 == 0) LogWrite(LogLevel::Debug, "test", std::string(root) + " frame");
            ProfileEnd();
            ProfileEndFrame();
        }
    };
    std::thread game(role, ProfileThread::Game, "t_game");
    std::thread render(role, ProfileThread::Render, "t_render");
    game.join();
    render.join();
    const std::vector<ProfileSection> results = ProfileResults();
    long gameAt = -1, renderAt = -1;
    bool mixed = false;
    for (size_t i = 0; i < results.size(); ++i) {
        const std::string& name = results[i].Name;
        if (name == "t_game/work") gameAt = static_cast<long>(i);
        if (name == "t_render/work") renderAt = static_cast<long>(i);
        mixed |= name.find("t_game/t_render") != std::string::npos || name.find("t_render/t_game") != std::string::npos;
    }
    CHECK(gameAt >= 0 && renderAt >= 0 && gameAt < renderAt);
    CHECK(!mixed);
    ProfileSetThread(ProfileThread::Game);
}

// The hang watchdog sees a stall and the section it is stuck in, then the recovery (CrashReport.h).
static void TestHangWatchdog()
{
    const fs::path dir = fs::temp_directory_path() / ("iyi_tests_watchdog_" + std::to_string(std::rand()));
    const auto beat = [&] {
        std::ifstream in(dir / "heartbeat.json");
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return text;
    };
    ProfileSetThread(ProfileThread::Game);
    ProfileBegin("t_stuck");
    CHECK(ProfileOpenSection(ProfileThread::Game) == "t_stuck");
    WatchdogStart(dir.generic_string());
    WatchdogFrame();
    std::this_thread::sleep_for(std::chrono::milliseconds(2700));
    const std::string stalled = beat();
    CHECK(stalled.find("\"stalled\": true") != std::string::npos);
    CHECK(stalled.find("\"game_section\": \"t_stuck\"") != std::string::npos);
    ProfileEnd();
    WatchdogFrame();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    CHECK(beat().find("\"stalled\": false") != std::string::npos);
    WatchdogStop();
    ProfileEndFrame();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// The frame packet records closures per pass and replays them in order; RenderOwned deletes on the
// render side at once and records the deletion on the game side. Closures only touch ints: no GL.
static void TestFramePacket()
{
    struct Probe { bool* Gone; ~Probe() { *Gone = true; } };
    FramePacket packet;
    std::vector<int> order;
    SetRecordingPacket(&packet);
    SetRenderSide(false);

    // Record appends to the chosen pass; ReplayPass runs only that pass, in recorded order.
    SetRecordPass(RecordPass::World);
    Record([&order]() { order.push_back(1); });
    Record([&order]() { order.push_back(2); });
    SetRecordPass(RecordPass::Offscreen);
    Record([&order]() { order.push_back(3); });
    CHECK(packet.Passes[static_cast<int>(RecordPass::World)].size() == 2);
    CHECK(packet.Passes[static_cast<int>(RecordPass::Offscreen)].size() == 1);
    CHECK(packet.Passes[static_cast<int>(RecordPass::Screen)].empty());
    ReplayPass(packet, RecordPass::Offscreen);
    CHECK(order == std::vector<int>({ 3 }));
    ReplayPass(packet, RecordPass::World);
    CHECK(order == std::vector<int>({ 3, 1, 2 }));

    // Clear empties every pass and the screenshot name; a cleared packet replays nothing.
    packet.Screenshot = "shot.png";
    packet.Clear();
    CHECK(packet.Screenshot.empty());
    for (const std::vector<RenderCommand>& pass : packet.Passes) CHECK(pass.empty());
    order.clear();
    ReplayPass(packet, RecordPass::Offscreen);
    ReplayPass(packet, RecordPass::World);
    ReplayPass(packet, RecordPass::Screen);
    CHECK(order.empty());

    // No recording packet: the command is dropped, nothing runs, nothing crashes.
    SetRecordingPacket(nullptr);
    Record([&order]() { order.push_back(9); });
    SetRecordingPacket(&packet);
    for (const std::vector<RenderCommand>& pass : packet.Passes) CHECK(pass.empty());
    CHECK(order.empty());

    // RenderOwned, game side: the last reference dropping records the deletion instead of running it.
    bool gone = false;
    SetRecordPass(RecordPass::Screen);
    {
        std::shared_ptr<Probe> owned = RenderOwned(new Probe{ &gone });
        owned.reset();
    }
    CHECK(!gone);
    CHECK(packet.Passes[static_cast<int>(RecordPass::Screen)].size() == 1);
    ReplayPass(packet, RecordPass::Screen);
    CHECK(gone);
    packet.Clear();

    // RenderOwned, a closure still holds it: the game side letting go deletes nothing; clearing the
    // packet on the render side drops the last reference there, so it is deleted at once, recording nothing.
    gone = false;
    {
        std::shared_ptr<Probe> owned = RenderOwned(new Probe{ &gone });
        SetRecordPass(RecordPass::World);
        Record([owned]() {});
        owned.reset();
    }
    CHECK(!gone);
    CHECK(packet.Passes[static_cast<int>(RecordPass::World)].size() == 1);
    SetRenderSide(true);
    packet.Clear();
    SetRenderSide(false);
    CHECK(gone);
    for (const std::vector<RenderCommand>& pass : packet.Passes) CHECK(pass.empty());

    // RecordSetup and RecordAudio go to their own pass whatever pass is current, and leave it unchanged.
    for (RecordPass current : { RecordPass::Offscreen, RecordPass::World, RecordPass::Screen }) {
        SetRecordPass(current);
        RecordSetup([]() {});
        RecordAudio([]() {});
        CHECK(CurrentRecordPass() == current);
        CHECK(packet.Passes[static_cast<int>(current)].empty());
    }
    CHECK(packet.Passes[static_cast<int>(RecordPass::Setup)].size() == 3);
    CHECK(packet.Passes[static_cast<int>(RecordPass::Audio)].size() == 3);
    packet.Clear();

    // Render side: a Record call runs at once and is not appended.
    SetRecordPass(RecordPass::Screen);
    SetRenderSide(true);
    int ranAtOnce = 0;
    Record([&ranAtOnce]() { ++ranAtOnce; });
    SetRenderSide(false);
    CHECK(ranAtOnce == 1);
    for (const std::vector<RenderCommand>& pass : packet.Passes) CHECK(pass.empty());

    // ReplayingWorld3D is true only while the World pass of a Camera3DActive packet runs.
    bool inWorld = true, inScreen = true;
    SetRecordPass(RecordPass::World);
    Record([&inWorld]() { inWorld = ReplayingWorld3D(); });
    SetRecordPass(RecordPass::Screen);
    Record([&inScreen]() { inScreen = ReplayingWorld3D(); });
    packet.Camera3DActive = false;
    ReplayPass(packet, RecordPass::World);
    CHECK(!inWorld);
    packet.Camera3DActive = true;
    ReplayPass(packet, RecordPass::World);
    CHECK(inWorld);
    ReplayPass(packet, RecordPass::Screen);
    CHECK(!inScreen);
    CHECK(!ReplayingWorld3D());
    packet.Camera3DActive = false;
    packet.Clear();

    // OnRenderSide is per thread.
    SetRenderSide(true);
    bool otherThreadSaw = true;
    std::thread other([&otherThreadSaw]() { otherThreadSaw = OnRenderSide(); });
    other.join();
    CHECK(OnRenderSide());
    CHECK(!otherThreadSaw);

    SetRecordingPacket(nullptr);
    SetRecordPass(RecordPass::Offscreen);
    SetRenderSide(false);
}

// Layers packed into columns with a 1-texel gap; each layer rect holds that layer's pixels.
static void TestSpriteStackPacking()
{
    std::vector<Image> layers;
    for (int i = 0; i < 5; ++i) {
        Image img = GenImageColor(3, 2, Color{ static_cast<unsigned char>(10 * (i + 1)), 0, 0, 255 });
        layers.push_back(img);
    }
    // BuildSpriteStack uploads a texture, which needs a GL context; check the layout math directly.
    SpriteStack stack;
    stack.Layers = 5; stack.Width = 3; stack.Height = 2; stack.PerColumn = 2;
    const Rectangle r0 = stack.LayerRect(0), r1 = stack.LayerRect(1), r2 = stack.LayerRect(2), r4 = stack.LayerRect(4);
    CHECK(r0.x == 0 && r0.y == 0 && r0.width == 3 && r0.height == 2);
    CHECK(r1.x == 0 && r1.y == 3);              // next row, 1 texel gap
    CHECK(r2.x == 4 && r2.y == 0);              // next column, 1 texel gap
    CHECK(r4.x == 8 && r4.y == 0);
    std::vector<Image> mixed = { layers[0], GenImageColor(4, 2, BLANK) };
    SpriteStack refused;
    Image atlas = { 0 };
    CHECK(!BuildSpriteStack(mixed, refused, atlas));   // different sizes are refused before any upload
    stack.AtlasX = 100; stack.AtlasY = 50;      // packed into a shared page
    CHECK(stack.LayerRect(2).x == 104 && stack.LayerRect(1).y == 53);
    std::vector<Image> many(36, layers[0]);
    SpriteStack block;
    CHECK(BuildSpriteStack(many, block, atlas));   // 36 layers of 3 x 2: a near-square block, not one tall column
    CHECK(block.PerColumn == 6 && atlas.width == 23 && atlas.height == 17);
    UnloadImage(atlas);
    UnloadImage(mixed[1]);
    for (Image& img : layers) UnloadImage(img);
}

// Culling keeps exactly the stacks that reach the target: one off to the side goes, one whose base
// is below the bottom edge but whose top climbs into view stays (layers lift toward screen-up).
static void TestSpriteStackCulling()
{
    SpriteStack stack;
    stack.Layers = 20; stack.Width = 10; stack.Height = 10;
    stack.Origin = Vector3{ 5.0f, 5.0f, 0.0f };
    SpriteStackPass pass;
    pass.LayerHeight = 3.0f;                                    // top layer 57 units up, at height scale 1
    const Matrix mvp = MatrixMultiply(MatrixIdentity(), MatrixOrtho(0.0, 100.0, 100.0, 0.0, 0.0, 1.0));
    auto at = [&](float x, float y) { SpriteStackItem item; item.Stack = &stack; item.X = x; item.Y = y; return item; };
    CHECK(!SpriteStackOffTarget(at(50.0f, 50.0f), pass, mvp));    // on screen
    CHECK(SpriteStackOffTarget(at(500.0f, 50.0f), pass, mvp));    // far right
    CHECK(SpriteStackOffTarget(at(50.0f, -20.0f), pass, mvp));    // above the top: climbing only goes further
    CHECK(!SpriteStackOffTarget(at(50.0f, 130.0f), pass, mvp));   // base below, top (y 73) in view
    CHECK(SpriteStackOffTarget(at(50.0f, 200.0f), pass, mvp));    // even its top (y 143) is below
    pass.HeightScale = 0.0f;                                    // flat: only the footprint counts
    CHECK(SpriteStackOffTarget(at(50.0f, 130.0f), pass, mvp));
    SpriteStackItem leaning = at(50.0f, 130.0f);
    leaning.Lean = 180.0f; pass.HeightScale = 1.0f;             // climbing down instead: never reaches in
    CHECK(SpriteStackOffTarget(leaning, pass, mvp));
}

// A static list's chunks pick at least every item culling one by one keeps, finer near the eye than
// far off, and an edit moves one item out of view and back without touching the others.
static void TestStaticStacks()
{
    SpriteStack stack;
    stack.Layers = 20; stack.Width = 8; stack.Height = 8;
    stack.Origin = Vector3{ 4.0f, 4.0f, 0.0f };
    stack.Texture.id = 1;                                        // only picked, never drawn
    SpriteStackPass pass;
    pass.ZUp = true;
    pass.LayerHeight = 0.2f;                                     // 0.05 m layers at scale 1/4: 1 m tall, 2 m wide
    std::vector<SpriteStackItem> items;
    for (int i = 0; i < 100; ++i) {
        for (int j = 0; j < 100; ++j) {
            SpriteStackItem item;
            item.Stack = &stack; item.Scale = 0.25f; item.X = i * 10.0f; item.Y = j * 10.0f;
            items.push_back(item);
        }
    }
    items[5].Tint.a = 128;                                       // see-through: DrawSpriteStacks' job
    const Matrix mvp = MatrixMultiply(MatrixLookAt(Vector3{ 150, 150, 120 }, Vector3{ 300, 300, 0 }, Vector3{ 0, 0, 1 }),
                                      MatrixPerspective(40.0 * DEG2RAD, 16.0 / 9.0, 1.0, 5000.0));
    StaticStacksDraw sd{ MakeStaticStacks(), items, {} };
    const auto picks = SelectStaticStacks(sd, pass, mvp, Vector2{ 960, 540 });
    std::vector<int> level(items.size(), -1);
    for (const auto& [i, lod] : picks) level[i] = lod;
    size_t kept = 0;
    bool superset = true;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i == 5 || SpriteStackOffTarget(items[i], pass, mvp)) continue;
        ++kept;
        superset = superset && level[i] >= 0;
    }
    CHECK(kept > 0 && superset && picks.size() < items.size() && level[5] < 0);
    const int near = level[24 * 100 + 24], far = level[55 * 100 + 55];   // 127 m and 566 m from the eye's foot
    CHECK(near >= 0 && far >= 0 && near < far);
    const unsigned int one = 30 * 100 + 30;
    SpriteStackItem moved = items[one];
    moved.X = -4000.0f;                                          // behind the camera
    sd.Edits = { { one, moved } };
    const auto without = SelectStaticStacks(sd, pass, mvp, Vector2{ 960, 540 });
    CHECK(without.size() == picks.size() - 1);
    sd.Edits = { { one, items[one] } };
    CHECK(SelectStaticStacks(sd, pass, mvp, Vector2{ 960, 540 }).size() == picks.size());
}

// A stack being built shows nothing at 0, all of it at 1, bottom layers first, every layer landing.
static void TestSpriteStackBuild()
{
    CHECK(BuildLayer(0.0f, 0, 10).Alpha == 0.0f);
    CHECK(BuildLayer(1.0f, 9, 10).Alpha == 1.0f && BuildLayer(1.0f, 9, 10).Drop == 0.0f);
    const BuildState low = BuildLayer(0.4f, 1, 10), high = BuildLayer(0.4f, 8, 10);
    CHECK(low.Alpha > high.Alpha && low.Drop < high.Drop);         // the bottom is further along
    CHECK(BuildLayer(0.999f, 9, 10).Drop < 0.01f);                 // the top has settled by the end
    SpriteStackItem tree;
    SpriteStackPass pass;
    CHECK(SwayLean(tree, pass) == 0.0f);                           // rigid without Sway
    tree.Sway = 5.0f;
    CHECK(SwayLean(tree, pass) == 0.0f);                           // and without wind
    pass.SwayStrength = 1.0f;
    CHECK(std::fabs(SwayLean(tree, pass)) <= 5.0f && SwayLean(tree, pass) != 0.0f);
}

// Particles age, rise, never sink below the ground, fade, and die; a full pool replaces old ones.
static void TestParticles()
{
    ParticlePool pool;
    pool.Capacity = 2;
    Particle p;
    p.Life = 1.0f;
    p.Velocity = Vector3{ 0.0f, 0.0f, -5.0f };
    EmitParticle(pool, p);
    UpdateParticles(pool, 0.5f);
    CHECK(pool.Live.size() == 1 && pool.Live[0].Position.z == 0.0f);
    CHECK(ParticleLook(pool.Live[0]).y > 0.0f && ParticleLook(pool.Live[0]).y < 1.0f);
    p.Life = 5.0f;
    EmitParticle(pool, p);
    EmitParticle(pool, p);                                         // full: replaces the first
    CHECK(pool.Live.size() == 2 && pool.Live[0].Life == 5.0f);
    UpdateParticles(pool, 6.0f);
    CHECK(pool.Live.empty());
}

// The pointer belongs to the UI over widgets and over panels that block input, nowhere else.
static void TestPointerOverUI()
{
    UICanvas canvas;
    UIPanel* bar = canvas.AddPanel("Bar");
    bar->ResolvedRect = Rectangle{ 100.0f, 100.0f, 200.0f, 50.0f };
    UIPanel* plain = canvas.AddPanel("Plain");
    plain->ResolvedRect = Rectangle{ 400.0f, 100.0f, 200.0f, 50.0f };
    UIButton* button = canvas.AddButton("Button", plain);
    button->ResolvedRect = Rectangle{ 410.0f, 110.0f, 40.0f, 30.0f };

    CHECK(!canvas.PointerOverUIAt(Vector2{ 150.0f, 120.0f }));   // a panel that does not block
    bar->BlocksInputBehind = true;
    CHECK(canvas.PointerOverUIAt(Vector2{ 150.0f, 120.0f }));
    CHECK(!canvas.PointerOverUIAt(Vector2{ 50.0f, 50.0f }));      // empty screen
    CHECK(canvas.PointerOverUIAt(Vector2{ 420.0f, 120.0f }));     // a button
    CHECK(!canvas.PointerOverUIAt(Vector2{ 500.0f, 120.0f }));    // its non-blocking parent beside it
    bar->Visible = false;
    CHECK(!canvas.PointerOverUIAt(Vector2{ 150.0f, 120.0f }));    // hidden panels do not block
}

// engine.video on the fixtures in tests/fixtures (README there): headers, both backends' opening
// paths, a broken file, the state machine without a window, and a full decode of every frame/sample.
static void TestVideo()
{
    const fs::path fixtures = fs::path(__FILE__).parent_path() / "fixtures";
    ResourceStore store(std::make_unique<DevResourceBackend>(fixtures.string()));

    auto sound = std::make_shared<Video>(store, "video_sound.mpg");
    CHECK(sound->GetState() == Video::State::Ready);
    CHECK(sound->Width() == 64 && sound->Height() == 48);
    CHECK(sound->HasAudio());
    // The container's estimate (pl_mpeg reads the last timestamp): short by up to one GOP (12 frames).
    CHECK(sound->Duration() > 0.5f && sound->Duration() <= 1.05f);
    auto silent = std::make_shared<Video>(store, "video_silent.mpg");
    CHECK(silent->GetState() == Video::State::Ready && !silent->HasAudio());

    auto missing = std::make_shared<Video>(store, "nope.mpg");
    CHECK(missing->GetState() == Video::State::Error && !missing->Error().empty());
    const fs::path temp = fs::temp_directory_path() / "iyi_video_test";
    fs::create_directories(temp);
    WriteFile(temp / "broken.mpg", std::string(4096, 'x'));
    ResourceStore tempStore(std::make_unique<DevResourceBackend>(temp.string()));
    auto broken = std::make_shared<Video>(tempStore, "broken.mpg");
    CHECK(broken->GetState() == Video::State::Error);
    // An open loose video never locks its file: a re-export can replace it while it plays.
    fs::copy_file(fixtures / "video_sound.mpg", temp / "open.mpg", fs::copy_options::overwrite_existing);
    auto open = std::make_shared<Video>(tempStore, "open.mpg");
    std::error_code moved;
    fs::rename(temp / "open.mpg", temp / "moved.mpg", moved);
    CHECK(open->GetState() == Video::State::Ready && !moved);
    open.reset();

    // No window and no recording packet: the render-side work is dropped, the game-side state moves.
    SetRecordingPacket(nullptr);
    sound->Play();
    CHECK(sound->GetState() == Video::State::Playing);
    sound->Pause();
    CHECK(sound->GetState() == Video::State::Paused);
    sound->Stop();
    CHECK(sound->GetState() == Video::State::Ready && sound->Position() == 0.0f);
    sound->Close();
    sound->Close();   // safe to repeat
    CHECK(sound->GetState() == Video::State::Closed);
    sound->Play();    // does nothing after Close
    CHECK(sound->GetState() == Video::State::Closed);
    broken->Play();
    CHECK(broken->GetState() == Video::State::Error);

    // The decoder itself: 25 frames in order, and audio samples (from a file, as a loose file plays).
    plm_t* plm = plm_create_with_filename((fixtures / "video_sound.mpg").string().c_str());
    CHECK(plm != nullptr);
    if (plm) {
        int frames = 0, samples = 0;
        double last = -1.0;
        bool ordered = true;
        for (plm_frame_t* f = plm_decode_video(plm); f; f = plm_decode_video(plm), ++frames) {
            ordered = ordered && f->time > last;
            last = f->time;
        }
        plm_rewind(plm);
        plm_set_video_enabled(plm, 0);
        for (plm_samples_t* s = plm_decode_audio(plm); s; s = plm_decode_audio(plm)) samples += s->count;
        CHECK(frames == 25 && ordered);
        CHECK(samples > 40000);   // ~1 s at 44.1 kHz
        plm_destroy(plm);
    }
    std::error_code ec;
    fs::remove_all(temp, ec);
    CHECK(!ec);   // the broken video let go of its file
}

#ifndef IYI_SHIPPING_BUILD
// IYITrailer's pure helpers: frame placement by real time, device rate snapping, argument quoting.
static void TestTrailerHelpers()
{
    long long next = 0;
    CHECK(TrailerFrameRepeats(0.0, 60, next) == 1 && next == 1);         // slot 0
    CHECK(TrailerFrameRepeats(0.010, 60, next) == 0 && next == 1);       // slot 0 again: dropped
    CHECK(TrailerFrameRepeats(1.0 / 60 + 0.001, 60, next) == 1);         // slot 1, on time
    CHECK(TrailerFrameRepeats(5.0 / 60 + 0.001, 60, next) == 4 && next == 6);   // slots 2..5: a hitch repeats
    CHECK(TrailerSnapSampleRate(48000.0) == 48000);
    CHECK(TrailerSnapSampleRate(47100.0) == 48000);
    CHECK(TrailerSnapSampleRate(44300.0) == 44100);
    CHECK(TrailerSnapSampleRate(40000.0) == 0);
    CHECK(TrailerQuoteArgs({ "ffmpeg", "-i", "pipe:0" }) == "ffmpeg -i pipe:0");
    CHECK(TrailerQuoteArgs({ "C:\\My Caps\\çekim.mp4" }) == "\"C:\\My Caps\\çekim.mp4\"");
    CHECK(TrailerQuoteArgs({ "a\\\"b", "" }) == "\"a\\\\\\\"b\" \"\"");   // a\"b -> "a\\\"b", empty -> ""
    CHECK(TrailerQuoteArgs({ "C:\\dir with space\\" }) == "\"C:\\dir with space\\\\\"");   // trailing slash doubles

    // Tracked UI: a still element is one range; a move, a fade or hiding starts the next.
    std::vector<TrailerUiRange> ranges;
    TrailerUiRange at{ 0.0, 0.0, 10, 20, 100, 50, 1.0f, true };
    TrailerAddUiSample(ranges, 0.0, at);
    TrailerAddUiSample(ranges, 0.5, at);
    at.X = 12;
    TrailerAddUiSample(ranges, 0.52, at);
    at.Visible = false;
    TrailerAddUiSample(ranges, 0.54, at);
    CHECK(ranges.size() == 3 && ranges[0].From == 0.0 && ranges[0].To == 0.5 && ranges[1].X == 12 && !ranges[2].Visible);
}
#endif

int TestImeWindows();

// ---------------------------------------------------------------------------
// Navigation (Nav.h): a wall with one gap closed and reopened by an Extra bit, clearance, a bridge
// closed and reopened on the graph, a narrowed edge, and the planner answering from its snapshot.
static void TestSight()
{
    SightGrid g;
    g.SetGround(4, 4, 10.0f, { 0.0f, 0.0f }, std::vector<float>(25, 0.0f));
    CHECK(g.Ground(15.0f, 15.0f) == 0.0f);
    std::vector<float> walls(40 * 40, 0.0f);
    for (int r = 0; r < 40; ++r) walls[r * 40 + 20] = 5.0f;                  // a 5 m wall along x = 20..21
    g.SetBlockers(40, 40, 1.0f, { 0.0f, 0.0f }, walls);
    CHECK(!g.Visible({ 5.0f, 10.0f, 1.6f }, { 35.0f, 10.0f, 1.6f }));       // through the wall
    CHECK(g.Visible({ 5.0f, 10.0f, 12.0f }, { 35.0f, 10.0f, 1.6f }));       // over it from a roof
    CHECK(g.Visible({ 5.0f, 10.0f, 1.6f }, { 15.0f, 30.0f, 1.6f }));        // same side
    Vector3 hit;
    CHECK(g.Ray({ 5.0f, 10.0f, 20.0f }, { 1.0f, 0.0f, -1.0f }, 1000.0f, &hit));
    CHECK(std::fabs(hit.x - 20.0f) < 0.1f && std::fabs(hit.z - 5.0f) < 0.6f); // hits the wall's top edge
    CHECK(g.Ray({ 5.0f, 10.0f, 50.0f }, { 0.0f, 0.0f, -1.0f }, 1000.0f, &hit) && std::fabs(hit.z) < 0.01f);
    g.SetBlockerCells({ 10 * 40 + 20 }, 0.0f);                              // a gap in the wall at row 10
    CHECK(g.Visible({ 5.0f, 10.5f, 1.6f }, { 35.0f, 10.5f, 1.6f }));
    const auto pairs = g.VisiblePairs({ { 5.0f, 10.5f, 1.6f, 100.0f }, { 5.0f, 2.0f, 1.6f, 10.0f } },
                                      { { 35.0f, 10.5f, 1.6f }, { 8.0f, 2.0f, 1.6f } });
    CHECK(pairs.size() == 3);                                               // the short-range observer misses the far target
    std::vector<float> lows(40 * 40, 0.0f), highs(40 * 40, 0.0f);
    for (int c = 8; c < 12; ++c) { lows[30 * 40 + c] = 1.0f; highs[30 * 40 + c] = 6.0f; }   // a 4 m crown, 1 to 6 m up
    g.SetFoliage(lows, highs);
    float leaf = -1.0f;
    CHECK(g.Visible({ 2.0f, 30.5f, 1.6f }, { 18.0f, 30.5f, 1.6f }, &leaf) && std::fabs(leaf - 4.0f) <= 0.6f);   // through it
    CHECK(g.Visible({ 2.0f, 30.5f, 0.5f }, { 18.0f, 30.5f, 0.5f }, &leaf) && leaf == 0.0f);                       // under it
    CHECK(g.Visible({ 2.0f, 25.5f, 1.6f }, { 18.0f, 25.5f, 1.6f }, &leaf) && leaf == 0.0f);                       // beside it
}

static void TestNav()
{
    NavGrid g{ 20, 10, 2.0f, { 0.0f, 0.0f }, std::vector<uint8_t>(200, 0), std::vector<uint8_t>(200, 0) };
    for (int y = 0; y < 10; ++y) if (y != 8) g.Base[y * 20 + 10] = 16;     // a wall at column 10, gap at row 8
    NavGridQuery q;
    q.Start = { 3.0f, 3.0f }; q.Goal = { 37.0f, 3.0f };
    q.Costs.fill(0.0f); q.Costs[0] = 1.0f; q.ExtraCosts.fill(1.0f); q.ExtraCosts[0] = 0.0f;
    NavResult r = FindNavGridPath(g, q);
    CHECK(r.Found && r.Points.size() >= 3);
    bool viaGap = false;
    for (const Vector2& p : r.Points) viaGap = viaGap || p.y > 14.0f;
    CHECK(viaGap);
    CHECK(r.Points.front().x == 3.0f && r.Points.back().x == 37.0f);
    g.Extra[8 * 20 + 10] = 1;                                               // debris in the gap
    CHECK(!FindNavGridPath(g, q).Found);
    g.Extra[8 * 20 + 10] = 0;
    CHECK(FindNavGridPath(g, q).Found);
    q.Clearance = 1;                                                        // a wide unit does not fit the gap
    CHECK(!FindNavGridPath(g, q).Found);
    q.Clearance = 0;
    q.MaxNodes = 3;
    CHECK(!FindNavGridPath(g, q).Found);

    // A river crossing: 0-1 is a short bridge, 0-2-3-1 the long way round.
    NavGraph graph;
    graph.Nodes = { { 0, 0 }, { 100, 0 }, { 0, 300 }, { 100, 300 } };
    graph.Edges = { { 0, 1, 100, 0, false, true, 6, 0, 0 }, { 0, 2, 300, 0, false, true, 6, 0, 0 },
                    { 2, 3, 100, 0, false, true, 6, 0, 0 }, { 3, 1, 300, 0, false, true, 6, 0, 0 } };
    BuildNavLinks(graph);
    NavGraphQuery gq{ { { 0, 0.0f } }, { { 1, 0.0f } }, { 1.0f }, 2.5f, 1.0f, 1000 };
    r = FindNavGraphPath(graph, gq);
    CHECK(r.Found && r.Edges == std::vector<int>{ 0 } && r.Cost == 100.0f);
    graph.Edges[0].Open = false;                                            // bridge destroyed
    r = FindNavGraphPath(graph, gq);
    CHECK(r.Found && (r.Edges == std::vector<int>{ 1, 2, 3 }) && r.Nodes.size() == 4);
    graph.Edges[0].Open = true;                                             // repaired
    CHECK(FindNavGraphPath(graph, gq).Edges == std::vector<int>{ 0 });
    graph.Edges[0].Clear = 2.0f;                                            // a wreck leaves 2 m
    CHECK(FindNavGraphPath(graph, gq).Edges.size() == 3);
    gq.Width = 1.5f;                                                        // a motorbike still fits
    CHECK(FindNavGraphPath(graph, gq).Edges.size() == 1);
    graph.Edges[0].Hazard = 1000.0f;                                        // an ambush on the bridge
    CHECK(FindNavGraphPath(graph, gq).Edges.size() == 3);
    gq.HazardWeight = 0.0f;
    CHECK(FindNavGraphPath(graph, gq).Edges.size() == 1);

    NavPlanner planner;
    planner.Grid = g;
    planner.Graph = graph;
    q.MaxNodes = 500000;
    const int a = planner.Request(q);
    planner.Grid.Extra[8 * 20 + 10] = 1;                                    // after the request: not in its snapshot
    planner.GridChanged = true;
    const int b = planner.Request(q);
    const int c = planner.Request(gq);
    const std::vector<NavResult> out = planner.Poll(true);
    CHECK(out.size() == 3 && out[0].Ticket == a && out[1].Ticket == b && out[2].Ticket == c);
    CHECK(out.size() == 3 && out[0].Found && !out[1].Found && out[2].Found);
    CHECK(planner.Pending() == 0);
}

// ---------------------------------------------------------------------------
// GeoWorld (libraries/geo_world/src/World.h): the former world.py self-check. Raster and frame on a tiny
// synthetic package; conditions, sight inputs, blasts and saves on COIN's committed Jowhar package when
// it is found beside the engine.
static std::string TinyPackage()
{
    // 10 x 10 grid of 1 m cells, a 2 x 2 terrain of 5 m cells (flat), one 4 m square building at (2..6, 2..6).
    return R"({"format":"geo_world","version":1,"name":"tiny","frame":{"lat":2.78,"lon":45.5,"half":5},
"terrain":{"cell":5,"columns":2,"rows":2,"origin":[0,0],"file":"h"},"grid":{"cell":1,"columns":10,"rows":10,"origin":[0,0],"file":"g"},
"sources":{"hash":"x"},"nodes":{},"roads":[],"walls":[],"trees":[],
"buildings":[{"id":"b:1","height":5,"obb":[4,4,4,4,0],"footprint":[[2,2],[6,2],[6,6],[2,6]]}]})";
}

static void TestGeoWorld()
{
    const std::string heights(sizeof(float) * 9, '\0'), grid(100, '\0');
    GeoWorld t("tiny", TinyPackage(), heights, grid);
    const std::vector<NavXY> square{ { 2, 2 }, { 6, 2 }, { 6, 6 }, { 2, 6 } }, hole{ { 3, 3 }, { 5, 3 }, { 5, 5 }, { 3, 5 } };
    CHECK(t.PolygonCells({ square }).size() == 16);
    CHECK(t.PolygonCells({ square, hole }).size() == 12);
    std::vector<int> row(10);
    for (int i = 0; i < 10; ++i) row[i] = i;
    CHECK(t.LineCells({ { 0, 0.5 }, { 10, 0.5 } }, 1.0) == row);
    CHECK(GeoWorld::Inside(square, 3, 3) && GeoWorld::Inside(hole, 4, 4));
    const NavXY ll = t.ToLonLat(t.FromLonLat(45.51, 2.77).x, t.FromLonLat(45.51, 2.77).y);
    CHECK(std::fabs(ll.x - 45.51) < 1e-9 && std::fabs(ll.y - 2.77) < 1e-9);
    const NavXY north = t.FromLonLat(45.5, 2.78 + 1.0 / 60);   // one arc minute north: ~1843 m, negative y
    CHECK(north.y > -1845 && north.y < -1840 && north.x == 0);
    CHECK(t.Flags(3.5, 3.5) == 0 && t.Flags(-1, 0) == -1 && t.BuildingAt(3, 3) && !t.BuildingAt(7, 7));
    CHECK(t.SetCondition("b:1", GeoWorld::Condition::Destroyed) && (t.Flags(3.5, 3.5) & GeoWorld::RUBBLE) && t.Revision() == 1);
    CHECK(t.SetCondition("b:1", GeoWorld::Condition::Intact) && t.Flags(3.5, 3.5) == GeoWorld::BUILDING);   // a standing building blocks its cells

    const char* candidates[] = { "../COIN/assets/maps/jowhar", "../../COIN/assets/maps/jowhar", "../../../COIN/assets/maps/jowhar", "../../../../COIN/assets/maps/jowhar" };
    const char* dir = nullptr;
    for (const char* c : candidates)
        if (fs::exists(std::string(c) + "/world.json")) { dir = c; break; }
    if (!dir) { std::puts("TestGeoWorld: COIN's jowhar package not found, package part skipped"); return; }

    GeoWorld w = GeoWorld::LoadDirectory(dir);
    const GeoWorld::Grid g = w.GridInfo();
    const auto cellCentre = [&](int i) { return NavXY{ g.Origin.x + (i % g.Columns + 0.5) * g.Cell, g.Origin.y + (i / g.Columns + 0.5) * g.Cell }; };
    CHECK(*std::min_element(w.Heights().begin(), w.Heights().end()) >= -1e-3f);
    const GeoWorld::Building* b = nullptr;
    for (const auto& o : w.Buildings())
        if (!w.PolygonCells({ o.Footprint }).empty()) { b = &o; break; }   // its cell centre is in the footprint
    CHECK(b != nullptr);
    if (!b) return;
    const int i = w.BuildingCells(*b)[0];
    const NavXY p = cellCentre(i);
    CHECK(w.BuildingAt(p.x, p.y) && (w.Flags(p.x, p.y) & GeoWorld::BUILDING));
    const std::vector<uint8_t> before(w.Bits(), w.Bits() + g.Columns * g.Rows);
    std::vector<float> blockers = w.BlockerHeights();
    float tallest = 0.0f;
    for (const auto& o : w.Buildings())
        for (int k : w.BuildingCells(o))
            if (k == i) tallest = std::max(tallest, static_cast<float>(o.Height));
    CHECK(blockers[i] >= tallest - 1e-4f);                                 // float32
    w.SetCondition(b->Id, GeoWorld::Condition::Destroyed);
    CHECK((w.Flags(p.x, p.y) & (GeoWorld::RUBBLE | GeoWorld::BUILDING)) && w.Revision() == 1);   // BUILDING if a neighbour overlaps

    // A SightGrid kept by UpdateSight matches a rebuilt one's blockers.
    std::vector<float> kept = blockers;
    GeoWorld::SightSink sink;
    sink.SetBlockerCells = [&](const std::vector<int>& cells, double h) { for (int k : cells) kept[k] = static_cast<float>(h); };
    CHECK(w.UpdateSight(sink, 0) == 1 && kept == w.BlockerHeights());
    const GeoWorld::Road* bridge = nullptr;
    for (const auto& r : w.Roads())
        if (r.Bridge) { bridge = &r; break; }
    if (bridge) { w.SetCondition(bridge->Id, GeoWorld::Condition::Destroyed); CHECK(!w.RoadOpen(bridge->Id)); }
    w.AddMark(GeoWorld::MakeMark(0, 1.0, 2.0, 6.0, 6.0, 30.0, 0.8, 12.0));
    const GeoWorld::State saved = w.SaveState();
    GeoWorld w2 = GeoWorld::LoadDirectory(dir);
    CHECK(w2.LoadState(saved).empty() && std::equal(w2.Bits(), w2.Bits() + g.Columns * g.Rows, w.Bits()));
    CHECK(w2.Marks() == w.Marks() && w2.MarksGeneration() == 1);
    for (const auto& [id, c] : std::map<std::string, GeoWorld::Condition>(w.Conditions())) w.SetCondition(id, GeoWorld::Condition::Intact);
    CHECK(std::equal(before.begin(), before.end(), w.Bits()));            // every condition back to intact restores the package grid
    CHECK(w.ChangesSince(0).size() == static_cast<size_t>(w.Revision()));
    const GeoWorld::Building& first = w.Buildings()[0];
    const auto near = w.BuildingsNear(first.Centre.x, first.Centre.y, 0.0);
    const auto wide = w.BuildingsNear(first.Centre.x, first.Centre.y, 30.0);
    CHECK(std::find(near.begin(), near.end(), first.Id) != near.end());
    for (const auto& id : near) CHECK(std::find(wide.begin(), wide.end(), id) != wide.end());
    bool onFree = true, blocking = true;
    for (const auto& o : w.Buildings()) {
        bool any = false;
        for (int k : w.BuildingCells(o)) any = any || (w.Bits()[k] & GeoWorld::BUILDING);
        blocking = blocking && any;                                        // every building blocks a cell
    }
    for (const auto& tr : w.Trees()) onFree = onFree && !(w.Flags(tr.X, tr.Y) & (GeoWorld::BUILDING | GeoWorld::ROAD | GeoWorld::WATER | GeoWorld::WALL));
    CHECK(blocking && onFree);

    const GeoWorld::Tree* tree = nullptr;
    int treeIndex = 0;
    for (size_t k = 0; k < w.Trees().size(); ++k)
        if (w.Trees()[k].Base > 0) { tree = &w.Trees()[k]; treeIndex = static_cast<int>(k); break; }
    CHECK(tree != nullptr);
    if (!tree) return;
    const int cell = static_cast<int>((tree->Y - g.Origin.y) / g.Cell) * g.Columns + static_cast<int>((tree->X - g.Origin.x) / g.Cell);
    blockers = w.BlockerHeights();
    CHECK(blockers[cell] >= tree->Base - 1e-4);                            // a trunk blocks sight up to its crown
    const auto [lows, highs] = w.Foliage();
    CHECK(highs[cell] >= tree->Height - 1e-4 && lows[cell] <= tree->Base + 1e-4);   // its crown is a leaf band
    const int rev = w.Revision();
    const double tx = tree->X, ty = tree->Y;
    CHECK(w.Blast(tx, ty, 6.0) >= 1 && w.Revision() == rev + 1 && !w.Scorches().empty());
    std::vector<float> bandLows, bandHighs;
    GeoWorld::SightSink leaves;
    leaves.SetBlockerCells = [&](const std::vector<int>& cells, double h) { for (int k : cells) blockers[k] = static_cast<float>(h); };
    leaves.SetFoliage = [&](const std::vector<float>& l, const std::vector<float>& h) { bandLows = l; bandHighs = h; };
    CHECK(w.UpdateSight(leaves, rev) == rev + 1);
    const std::vector<float> fresh = w.BlockerHeights();
    bool same = true;
    for (int k : w.LineCells({ { tx, ty }, { tx, ty } }, 12.0)) same = same && blockers[k] == fresh[k];
    CHECK(same);
    const auto full = w.Foliage();
    CHECK(bandLows == full.first && bandHighs == full.second);             // the leaf bands redone round the blast match a full rebuild
    GeoWorld w3 = GeoWorld::LoadDirectory(dir);
    w3.LoadState(w.SaveState());
    CHECK(w3.TreeStates() == w.TreeStates() && w3.Scorches().size() == w.Scorches().size());
    w.Blast(tx + 4.0, ty, 6.0);                                            // it would burn there, but it is already snapped
    CHECK(w.TreeStates().at(treeIndex) == GeoWorld::TreeState::Broken);
}

int main()
{
    TestGlesShaderSource();
    g_failures += TestImeWindows();
    TestLibraryRoots();
    TestNativeDirs();
    TestProfiler();
    TestProfilerThreads();
    TestHangWatchdog();
    TestDaylight();
    TestRtsSelect();
    TestProfilerOverlay();
    TestFramePacket();
    TestSpriteStackPacking();
    TestSpriteStackCulling();
    TestStaticStacks();
    TestSpriteStackBuild();
    TestParticles();
    TestNav();
    TestSight();
    TestGeoWorld();
    TestPointerOverUI();
    TestTiltedCameraMatrix();
    TestPixelFontSnap();
    TestFontGroupSize();
    TestFitHeightWrapAndStyles();
    TestResourcePaths();
    TestDevBackendKeepsCase();
    TestDevBackendContainment();
    TestPackagedLookup();
    TestFixedStep();
    TestTweensAndSequences();
    TestRetiredElements();
    TestReclaimRetired();
    TestPasteFilter();
    TestUtf8Text();
    TestSteamHelpers();
    TestVideo();
#ifdef IYI_NET
    g_failures += RunNetTests();
#endif
#ifdef IYI_WITH_PYTHON
    TestHelperReloadIsAtomic();
#endif
#ifndef IYI_SHIPPING_BUILD
    TestTrailerHelpers();
#endif
    if (g_failures == 0) std::puts("engine_tests: all passed");
    else std::fprintf(stderr, "engine_tests: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
