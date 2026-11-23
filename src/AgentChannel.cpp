// Implements the agent channel (AgentChannel.h, SPEC §3.1). A request is read and deleted, handed
// to the entry script's AgentRequest hook, then the game runs the requested frames/seconds so the
// change is visible; the screenshot is taken in the last of those frames and the reply is written
// one frame later, after the image file exists. The reply is written to a temporary name and renamed,
// so an agent that sees <id>.json can read it whole.

#include "IYI/AgentChannel.h"
#include "IYI/FrameInput.h"
#include "IYI/ImageCompare.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <nlohmann/json.hpp>
#include "IYI/Application.h"
#include "IYI/ActorRegistry.h"
#include "IYI/IYIUI/IYIUI.h"
#include "IYI/Scene.h"
#include "IYI/Log.h"
#include "IYI/LocalizationManager.h"
#include "IYI/Profiler.h"

namespace fs = std::filesystem;

#ifdef _WIN32
// psapi's PROCESS_MEMORY_COUNTERS, declared here so windows.h (which clashes with raylib) stays out.
struct IyiProcessMemory { unsigned long cb, PageFaultCount; size_t PeakWorkingSet, WorkingSet, QuotaPeakPaged,
    QuotaPaged, QuotaPeakNonPaged, QuotaNonPaged, Private, PeakPrivate; };
extern "C" __declspec(dllimport) void* __stdcall GetCurrentProcess();
extern "C" __declspec(dllimport) int __stdcall K32GetProcessMemoryInfo(void* process, IyiProcessMemory* counters, unsigned long cb);
// toolhelp's PROCESSENTRY32W and the calls WatchLauncher needs, the same way.
struct IyiProcessEntry { unsigned long Size, Usage, ProcessId; unsigned long long DefaultHeapId; unsigned long ModuleId,
    Threads, ParentProcessId; long PriorityBase; unsigned long Flags; wchar_t ExeFile[260]; };
extern "C" __declspec(dllimport) void* __stdcall CreateToolhelp32Snapshot(unsigned long flags, unsigned long processId);
extern "C" __declspec(dllimport) int __stdcall Process32FirstW(void* snapshot, IyiProcessEntry* entry);
extern "C" __declspec(dllimport) int __stdcall Process32NextW(void* snapshot, IyiProcessEntry* entry);
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentProcessId();
extern "C" __declspec(dllimport) void* __stdcall OpenProcess(unsigned long access, int inherit, unsigned long processId);
extern "C" __declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void* handle, unsigned long ms);
extern "C" __declspec(dllimport) int __stdcall CloseHandle(void* handle);
#else
#include <unistd.h>
#endif

namespace IYI {

namespace {

constexpr float kPollSeconds = 0.25f;
constexpr int kIdleWaitsToQuit = 10;   // lockstep: 10 minutes with no request, the agent is gone

// Once: the launcher has ended, so this agent run quits (logged).
bool LauncherLeft(AgentChannel& channel)
{
    if (!channel.LauncherGone || !channel.LauncherGone()) return false;
    channel.LauncherGone = nullptr;
    LogWrite(LogLevel::Warn, "agent", "the process that started this agent run has ended: quitting");
    CurrentApp().RequestQuit();
    return true;
}

fs::path RequestPath(const AgentChannel& channel)
{
    return fs::path(channel.Dir) / "request.json";
}

void WriteAtomically(const fs::path& path, const std::string& text)
{
    const fs::path temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary);
        out << text;
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) LogWrite(LogLevel::Warn, "agent", "could not write " + path.generic_string() + ": " + ec.message());
}

// The first live canvas element of that name, laid out as this frame draws it.
UI::UIElement* FindUi(const std::string& name, UI::UICanvas** canvasOut = nullptr)
{
    for (const auto& actor : CurrentApp().GetScene().Actors.GetActors()) {
        auto* canvas = dynamic_cast<UI::UICanvas*>(actor.get());
        if (!canvas || actor->PendingDestroy) continue;
        canvas->ResolveLayoutTree();
        if (UI::UIElement* e = canvas->Find(name)) {
            if (canvasOut) *canvasOut = canvas;
            return e;
        }
    }
    return nullptr;
}

bool ShownOnScreen(const UI::UIElement* e)
{
    for (; e; e = e->Parent) if (!e->Visible) return false;
    return true;
}

// raylib key codes by name, for "key": "ESCAPE" (a number is taken as the code itself).
int KeyCode(const nlohmann::json& key)
{
    if (key.is_number_integer()) {   // a raylib code; outside the input bitsets is unknown
        const int code = key.get<int>();
        return code > 0 && code < 512 ? code : 0;
    }
    if (!key.is_string()) return 0;
    std::string k = key.get<std::string>();
    for (char& c : k) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (k.rfind("KEY_", 0) == 0) k = k.substr(4);
    if (k.size() == 1 && ((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= '0' && k[0] <= '9'))) return k[0];
    if (k.size() >= 2 && k[0] == 'F' && std::isdigit(static_cast<unsigned char>(k[1]))) {
        const int n = std::atoi(k.c_str() + 1);
        if (n >= 1 && n <= 12) return KEY_F1 + n - 1;
    }
    static const std::pair<const char*, int> names[] = {
        { "ESCAPE", KEY_ESCAPE }, { "ENTER", KEY_ENTER }, { "SPACE", KEY_SPACE }, { "TAB", KEY_TAB },
        { "BACKSPACE", KEY_BACKSPACE }, { "DELETE", KEY_DELETE }, { "LEFT", KEY_LEFT }, { "RIGHT", KEY_RIGHT },
        { "UP", KEY_UP }, { "DOWN", KEY_DOWN }, { "HOME", KEY_HOME }, { "END", KEY_END },
        { "PAGE_UP", KEY_PAGE_UP }, { "PAGE_DOWN", KEY_PAGE_DOWN }, { "LEFT_SHIFT", KEY_LEFT_SHIFT },
        { "LEFT_CONTROL", KEY_LEFT_CONTROL }, { "LEFT_ALT", KEY_LEFT_ALT }, { "RIGHT_SHIFT", KEY_RIGHT_SHIFT },
        { "RIGHT_CONTROL", KEY_RIGHT_CONTROL }, { "RIGHT_ALT", KEY_RIGHT_ALT } };
    for (const auto& [name, code] : names) if (k == name) return code;
    return 0;
}

