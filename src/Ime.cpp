// Observe GLFW's HWND without replacing its input path. DefSubclassProc owns all
// native message handling; only raylib's GetCharPressed delivers committed text.
// Threads: everything here runs on the main (window) thread. The game side's ImeRequest,
// ImeCancelOwner and ImeAcceptText are in FrameInput.cpp (no windows.h next to raylib.h there).
#include "IYI/Ime.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <imm.h>
#include <algorithm>
#include <cmath>
#include "IYI/Log.h"

namespace IYI {
namespace {
constexpr UINT_PTR kSubclass = 0x495949;
HWND window = nullptr;
HIMC originalContext = nullptr; // borrowed from Windows, never destroyed by us
const void* owner = nullptr;
const void* requested = nullptr;
const void* pendingCommitOwner = nullptr;
const void* frameCommitOwner = nullptr;
bool composing = false, pendingActivity = false, frameActivity = false;
bool discardFrame = false, pendingDiscard = false;
bool inputFrame = false;
float anchorX = 0, anchorY = 0, anchorHeight = 0, viewportW = 1, viewportH = 1;

void Cancel()
{
    // Clear identity before calling IMM: cancellation can synchronously reenter our hook.
    const void* previousOwner = owner;
    owner = nullptr;
    const bool wasComposing = composing;
    if (wasComposing) pendingCommitOwner = previousOwner;
    composing = false;
    if (wasComposing) pendingActivity = true;
    if (!window || !wasComposing) return;
    HIMC context = ImmGetContext(window);
    if (context) {
        ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmReleaseContext(window, context);
    }
}

LRESULT CALLBACK Observe(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (msg == WM_CHAR && owner) pendingCommitOwner = owner;
    if (owner && (msg == WM_IME_STARTCOMPOSITION || msg == WM_IME_COMPOSITION || msg == WM_IME_ENDCOMPOSITION)) {
        pendingActivity = true;
        pendingCommitOwner = owner; // preserve through END -> WM_CHAR in the same poll
        if (msg == WM_IME_STARTCOMPOSITION || (msg == WM_IME_COMPOSITION && (lp & GCS_COMPSTR))) composing = true;
        if (msg == WM_IME_ENDCOMPOSITION || (msg == WM_IME_COMPOSITION && lp == 0)) composing = false;
    }
    if (msg == WM_KILLFOCUS) {
        discardFrame = pendingDiscard = true;
        Cancel();
        if (window) ImmAssociateContext(window, nullptr);
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, Observe, kSubclass);
        window = nullptr;
        owner = requested = nullptr;
        composing = false;
    }
    // In particular, do not consume/synthesize WM_CHAR, WM_IME_CHAR or GCS_RESULTSTR.
    return DefSubclassProc(hwnd, msg, wp, lp);
}
}

void ImeInitialize(void* handle)
{
    ImeShutdown();
    window = static_cast<HWND>(handle);
    if (!window || !SetWindowSubclass(window, Observe, kSubclass, 0)) {
        window = nullptr;
        LogWrite(LogLevel::Warn, "ui", "Windows IME observer could not be installed");
        return;
    }
    originalContext = ImmGetContext(window);
    if (originalContext) ImmReleaseContext(window, originalContext);
    ImmAssociateContext(window, nullptr); // no text field: IME must not eat game shortcuts
}

void ImeShutdown()
{
    Cancel();
    if (window) {
        ImmAssociateContext(window, originalContext);
        RemoveWindowSubclass(window, Observe, kSubclass);
    }
    window = nullptr;
    originalContext = nullptr;
    requested = pendingCommitOwner = frameCommitOwner = nullptr;
    pendingActivity = frameActivity = discardFrame = pendingDiscard = false;
    inputFrame = false;
}

void ImeBeginFrame()
{
    inputFrame = true;
    // EndDrawing polls messages. Preserve that poll for ALL UI/game hooks this frame,
    // even when the IME both committed and ended composition inside one event pump.
    frameActivity = pendingActivity;
    frameCommitOwner = pendingCommitOwner;
    pendingActivity = false;
    pendingCommitOwner = nullptr;
    discardFrame = pendingDiscard;
    pendingDiscard = false;
    requested = nullptr;
}

