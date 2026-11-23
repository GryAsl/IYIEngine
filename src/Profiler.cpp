// Implements the frame profiler (see Profiler.h). Sections are kept in first-seen order with a link
// to their parent. Each frame's totals roll into a one-second window; when the window closes it is
// published in tree order (each section followed by its children, siblings in first-seen order) and
// the next one starts empty. A section that stops running simply drops out of the next result.
// Each thread role (game, render) has its own tree, so the two threads never share section state.

#include "IYI/Profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#ifndef IYI_MOBILE
#include <GLFW/glfw3.h>   // glfwGetProcAddress only: the timer query functions
#endif
#include "IYI/Log.h"

#if defined(_WIN32)
#define IYI_GLAPI __stdcall
#else
#define IYI_GLAPI
#endif

namespace IYI {

namespace {

constexpr size_t kNoParent = static_cast<size_t>(-1);

struct Section
{
    std::string Name;                                    // the full path, "update/.../sim"
    std::string Leaf;                                    // the name it was opened with, "sim"
    size_t Parent = kNoParent;
    std::vector<size_t> Children;                        // looked up by Leaf, so a reopen builds no path
    double FrameMs = 0.0;   // this frame
    int FrameCalls = 0;
    double SumMs = 0.0;     // this window
    double MaxMs = 0.0;
    int Calls = 0;
    double SelfSumMs = 0.0; // CPU time outside immediate child sections, this window
    double SelfMaxMs = 0.0;
    double GpuFrameMs = 0.0;   // GPU time of the frame being read back
    double GpuSumMs = 0.0;     // this window
    double GpuMaxMs = 0.0;
    std::vector<std::pair<std::string, double>> Counts;  // ProfileCount, this window
};

struct Open
{
    size_t Index;
    double Start;
    bool Script;
    unsigned int GpuBegin;   // 0: no GPU mark
};

// GPU timing: timestamp queries from a pool, one list of (section, begin, end) per frame, read back
// once the GPU has passed a frame's last mark.
using GlGenQueries = void (IYI_GLAPI*)(int, unsigned int*);
using GlQueryCounter = void (IYI_GLAPI*)(unsigned int, unsigned int);
using GlGetQueryObjectiv = void (IYI_GLAPI*)(unsigned int, unsigned int, int*);
using GlGetQueryObjectui64v = void (IYI_GLAPI*)(unsigned int, unsigned int, unsigned long long*);
constexpr unsigned int kGlTimestamp = 0x8E28, kGlQueryResult = 0x8866, kGlQueryResultAvailable = 0x8867;
// Resolved once by ProfileInitGpu on the render thread, before any other thread starts; read-only after.
GlGenQueries s_genQueries = nullptr;
GlQueryCounter s_queryCounter = nullptr;
GlGetQueryObjectiv s_getQueryObjectiv = nullptr;
GlGetQueryObjectui64v s_getQueryObjectui64v = nullptr;

struct GpuMark
{
    size_t Index;
    unsigned int Begin, End;
};

// One thread role's sections. A tree is touched only by the thread using it (ProfileSetThread), so
// it needs no lock; only its published Results and Fps are shared, under s_publishMutex. GPU marks go
// only into the render tree: its thread holds the GL context.
struct Tree
{
    std::vector<Section> Sections;
    std::vector<size_t> Roots;                           // sections opened with nothing open
    std::vector<Open> OpenStack;
    std::atomic<size_t> OpenIndex{ kNoParent };          // OpenStack's top, for ProfileOpenSection
    double WindowStart = -1.0;
    int WindowFrames = 0;
    bool Gpu = false;                                    // marks being placed (render tree only)
    std::vector<unsigned int> FreeQueries;
    std::vector<GpuMark> GpuFrame;                       // this frame's marks
    std::deque<std::vector<GpuMark>> GpuPending;         // earlier frames, oldest first
    int GpuFrames = 0;                                   // frames read back in this window
    std::vector<ProfileSection> Results;                 // published, under s_publishMutex
    float Fps = 0.0f;                                    // published, under s_publishMutex
};

constexpr int kGameTree = static_cast<int>(ProfileThread::Game);
constexpr int kRenderTree = static_cast<int>(ProfileThread::Render);
Tree s_trees[2];
thread_local Tree* t_tree = &s_trees[kGameTree];
std::mutex s_publishMutex;
std::atomic<bool> s_gpuWanted{ false };                 // set from any thread, applied by the render tree
std::mutex s_sectionsMutex;   // only a new section (Sections may move) and ProfileOpenSection take it

unsigned int PlaceGpuMark(Tree& tree)
{
    if (tree.FreeQueries.empty()) {
        unsigned int ids[64];
        s_genQueries(64, ids);
        tree.FreeQueries.assign(ids, ids + 64);
    }
    const unsigned int id = tree.FreeQueries.back();
    tree.FreeQueries.pop_back();
    // No batch flush here: a flush drops raylib's extra sampler slots (a shader's second texture),
    // so profiling would change the picture. Draws count where raylib submits them.
    s_queryCounter(id, kGlTimestamp);
    return id;
}

// Reads back every pending frame the GPU has finished, oldest first.
void ReadGpuFrames(Tree& tree)
{
    std::vector<Section>& sections = tree.Sections;
    while (!tree.GpuPending.empty()) {
        std::vector<GpuMark>& marks = tree.GpuPending.front();
        if (!marks.empty()) {
            int done = 0;
            s_getQueryObjectiv(marks.back().End, kGlQueryResultAvailable, &done);
            if (!done) return;
        }
        for (const GpuMark& mark : marks) {
            unsigned long long begin = 0, end = 0;
            s_getQueryObjectui64v(mark.Begin, kGlQueryResult, &begin);
            s_getQueryObjectui64v(mark.End, kGlQueryResult, &end);
            sections[mark.Index].GpuFrameMs += static_cast<double>(end - begin) / 1.0e6;
            tree.FreeQueries.push_back(mark.Begin);
            tree.FreeQueries.push_back(mark.End);
        }
        for (const GpuMark& mark : marks) {
            Section& section = sections[mark.Index];
            if (section.GpuFrameMs < 0.0) continue;      // rolled in already (several marks)
            section.GpuSumMs += section.GpuFrameMs;
            section.GpuMaxMs = std::max(section.GpuMaxMs, section.GpuFrameMs);
            section.GpuFrameMs = -1.0;
        }
        for (const GpuMark& mark : marks) sections[mark.Index].GpuFrameMs = 0.0;
        ++tree.GpuFrames;
        tree.GpuPending.pop_front();
    }
}

constexpr double kWindowSeconds = 1.0;

double NowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Closes the tree's window: its averages become the published result and the next window starts empty.
void Publish(Tree& tree, double now)
{
    std::vector<Section>& sections = tree.Sections;
    const double elapsed = std::max(now - tree.WindowStart, 1e-3);
    const int frames = std::max(tree.WindowFrames, 1);
    // Tree order: sort by the chain of first-seen indices from the root down to the section, so a
    // section that first ran late (a night-only pass) still lands under its parent.
    std::vector<std::pair<std::vector<size_t>, size_t>> order;
    for (size_t i = 0; i < sections.size(); ++i) {
        if (sections[i].Calls == 0) continue;
        std::vector<size_t> chain;
        for (size_t at = i; at != kNoParent; at = sections[at].Parent) chain.push_back(at);
        std::reverse(chain.begin(), chain.end());
        order.emplace_back(std::move(chain), i);
    }
    std::sort(order.begin(), order.end());
    std::vector<ProfileSection> results;
    for (const auto& [chain, i] : order) {
        const Section& section = sections[i];
        results.push_back(ProfileSection{ section.Name,
            static_cast<float>(section.SumMs / frames),
            static_cast<float>(section.MaxMs),
            static_cast<float>(section.Calls) / static_cast<float>(frames) });
        for (const auto& [counter, sum] : section.Counts) {
            results.back().Counts.emplace_back(counter, static_cast<float>(sum / frames));
        }
        // An expensive parent must not hide CPU work merely because it lacks a named child scope.
        if (std::any_of(section.Children.begin(), section.Children.end(), [&](size_t child) {
                return sections[child].Calls > 0;
            }) && section.SelfSumMs > 0.0) {
            results.push_back(ProfileSection{ section.Name + "/self",
                static_cast<float>(section.SelfSumMs / frames),
                static_cast<float>(section.SelfMaxMs),
                static_cast<float>(section.Calls) / static_cast<float>(frames) });
        }
    }
    if (tree.GpuFrames > 0) {                            // the GPU tree, same order
        for (const auto& [chain, i] : order) {
            const Section& section = sections[i];
            if (section.GpuSumMs <= 0.0) continue;
            results.push_back(ProfileSection{ "gpu/" + section.Name,
                static_cast<float>(section.GpuSumMs / tree.GpuFrames),
                static_cast<float>(section.GpuMaxMs),
                static_cast<float>(section.Calls) / static_cast<float>(frames) });
        }
    }
    for (Section& section : sections) {
        section.SumMs = 0.0;
        section.MaxMs = 0.0;
        section.Calls = 0;
        section.SelfSumMs = 0.0;
        section.SelfMaxMs = 0.0;
        section.GpuSumMs = 0.0;
        section.GpuMaxMs = 0.0;
        for (auto& count : section.Counts) count.second = 0.0;
    }
    tree.GpuFrames = 0;
    {
        std::lock_guard<std::mutex> lock(s_publishMutex);
        tree.Results = std::move(results);
        tree.Fps = static_cast<float>(tree.WindowFrames * 1000.0 / elapsed);
    }
    tree.WindowStart = now;
    tree.WindowFrames = 0;
}

// The render tree follows set_gpu_profiling at its frame boundary, where nothing is open. Marks of
// unread frames are dropped with their queries: a moment of no data.
void ApplyGpuWanted(Tree& tree)
{
    const bool wanted = s_gpuWanted.load() && s_queryCounter != nullptr;
    if (&tree != &s_trees[kRenderTree] || tree.Gpu == wanted) return;
    tree.Gpu = wanted;
    tree.GpuFrame.clear();
    tree.GpuPending.clear();
}

} // namespace

void ProfileSetThread(ProfileThread thread)
{
    t_tree = &s_trees[static_cast<int>(thread)];
}

// The section `name` under the innermost open one, made on first use.
static size_t ChildSection(Tree& tree, std::string_view name)
{
    std::vector<Section>& sections = tree.Sections;
    const size_t parent = tree.OpenStack.empty() ? kNoParent : tree.OpenStack.back().Index;
    const std::vector<size_t>& siblings = parent == kNoParent ? tree.Roots : sections[parent].Children;
    size_t index = kNoParent;
    for (size_t child : siblings) {
        if (sections[child].Leaf == name) {
            index = child;
            break;
        }
    }
    if (index == kNoParent) {                            // first time under this parent: name it once
        index = sections.size();
        std::string path = parent == kNoParent ? std::string(name) : sections[parent].Name + "/" + std::string(name);
        std::lock_guard<std::mutex> lock(s_sectionsMutex);
        sections.push_back(Section{ std::move(path), std::string(name), parent });
        (parent == kNoParent ? tree.Roots : sections[parent].Children).push_back(index);
    }
    return index;
}

void ProfileBegin(std::string_view name, bool script)
{
    Tree& tree = *t_tree;
    const size_t index = ChildSection(tree, name);
    tree.OpenStack.push_back(Open{ index, NowMs(), script, tree.Gpu ? PlaceGpuMark(tree) : 0u });
    tree.OpenIndex.store(index, std::memory_order_relaxed);
}

void ProfileAdd(std::string_view name, double ms, int calls)
{
    Tree& tree = *t_tree;
    if (calls <= 0) return;
    Section& section = tree.Sections[ChildSection(tree, name)];
    section.FrameMs += ms;
    section.FrameCalls += calls;
}

double ProfileNowMs()
{
    return NowMs();
}

static std::atomic<bool> s_detail{ false };

void ProfileSetDetail(bool enabled)
{
    s_detail.store(enabled, std::memory_order_relaxed);
}

bool ProfileDetail()
{
    return s_detail.load(std::memory_order_relaxed);
}

void ProfileEnd(bool script)
{
    Tree& tree = *t_tree;
    if (tree.OpenStack.empty() || (script && !tree.OpenStack.back().Script)) return;
    const Open open = tree.OpenStack.back();
    tree.OpenStack.pop_back();
    tree.OpenIndex.store(tree.OpenStack.empty() ? kNoParent : tree.OpenStack.back().Index, std::memory_order_relaxed);
    Section& section = tree.Sections[open.Index];
    section.FrameMs += NowMs() - open.Start;
    section.FrameCalls += 1;
    if (open.GpuBegin != 0) tree.GpuFrame.push_back({ open.Index, open.GpuBegin, PlaceGpuMark(tree) });
}

void ProfileCount(std::string_view counter, double n)
{
    Tree& tree = *t_tree;
    if (tree.OpenStack.empty()) return;
    auto& counts = tree.Sections[tree.OpenStack.back().Index].Counts;
    for (auto& count : counts) {
        if (count.first == counter) {
            count.second += n;
            return;
        }
    }
    counts.emplace_back(std::string(counter), n);
}

std::string ProfileOpenSection(ProfileThread thread, bool wait)
{
    std::unique_lock<std::mutex> lock(s_sectionsMutex, std::defer_lock);
    if (wait) lock.lock();
    else if (!lock.try_lock()) return "?";
    const Tree& tree = s_trees[static_cast<int>(thread)];
    const size_t index = tree.OpenIndex.load(std::memory_order_relaxed);
    return index < tree.Sections.size() ? tree.Sections[index].Name : std::string();
}

size_t ProfileDepth()
{
    return t_tree->OpenStack.size();
}

void ProfileEndTo(size_t depth)
{
    while (t_tree->OpenStack.size() > depth) ProfileEnd();
}

void ProfileEndFrame()
{
    Tree& tree = *t_tree;
    ProfileEndTo(0);
    if (tree.Gpu) {
        tree.GpuPending.push_back(std::move(tree.GpuFrame));
        tree.GpuFrame.clear();
        ReadGpuFrames(tree);
    }
    ApplyGpuWanted(tree);
    const double now = NowMs();
    if (tree.WindowStart < 0.0) tree.WindowStart = now;
    ++tree.WindowFrames;
    // Compute the exclusive cost before resetting child totals. Subtract only immediate children
    // and calculate the maximum per frame; differences between published maxima are meaningless.
    for (Section& section : tree.Sections) {
        if (section.FrameCalls == 0) continue;
        double self = section.FrameMs;
        for (size_t child : section.Children) self -= tree.Sections[child].FrameMs;
        self = std::max(0.0, self);
        section.SelfSumMs += self;
        section.SelfMaxMs = std::max(section.SelfMaxMs, self);
    }
    for (Section& section : tree.Sections) {
        section.SumMs += section.FrameMs;
        section.MaxMs = std::max(section.MaxMs, section.FrameMs);
        section.Calls += section.FrameCalls;
        section.FrameMs = 0.0;
        section.FrameCalls = 0;
    }

    if (now - tree.WindowStart >= kWindowSeconds * 1000.0) Publish(tree, now);
}

// Called with no thread running a tree (at exit, in tests), so both trees may be published here.
void ProfilePublishPartial()
{
    for (Tree& tree : s_trees) {
        bool empty;
        {
            std::lock_guard<std::mutex> lock(s_publishMutex);
            empty = tree.Results.empty();
        }
        if (empty && tree.WindowFrames > 0) Publish(tree, NowMs());
    }
}

void ProfileInitGpu()
{
#ifndef IYI_MOBILE
    s_genQueries = reinterpret_cast<GlGenQueries>(glfwGetProcAddress("glGenQueries"));
    s_queryCounter = reinterpret_cast<GlQueryCounter>(glfwGetProcAddress("glQueryCounter"));
    s_getQueryObjectiv = reinterpret_cast<GlGetQueryObjectiv>(glfwGetProcAddress("glGetQueryObjectiv"));
    s_getQueryObjectui64v = reinterpret_cast<GlGetQueryObjectui64v>(glfwGetProcAddress("glGetQueryObjectui64v"));
    if (!s_genQueries || !s_getQueryObjectiv || !s_getQueryObjectui64v) s_queryCounter = nullptr;
#endif
}

bool ProfileSetGpu(bool enabled)
{
    if (enabled && !s_queryCounter) {
        LogWrite(LogLevel::Warn, "profile", "GPU timing unavailable: no timestamp queries");
        enabled = false;
    }
    s_gpuWanted.store(enabled);
    return enabled;
}

std::vector<ProfileSection> ProfileResults()
{
    std::lock_guard<std::mutex> lock(s_publishMutex);
    std::vector<ProfileSection> results = s_trees[kGameTree].Results;
    const std::vector<ProfileSection>& render = s_trees[kRenderTree].Results;
    results.insert(results.end(), render.begin(), render.end());
    return results;
}

float ProfileFps()
{
    std::lock_guard<std::mutex> lock(s_publishMutex);
    return s_trees[kRenderTree].Fps > 0.0f ? s_trees[kRenderTree].Fps : s_trees[kGameTree].Fps;
}

} // namespace IYI