// Where an action aimed at `spec` lands, in window pixels: an element name (its centre, through its
// transform, the canvas scale and the postprocess radial warp) or [x, y]. With `check`, an element a
// player could not click is refused instead: {"element", "error"} with hidden (it or an ancestor), ambiguous
// (several live shown elements of that name and no `scope` ancestor), disabled, offscreen, or occluded
// (the topmost hit at its centre is neither it nor a descendant: a modal or popup on top).
nlohmann::json ResolvePoint(const nlohmann::json& spec, const std::string& scope, bool check, Vector2& p)
{
    if (spec.is_array() && spec.size() == 2 && spec[0].is_number() && spec[1].is_number()) {
        p = { spec[0].get<float>(), spec[1].get<float>() };
        return { { "screen", { p.x, p.y } } };
    }
    if (!spec.is_string()) return { { "error", "target is neither a name nor [x, y]" } };
    const std::string name = spec.get<std::string>();
    UI::UIElement* e = nullptr;
    UI::UICanvas* canvas = nullptr;
    bool eShown = false;
    int shown = 0;
    for (const auto& actor : CurrentApp().GetScene().Actors.GetActors()) {
        auto* cv = dynamic_cast<UI::UICanvas*>(actor.get());
        if (!cv || actor->PendingDestroy) continue;
        cv->ResolveLayoutTree();
        UI::UIElement* c = cv->Find(name);
        if (!c) continue;
        if (!scope.empty()) {
            const UI::UIElement* up = c->Parent;
            while (up && up->Name != scope) up = up->Parent;
            if (!up) continue;
        }
        const bool vis = cv->Visible && ShownOnScreen(c);
        shown += vis;
        if (!e || (vis && !eShown)) { e = c; canvas = cv; eShown = vis; }
    }
    if (!e) return { { "element", name }, { "error", "no such element" } };
    const Rectangle r = e->ResolvedRect;
    const Vector2 logical = e->ResolvedTransform.Apply({ r.x + r.width * 0.5f, r.y + r.height * 0.5f });
    const float sf = canvas->ScaleFactor();
    p = UI::WarpedScreenPoint({ logical.x * sf, logical.y * sf });
    if (check) {
        bool disabled = false;
        for (const UI::UIElement* up = e; up; up = up->Parent) disabled = disabled || !up->Enabled;
        const UI::UIElement* top = eShown ? canvas->PickElement(logical) : nullptr;
        const UI::UIElement* own = top;   // top, or an ancestor of it, is e: the click lands on e's subtree
        while (own && own != e) own = own->Parent;
        const char* why = !eShown ? "hidden" : (shown > 1 && scope.empty()) ? "ambiguous" : disabled ? "disabled"
            : (p.x < 0 || p.y < 0 || p.x >= GameInput().ScreenWidth || p.y >= GameInput().ScreenHeight) ? "offscreen"
            : (top && !own) ? "occluded" : nullptr;
        if (why) return { { "element", name }, { "error", why } };
    }
    return { { "element", name }, { "screen", { p.x, p.y } }, { "visible", eShown }, { "enabled", e->Enabled } };
}

AgentInputStep MouseStep(int mouse, Vector2 p)
{
    AgentInputStep step;
    step.Mouse = mouse;
    step.X = p.x; step.Y = p.y;
    return step;
}

// A left click at the target, through the normal pointer path (hit test, pressed style,
// clicked, sounds, focus): press on one frame, release on the next. Nothing is queued when refused.
nlohmann::json QueueClick(AgentChannel& channel, const nlohmann::json& spec, const std::string& scope)
{
    Vector2 p{};
    nlohmann::json out = ResolvePoint(spec, scope, true, p);
    if (!out.contains("error")) {
        channel.Input.push_back(MouseStep(1, p));
        channel.Input.push_back(MouseStep(2, p));
    }
    return out;
}