void ImeCancelOwnerNow(const void* token)
{
    if (frameCommitOwner == token || pendingCommitOwner == token) {
        discardFrame = true;
        if (!inputFrame) pendingDiscard = true;
    }
    if (requested == token) requested = nullptr;
    if (owner == token) {
        if (composing || frameActivity || pendingActivity) {
            discardFrame = true;
            if (!inputFrame) pendingDiscard = true;
        }
        Cancel();
        if (window) ImmAssociateContext(window, nullptr);
    }
}

void ImeRequestNow(const void* token, float x, float y, float height, float width, float screenHeight)
{
    requested = token; // last/topmost drawn focused field wins
    anchorX = x; anchorY = y; anchorHeight = height;
    viewportW = std::max(1.0f, width); viewportH = std::max(1.0f, screenHeight);
}

void ImeEndFrame()
{
    inputFrame = false;
    if (!window) return;
    if (owner != requested) {
        if (owner) ImeCancelOwnerNow(owner);
        owner = requested;
        ImmAssociateContext(window, owner ? originalContext : nullptr);
    }
    if (!owner) return;
    HIMC context = ImmGetContext(window);
    if (!context) return;
    RECT client{}; GetClientRect(window, &client);
    const float sx = static_cast<float>(client.right) / viewportW;
    const float sy = static_cast<float>(client.bottom) / viewportH;
    POINT point{std::clamp(static_cast<LONG>(std::lround(anchorX * sx)), 0L, std::max(0L, client.right - 1)),
                std::clamp(static_cast<LONG>(std::lround(anchorY * sy)), 0L, std::max(0L, client.bottom - 1))};
    const LONG height = std::max(1L, static_cast<LONG>(std::lround(anchorHeight * sy)));
    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = point;
    ImmSetCompositionWindow(context, &composition);
    // Let Windows render its own candidate UI, positioned outside the caret line.
    for (DWORD index = 0; index < 4; ++index) {
        CANDIDATEFORM candidate{};
        candidate.dwIndex = index;
        candidate.dwStyle = CFS_EXCLUDE;
        candidate.ptCurrentPos = {point.x, point.y + height};
        candidate.rcArea = {point.x, point.y, point.x + 1, point.y + height};
        ImmSetCandidateWindow(context, &candidate);
    }
    LOGFONTW font{};
    font.lfHeight = -height;
    font.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(font.lfFaceName, L"Segoe UI");
    ImmSetCompositionFontW(context, &font);
    ImmReleaseContext(window, context);
}

bool ImeKeyboardCaptured() { return composing || frameActivity || pendingActivity; }
ImeFrame ImeTakeFrame() { return { composing, ImeKeyboardCaptured(), discardFrame, owner, frameCommitOwner }; }
}
#elif defined(IYI_PLATFORM_ANDROID)
#include "MobilePaths.h"
#include <algorithm>
#include <atomic>
namespace IYI {
namespace {
const void* owner = nullptr; const void* requested = nullptr;
float requestedBottom = -1.0f;
std::atomic<float> fieldBottom{-1.0f};   // focused field's bottom edge / UI height; < 0 = none
}
void ImeInitialize(void*) {}
void ImeShutdown() { if (owner) AndroidShowKeyboard(false); owner = requested = nullptr; fieldBottom = -1.0f; }
void ImeBeginFrame() { requested = nullptr; }
void ImeEndFrame() {
    if (owner != requested) {
        if (!owner || !requested) AndroidShowKeyboard(requested != nullptr);
        owner = requested;
    }
    fieldBottom = owner ? requestedBottom : -1.0f;
}
float ImeFieldBottom() { return fieldBottom; }
void ImeRequestNow(const void* token, float, float y, float height, float, float screenHeight) {
    requested = token;
    requestedBottom = (y + height) / std::max(1.0f, screenHeight);
}
void ImeCancelOwnerNow(const void* token) {
    if (requested == token) requested = nullptr;
    if (owner == token) { owner = nullptr; AndroidShowKeyboard(false); }
}
bool ImeKeyboardCaptured() { return false; }
ImeFrame ImeTakeFrame() { return {}; }
}
#else
namespace IYI {
void ImeInitialize(void*) {}
void ImeShutdown() {}
void ImeBeginFrame() {}
void ImeEndFrame() {}
void ImeRequestNow(const void*, float, float, float, float, float) {}
void ImeCancelOwnerNow(const void*) {}
bool ImeKeyboardCaptured() { return false; }
ImeFrame ImeTakeFrame() { return {}; }
}
#endif