// "click": "Name", "type": {"element", "text"} (clicks it first, then types), "key": "ESCAPE" or
// {"key", "hold_frames", "modifiers"}, "scroll": {"element", "amount"}, "drag": {"from", "to", "frames"};
// "scope" names an ancestor that tells apart elements sharing a name.
void QueueInput(AgentChannel& channel, const nlohmann::json& request)
{
    channel.Input.clear();
    channel.InputNext = 0;
    nlohmann::json out = nlohmann::json::object();
    const std::string scope = request.value("scope", std::string());
    if (request.contains("click") && request["click"].is_string()) out["click"] = QueueClick(channel, request["click"], scope);
    if (request.contains("scroll") && request["scroll"].is_object()) {
        const nlohmann::json& sc = request["scroll"];
        Vector2 p{};
        nlohmann::json r = ResolvePoint(sc.value("element", nlohmann::json()), scope, true, p);
        if (!r.contains("error")) {
            AgentInputStep step = MouseStep(4, p);
            step.Wheel = sc.value("amount", 0.0f);
            channel.Input.push_back(step);
            r["amount"] = step.Wheel;
        }
        out["scroll"] = r;
    }
    if (request.contains("drag") && request["drag"].is_object()) {
        const nlohmann::json& d = request["drag"];
        Vector2 a{}, b{};
        nlohmann::json from = ResolvePoint(d.value("from", nlohmann::json()), scope, true, a);
        nlohmann::json to = from.contains("error") ? nlohmann::json::object() : ResolvePoint(d.value("to", nlohmann::json()), scope, false, b);
        const int frames = std::max(1, d.value("frames", 10));
        if (from.contains("error")) out["drag"] = from;
        else if (to.contains("error")) out["drag"] = to;
        else {
            channel.Input.push_back(MouseStep(1, a));
            for (int i = 1; i <= frames; ++i) {
                const float t = static_cast<float>(i) / frames;
                channel.Input.push_back(MouseStep(3, { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t }));
            }
            channel.Input.push_back(MouseStep(2, b));
            out["drag"] = { { "from", from }, { "to", to }, { "frames", frames } };
        }
    }
    if (request.contains("type") && request["type"].is_object()) {
        const nlohmann::json& t = request["type"];
        nlohmann::json r = t.contains("element") && t["element"].is_string() ? QueueClick(channel, t["element"], scope) : nlohmann::json::object();
        if (!r.empty()) out["type"] = r;
        AgentInputStep chars;
        const std::string text = t.value("text", std::string());
        for (size_t i = 0; i < text.size();) {   // UTF-8 to code points, as raylib's char queue gives them
            int cp = static_cast<unsigned char>(text[i]), extra = 0;
            if (cp >= 0xF0) { cp &= 0x07; extra = 3; } else if (cp >= 0xE0) { cp &= 0x0F; extra = 2; }
            else if (cp >= 0xC0) { cp &= 0x1F; extra = 1; }
            for (++i; extra > 0 && i < text.size(); --extra, ++i) cp = (cp << 6) | (text[i] & 0x3F);
            chars.Chars.push_back(cp);
        }
        if (!r.contains("error")) channel.Input.push_back(chars);
    }
    if (request.contains("key")) {
        const nlohmann::json& k = request["key"];
        const int code = KeyCode(k.is_object() ? k.value("key", nlohmann::json()) : k);
        std::vector<int> mods;
        bool modsOk = true;
        if (k.is_object() && k.contains("modifiers") && k["modifiers"].is_array()) {
            for (const auto& m : k["modifiers"]) {
                mods.push_back(KeyCode(m));
                modsOk = modsOk && mods.back() != 0;
            }
        }
        if (code == 0 || !modsOk) {
            out["key"] = { { "error", code == 0 ? "unknown key" : "unknown modifier" } };
        } else {
            const int hold = k.is_object() ? std::max(1, k.value("hold_frames", 1)) : 1;
            AgentInputStep step;
            step.Key = code;
            step.Mods = mods;
            channel.Input.push_back(step);   // pressed
            step.KeyHold = true;
            for (int i = 1; i < hold; ++i) channel.Input.push_back(step);
            step.KeyHold = false;
            step.KeyUp = true;
            channel.Input.push_back(step);   // released
            out["key"] = code;
        }
    }
    channel.InputReply = out.empty() ? std::string() : out.dump();
    // the outputs wait until every step has run and the game has seen its effect for a frame
    channel.FramesLeft = std::max(channel.FramesLeft, static_cast<int>(channel.Input.size()) + 2);
}

// "ui": every named element of every canvas, as this frame draws it: what is on screen, by numbers.
// The layout facts the visual tests' UI rules read (docs/VISUAL-TESTS.md): where the element sits in its
// parent and on screen, and how its text fits its font and box.
void UiDumpLayout(UI::UICanvas& canvas, const UI::UIElement& e, const std::string& text, nlohmann::json& entry)
{
    const Rectangle r = e.ResolvedRect;
    if (e.Parent && e.Parent->Type != UI::UIElementType::Root) {
        const Rectangle p = e.Parent->ResolvedRect;
        entry["parent"] = e.Parent->Name;
        entry["parent_rect"] = { p.x, p.y, p.width, p.height };
    }
    for (const UI::UIElement* up = e.Parent; up; up = up->Parent) {
        if (up->Type == UI::UIElementType::ScrollView || up->ClipChildren) { entry["clipped"] = true; break; }
    }
    entry["canvas_size"] = { canvas.LogicalWidth(), canvas.LogicalHeight() };
    const float sf = canvas.ScaleFactor();
    const Vector2 a = e.ResolvedTransform.Apply({ r.x, r.y }), b = e.ResolvedTransform.Apply({ r.x + r.width, r.y + r.height });
    entry["screen_rect"] = { std::min(a.x, b.x) * sf, std::min(a.y, b.y) * sf, std::fabs(b.x - a.x) * sf, std::fabs(b.y - a.y) * sf };
    if (e.Type != UI::UIElementType::Text && e.Type != UI::UIElementType::Button) return;
    auto& editable = const_cast<UI::UIElement&>(e);   // the metrics are memoized, not changed
    const bool isText = e.Type == UI::UIElementType::Text;
    entry["measured_width"] = isText ? static_cast<UI::UIText&>(editable).MeasuredWidth() : static_cast<UI::UIButton&>(editable).MeasuredWidth();
    entry["fitted_font_size"] = isText ? static_cast<UI::UIText&>(editable).FittedFontSize() : static_cast<UI::UIButton&>(editable).FittedFontSize();
    if (e.CustomStyle) entry["font_size"] = e.CustomStyle->FontSize;
    else if (!e.StyleClass.empty()) entry["font_size"] = canvas.Theme().Resolve(e.StyleClass).FontSize;
    if (text.empty()) return;
    const std::string missing = canvas.MissingGlyphs(e, text);
    if (!missing.empty()) entry["missing_glyphs"] = missing;
}

nlohmann::json UiDump()
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& actor : CurrentApp().GetScene().Actors.GetActors()) {
        auto* canvas = dynamic_cast<UI::UICanvas*>(actor.get());
        if (!canvas || actor->PendingDestroy || !canvas->Visible) continue;
        canvas->ResolveLayoutTree();
        std::vector<const UI::UIElement*> stack = { canvas->Root() };
        while (!stack.empty()) {
            const UI::UIElement* e = stack.back();
            stack.pop_back();
            if (!e->Visible) continue;   // a hidden element hides its subtree
            for (auto it = e->Children.rbegin(); it != e->Children.rend(); ++it) stack.push_back(it->get());
            if (e->Name.empty() || e->Type == UI::UIElementType::Root) continue;
            const Rectangle r = e->ResolvedRect;
            nlohmann::json entry = { { "name", e->Name }, { "type", e->TypeName() },
                                     { "rect", { r.x, r.y, r.width, r.height } }, { "opacity", e->ResolvedOpacity } };
            if (!e->Enabled) entry["enabled"] = false;
            std::string text;   // as drawn: a localized key resolved
            if (e->Type == UI::UIElementType::Text) text = static_cast<const UI::UIText*>(e)->ResolvedText();
            if (e->Type == UI::UIElementType::Button) text = static_cast<const UI::UIButton*>(e)->ResolvedText();
            if (e->Type == UI::UIElementType::TextInput) text = static_cast<const UI::UITextInput*>(e)->Text;
            if (!text.empty()) entry["text"] = text;
            UiDumpLayout(*canvas, *e, text, entry);
            out.push_back(std::move(entry));
        }
    }
    return out;
}

// "ui_problems": the UI rules (UICanvas::Problems, docs/VISUAL-TESTS.md) on every shown canvas.
nlohmann::json UiProblems()
{
    nlohmann::json out = nlohmann::json::array();
    for (const auto& actor : CurrentApp().GetScene().Actors.GetActors()) {
        auto* canvas = dynamic_cast<UI::UICanvas*>(actor.get());
        if (!canvas || actor->PendingDestroy || !canvas->Visible) continue;
        for (const auto& p : canvas->Problems()) {
            const Rectangle r = p.Element->ResolvedRect;
            out.push_back({ { "kind", p.Kind }, { "element", p.Element->Name },
                            { "rect", { r.x, r.y, r.width, r.height } }, { "detail", p.Detail } });
        }
    }
    return out;
}

// Reads and deletes a waiting request. A file that does not parse yet is left for the next poll:
// the agent may still be writing it.
bool TakeRequest(AgentChannel& channel)
{
    std::error_code ec;
    const fs::path path = RequestPath(channel);
    if (!fs::exists(path, ec)) return false;
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    nlohmann::json request = nlohmann::json::parse(text, nullptr, false);
    if (request.is_discarded() || !request.is_object()) return false;
    fs::remove(path, ec);

    channel.Pending = true;
    channel.Id = request.value("id", std::string("out"));
    for (char& c : channel.Id) {   // the id names files inside agentio/, never a path
        if (c == '/' || c == '\\' || c == ':' || c == '.') c = '_';
    }
    channel.Screenshot = request.value("screenshot", false);
    channel.Profile = request.value("profile", false);
    channel.Health = request.value("health", false);
    channel.Quit = request.value("quit", false);
    channel.Image = request.contains("regions") || request.contains("compare") || request.contains("pixels");
    channel.Request = request.dump();
    channel.Points.clear();
    channel.TrackNames.clear();
    channel.TrackFrames.clear();
    if (request.contains("track_ui") && request["track_ui"].is_array()) {
        for (const auto& name : request["track_ui"]) {
            if (name.is_string()) channel.TrackNames.push_back(name.get<std::string>());
        }
    }
    channel.LogMark = LogWarnCount() + LogErrorCount();
    channel.EventMark = LogEventCount();
    channel.FramesLeft = std::max(2, request.value("wait_frames", 2));
    channel.SecondsLeft = request.value("wait_seconds", 0.0f);
    channel.ShotRequested = false;
    if (request.contains("scene") && request["scene"].is_string()) {   // applied at the end of this frame
        CurrentApp().RequestSceneChange(request["scene"].get<std::string>());
    }
    // A UI scenario's viewport and language: the window as --window-size pins it, the language as
    // engine.loc.set_language sets it. The game sees both before this request's input.
    const nlohmann::json size = request.value("window_size", nlohmann::json());
    if (size.is_array() && size.size() == 2 && size[0].is_number_integer() && size[1].is_number_integer()) {
        CurrentApp().PinWindowSize(size[0].get<int>(), size[1].get<int>());
    }
    if (request.contains("locale") && request["locale"].is_string()) {
        LocalizationManager::Get().SetLanguage(request["locale"].get<std::string>());
    }
    QueueInput(channel, request);
    channel.UiDump = request.value("ui", false);
    channel.UiProblems = request.value("ui_problems", false);
    channel.Waiting = channel.WaitMet = false;
    channel.WaitFrames = 0;
    channel.WaitScene.clear();
    channel.WaitVisible.clear();
    if (request.contains("wait_until") && request["wait_until"].is_object()) {
        const nlohmann::json& w = request["wait_until"];
        channel.WaitScene = w.value("scene", std::string());
        channel.WaitVisible = w.value("visible", std::string());
        channel.WaitTimeout = std::max(1, w.value("timeout_frames", 600));
        channel.Waiting = true;
    }
    channel.GameReply = CurrentApp().CallAgentRequest(request.dump());
    LogWrite(LogLevel::Info, "agent", "request '" + channel.Id + "'");
    return true;
}

// The engine's own view of the running game, for an agent finding out why it misbehaves.
nlohmann::json Health(AgentChannel& channel)
{
    nlohmann::json health;
    health["uptime_s"] = channel.Seconds;
    health["frames"] = channel.Frames;

    const int n = std::min(channel.Frames, static_cast<int>(channel.FrameMs.size()));
    float sum = 0.0f, worst = 0.0f;
    int slow = 0;
    for (int i = 0; i < n; ++i) {
        sum += channel.FrameMs[i];
        worst = std::max(worst, channel.FrameMs[i]);
        if (channel.FrameMs[i] > 50.0f) ++slow;
    }
    health["frame_ms"] = { { "frames", n }, { "avg", n ? sum / n : 0.0f }, { "max", worst }, { "over_50ms", slow } };

    const FrameInput& in = GameInput();   // the window as the main thread saw it this frame
    health["window"] = { { "focused", in.Focused }, { "minimized", in.Minimized },
                         { "focus_changes", channel.FocusChanges }, { "screen", { in.ScreenWidth, in.ScreenHeight } },
                         { "render", { in.RenderWidth, in.RenderHeight } }, { "monitor_hz", in.MonitorRefreshRate } };

    health["memory"] = nullptr;
#ifdef _WIN32
    IyiProcessMemory mem{};
    mem.cb = sizeof(mem);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &mem, sizeof(mem))) {
        const double privateMb = mem.Private / (1024.0 * 1024.0);
        nlohmann::json memory = { { "private_mb", privateMb }, { "working_set_mb", mem.WorkingSet / (1024.0 * 1024.0) },
                                  { "peak_private_mb", mem.PeakPrivate / (1024.0 * 1024.0) } };
        // Growth since the previous health report: steady growth while idle is a leak.
        const double minutes = (channel.Seconds - channel.LastMemSeconds) / 60.0;
        memory["growth_mb_per_min"] = channel.LastMemSeconds >= 0.0 && minutes > 0.0
            ? nlohmann::json((privateMb - channel.LastPrivateMb) / minutes) : nlohmann::json(nullptr);
        channel.LastMemSeconds = channel.Seconds;
        channel.LastPrivateMb = privateMb;
        health["memory"] = memory;
    }
#endif

    const AssetStats assets = CurrentApp().GetAssets().Stats();
    health["assets"] = { { "textures", assets.Textures }, { "sprite_stacks", assets.SpriteStacks },
                         { "render_textures", assets.RenderTextures }, { "fonts", assets.Fonts },
                         { "shaders", assets.Shaders }, { "sounds", assets.Sounds }, { "music", assets.Music },
                         { "gpu_mb_estimate", assets.GpuMb } };
    health["scene_changes"] = CurrentApp().SceneChanges();
    health["log"] = { { "warnings", LogWarnCount() }, { "errors", LogErrorCount() }, { "recent", LogRecentLines() } };
    return health;
}

// The warnings and errors logged since the request came in (shader compile errors included: raylib's
// warnings go to the log), so an agent sees a failure without asking for health or reading the log.
nlohmann::json NewLog(const AgentChannel& channel)
{
    const int added = LogWarnCount() + LogErrorCount() - channel.LogMark;
    nlohmann::json lines = nlohmann::json::array();
    if (added > 0) {
        std::vector<std::string> recent = LogRecentLines();
        for (auto it = recent.rbegin(); it != recent.rend() && static_cast<int>(lines.size()) < added; ++it) {
            if (it->find("[WARN ]") != std::string::npos || it->find("[ERROR]") != std::string::npos) lines.insert(lines.begin(), *it);
        }
    }
    return { { "new_problems", std::max(0, added) }, { "lines", lines } };
}

// "pixels" as given, each with where it lies on screen now (null: behind the camera). A point is
// {"world": [x, y, z]} (also a bare [x, y, z]) or {"screen": [x, y]} in window pixels.
std::string LocatePoints(const nlohmann::json& points)
{
    nlohmann::json out = nlohmann::json::array();
    if (!points.is_array()) return out.dump();
    for (const nlohmann::json& p : points) {
        const nlohmann::json& world = p.is_array() ? p : p.value("world", nlohmann::json());
        nlohmann::json screen = nullptr;
        if (p.is_object() && p.contains("screen") && p["screen"].size() == 2) {
            screen = p["screen"];
        } else if (world.is_array() && world.size() == 3) {
            Vector2 at{};
            const Vector3 w{ world[0].get<float>(), world[1].get<float>(), world[2].get<float>() };
            if (CurrentApp().WorldToScreen(w, at)) screen = { at.x, at.y };
        }
        out.push_back({ { "point", p }, { "screen", screen } });
    }
    return out.dump();
}

float Luma(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// The image keys, measured on the saved frame (0..255 values, rows top first).
void Measure(const AgentChannel& channel, nlohmann::json& reply)
{
    const nlohmann::json request = nlohmann::json::parse(channel.Request, nullptr, false);
    const fs::path dir(channel.Dir);
    Image image = LoadImage((dir / (channel.Id + ".png")).string().c_str());
    if (!image.data) {
        reply["image_error"] = "the frame's image could not be read";
        return;
    }
    ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    const int w = image.width, h = image.height;
    const auto* px = static_cast<const unsigned char*>(image.data);
    reply["image"] = { { "file", channel.Id + ".png" }, { "size", { w, h } } };

    if (request.contains("regions")) {
        const nlohmann::json& r = request["regions"];
        const int cols = r.is_array() && r.size() == 2 ? std::max(1, r[0].get<int>()) : 8;
        const int rows = r.is_array() && r.size() == 2 ? std::max(1, r[1].get<int>()) : 4;
        nlohmann::json cells = nlohmann::json::array();
        double all[3] = { 0.0, 0.0, 0.0 };
        for (int cy = 0; cy < rows; ++cy) {
            for (int cx = 0; cx < cols; ++cx) {
                const int x0 = cx * w / cols, x1 = (cx + 1) * w / cols, y0 = cy * h / rows, y1 = (cy + 1) * h / rows;
                double sum[3] = { 0.0, 0.0, 0.0 };
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        const unsigned char* p = px + (static_cast<size_t>(y) * w + x) * 4;
                        for (int k = 0; k < 3; ++k) sum[k] += p[k];
                    }
                }
                const double n = std::max(1, (x1 - x0) * (y1 - y0));
                for (int k = 0; k < 3; ++k) all[k] += sum[k];
                const float c[3] = { static_cast<float>(sum[0] / n), static_cast<float>(sum[1] / n), static_cast<float>(sum[2] / n) };
                cells.push_back({ std::lround(c[0]), std::lround(c[1]), std::lround(c[2]), std::lround(Luma(c[0], c[1], c[2])) });
            }
        }
        const double n = static_cast<double>(w) * h;
        const float m[3] = { static_cast<float>(all[0] / n), static_cast<float>(all[1] / n), static_cast<float>(all[2] / n) };
        reply["regions"] = { { "grid", { cols, rows } }, { "cells", cells },
                             { "mean", { std::lround(m[0]), std::lround(m[1]), std::lround(m[2]), std::lround(Luma(m[0], m[1], m[2])) } } };
    }

    if (request.contains("compare") && request["compare"].is_string()) {
        std::string other = request["compare"].get<std::string>();
        for (char& c : other) {
            if (c == '/' || c == '\\' || c == ':' || c == '.') c = '_';
        }
        Image base = LoadImage((dir / (other + ".png")).string().c_str());
        if (!base.data) {
            reply["compare"] = { { "against", other }, { "error", "no image " + other + ".png" } };
        } else if (base.width != w || base.height != h) {
            reply["compare"] = { { "against", other }, { "error", "sizes differ" } };
        } else {
            ImageFormat(&base, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            ImageCompareOptions options;
            options.tileMinPixels = 1;   // every changed pixel counts: the regions' union is the bbox
            Image diff = {};
            const ImageCompareResult r = CompareImages(base, image, options, &diff);
            float x0 = 1e9f, y0 = 1e9f, x1 = -1.0f, y1 = -1.0f;
            for (const ImageCompareRegion& g : r.regions) {
                x0 = std::min(x0, g.box.x); y0 = std::min(y0, g.box.y);
                x1 = std::max(x1, g.box.x + g.box.width - 1); y1 = std::max(y1, g.box.y + g.box.height - 1);
            }
            ExportImage(diff, (dir / (channel.Id + "_diff.png")).string().c_str());
            UnloadImage(diff);
            reply["compare"] = { { "against", other }, { "changed_pct", r.changedPct }, { "mean_diff", r.meanDiff },
                                 { "bbox", x1 < 0 ? nlohmann::json(nullptr) : nlohmann::json({ x0, y0, x1, y1 }) },
                                 { "diff_image", channel.Id + "_diff.png" } };
        }
        if (base.data) UnloadImage(base);
    }

    if (request.contains("pixels")) {
        const nlohmann::json points = nlohmann::json::parse(channel.Points, nullptr, false);
        const float sx = static_cast<float>(w) / std::max(1, GameInput().ScreenWidth);
        const float sy = static_cast<float>(h) / std::max(1, GameInput().ScreenHeight);
        nlohmann::json out = nlohmann::json::array();
        for (const nlohmann::json& p : points.is_array() ? points : nlohmann::json::array()) {
            nlohmann::json entry = { { "point", p["point"] }, { "screen", p["screen"] }, { "px", nullptr }, { "rgb", nullptr } };
            if (p["screen"].is_array()) {
                const int x = static_cast<int>(std::lround(p["screen"][0].get<float>() * sx));
                const int y = static_cast<int>(std::lround(p["screen"][1].get<float>() * sy));
                if (x >= 1 && y >= 1 && x < w - 1 && y < h - 1) {
                    int sum[3] = { 0, 0, 0 };                // a 3x3 mean: one texel of noise does not decide
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const unsigned char* q = px + (static_cast<size_t>(y + dy) * w + x + dx) * 4;
                            for (int k = 0; k < 3; ++k) sum[k] += q[k];
                        }
                    }
                    entry["px"] = { x, y };
                    entry["rgb"] = { sum[0] / 9, sum[1] / 9, sum[2] / 9 };
                }
            }
            out.push_back(entry);
        }
        reply["pixels"] = out;
    }
    UnloadImage(image);
}


void Reply(AgentChannel& channel)
{
    nlohmann::json reply;
    reply["protocol"] = 2;   // 1: click/type/key only; 2: scroll, drag, key objects, click safety
    reply["id"] = channel.Id;
    reply["fps"] = ProfileFps();
    reply["scene"] = CurrentApp().GetScene().ScriptPath;
    reply["game"] = nlohmann::json::parse(channel.GameReply, nullptr, false);
    if (reply["game"].is_discarded()) reply["game"] = nullptr;
    if (channel.Screenshot) reply["screenshot"] = channel.Id + ".png";
    if (channel.Profile) reply["profile"] = nlohmann::json::parse(ProfileJson());
    if (channel.Health) reply["health"] = Health(channel);
    reply["log"] = NewLog(channel);
    if (!channel.InputReply.empty()) reply["input"] = nlohmann::json::parse(channel.InputReply);
    if (!channel.WaitScene.empty() || !channel.WaitVisible.empty()) {
        reply["wait_until"] = { { "met", channel.WaitMet }, { "frames", channel.WaitFrames } };
    }
    if (channel.UiDump) reply["ui"] = UiDump();
    if (channel.UiProblems) reply["ui_problems"] = UiProblems();
    reply["events"] = nlohmann::json::array();
    for (const std::string& line : LogEventsSince(channel.EventMark)) reply["events"].push_back(nlohmann::json::parse(line, nullptr, false));
    if (channel.Image) Measure(channel, reply);
    if (!channel.TrackNames.empty()) {
        reply["track_ui"] = nlohmann::json::array();
        for (const std::string& frame : channel.TrackFrames) reply["track_ui"].push_back(nlohmann::json::parse(frame));
    }
    WriteAtomically(fs::path(channel.Dir) / (channel.Id + ".json"), reply.dump(1));
    channel.Pending = false;
    if (channel.Quit) CurrentApp().RequestQuit();
}

} // namespace

// The named UI elements as this frame will draw them: every hook has run, so lay each canvas out
// now (the draw pass then restores the same layout from the cache). Rect = on-screen bounds in
// canvas units, after scale; visible = it and every ancestor.
std::string TrackUi(const std::vector<std::string>& names)
{
    nlohmann::json frame = nlohmann::json::object();
    for (const auto& actor : CurrentApp().GetScene().Actors.GetActors()) {
        auto* canvas = dynamic_cast<UI::UICanvas*>(actor.get());
        if (!canvas || actor->PendingDestroy) continue;
        canvas->ResolveLayoutTree();
        for (const std::string& name : names) {
            UI::UIElement* e = frame.contains(name) ? nullptr : canvas->Find(name);
            if (!e) continue;
            const Rectangle r = e->ResolvedRect;
            const UI::UITransform2D& t = e->ResolvedTransform;
            const Vector2 corners[4] = { t.Apply({ r.x, r.y }), t.Apply({ r.x + r.width, r.y }),
                                         t.Apply({ r.x, r.y + r.height }), t.Apply({ r.x + r.width, r.y + r.height }) };
            float x0 = corners[0].x, y0 = corners[0].y, x1 = x0, y1 = y0;
            for (const Vector2& p : corners) {
                x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y);
            }
            bool visible = true;
            for (const UI::UIElement* up = e; up; up = up->Parent) visible = visible && up->Visible;
            frame[name] = { { "rect", { x0, y0, x1 - x0, y1 - y0 } }, { "opacity", e->ResolvedOpacity },
                            { "visible", visible } };
        }
    }
    return frame.dump();
}

std::string ProfileJson()
{
    nlohmann::json sections = nlohmann::json::array();
    for (const ProfileSection& section : ProfileResults()) {
        nlohmann::json entry = { { "name", section.Name }, { "avg_ms", section.AvgMs },
                                 { "max_ms", section.MaxMs }, { "calls", section.Calls } };
        for (const auto& [counter, n] : section.Counts) entry["counts"][counter] = n;
        sections.push_back(std::move(entry));
    }
    return sections.dump();
}

void ApplyAgentInput(AgentChannel& channel)
{
#ifdef IYI_MOBILE
    (void)channel;
#else
    if (!channel.Pending || channel.InputNext >= channel.Input.size()) return;
    const AgentInputStep& step = channel.Input[channel.InputNext++];
    FrameInput& in = GameInput();
    if (step.Key) {
        const auto k = static_cast<size_t>(step.Key);
        const bool press = !step.KeyUp && !step.KeyHold;
        in.KeyDown[k] = !step.KeyUp;
        in.KeyPressed[k] = press;
        in.KeyReleased[k] = step.KeyUp;
        if (press) in.Keys.push_back(step.Key);
        for (int m : step.Mods) in.KeyDown[static_cast<size_t>(m)] = true;
    }
    if (step.Mouse) {
        in.MouseDelta = { step.X - in.Mouse.x, step.Y - in.Mouse.y };
        in.Mouse = { step.X, step.Y };
        in.MouseDown[MOUSE_BUTTON_LEFT] = step.Mouse == 1 || step.Mouse == 3;
        in.MousePressed[MOUSE_BUTTON_LEFT] = step.Mouse == 1;
        in.MouseReleased[MOUSE_BUTTON_LEFT] = step.Mouse == 2;
        if (step.Wheel != 0.0f) in.Wheel = { 0.0f, step.Wheel };
    }
    in.Chars.insert(in.Chars.end(), step.Chars.begin(), step.Chars.end());
#endif
}

std::function<bool()> WatchLauncher()
{
#ifdef IYI_MOBILE
    return nullptr;
#elif defined(_WIN32)
    unsigned long parent = 0;
    const unsigned long self = GetCurrentProcessId();
    void* snapshot = CreateToolhelp32Snapshot(0x2 /* TH32CS_SNAPPROCESS */, 0);
    if (snapshot == reinterpret_cast<void*>(-1)) return nullptr;
    IyiProcessEntry entry{};
    entry.Size = sizeof(entry);
    for (int ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (entry.ProcessId == self) { parent = entry.ParentProcessId; break; }
    }
    CloseHandle(snapshot);
    // The handle is kept open for the whole run, so a reused process id can never pass for the launcher.
    void* handle = parent ? OpenProcess(0x00100000 /* SYNCHRONIZE */, 0, parent) : nullptr;
    if (!handle) return [] { return true; };   // it ended before this process looked: an orphan already
    return [handle] { return WaitForSingleObject(handle, 0) == 0; };   // WAIT_OBJECT_0: it has ended
#else
    const pid_t parent = getppid();   // an orphan is re-parented, so its parent id changes
    return [parent] { return getppid() != parent; };
#endif
}

void UpdateAgentChannel(AgentChannel& channel, float dt)
{
#ifdef IYI_MOBILE
    (void)channel; (void)dt;
#else
    if (channel.Dir.empty()) return;
    if (LauncherLeft(channel)) return;
    channel.FrameMs[channel.Frames % channel.FrameMs.size()] = dt * 1000.0f;
    channel.Frames += 1;
    channel.Seconds += dt;
    if (GameInput().Focused != channel.Focused) {
        channel.Focused = !channel.Focused;
        if (channel.Frames > 1) channel.FocusChanges += 1;
    }
    // Lockstep: the game waits for the agent, so the frames between two requests are exactly what the
    // requests asked for and a run repeats. Up to 60 s, then a frame runs; 10 such minutes in a row, or the
    // launcher ending, quit the run. A closed window (a --visible run) ends the wait too; that frame's input
    // then quits.
    int waited = 0;
    for (; channel.Lockstep && !channel.Pending && waited < 6000 && !(channel.CloseAsked && channel.CloseAsked()); ++waited) {
        if (TakeRequest(channel)) { channel.IdleWaits = 0; return; }
        if (waited % 50 == 0 && LauncherLeft(channel)) return;   // every half second
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (waited == 6000 && ++channel.IdleWaits >= kIdleWaitsToQuit) {
        LogWrite(LogLevel::Warn, "agent", "no request for 10 minutes in a lockstep run: quitting");
        CurrentApp().RequestQuit();
        return;
    }
    if (!channel.Pending) {
        channel.PollTimer += dt;
        if (channel.PollTimer < kPollSeconds) return;
        channel.PollTimer = 0.0f;
        TakeRequest(channel);
        return;
    }
    if (channel.ShotRequested) {   // reply once the render side has drawn and saved that step's frame
        if (GameInput().Presented > channel.ShotStep) Reply(channel);
        return;
    }
    if (!channel.TrackNames.empty()) channel.TrackFrames.push_back(TrackUi(channel.TrackNames));
    if (channel.Waiting && channel.InputNext >= channel.Input.size()) {   // "wait_until" gates the countdown
        channel.WaitFrames += 1;
        const bool scene = channel.WaitScene.empty() || CurrentApp().GetScene().ScriptPath == channel.WaitScene;
        UI::UIElement* e = channel.WaitVisible.empty() ? nullptr : FindUi(channel.WaitVisible);
        const bool visible = channel.WaitVisible.empty() || (e && ShownOnScreen(e));
        channel.WaitMet = scene && visible;
        if (!channel.WaitMet && channel.WaitFrames < channel.WaitTimeout) return;
        channel.Waiting = false;
    }
    channel.FramesLeft -= 1;
    channel.SecondsLeft -= dt;
    if (channel.FramesLeft > 0 || channel.SecondsLeft > 0.0f) return;
    if (channel.Screenshot || channel.Image) {
        if (channel.Image) {
            const nlohmann::json request = nlohmann::json::parse(channel.Request, nullptr, false);
            if (request.is_object() && request.contains("pixels")) channel.Points = LocatePoints(request["pixels"]);
        }
        CurrentApp().RequestScreenshot((fs::path(channel.Dir) / (channel.Id + ".png")).generic_string());
        channel.ShotRequested = true;
        channel.ShotStep = CurrentApp().FrameCount();
    } else {
        Reply(channel);
    }
#endif
}

} // namespace IYI
