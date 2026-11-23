// Focused native-message tests for the Windows IME observer.
// The test HWND stays hidden and never changes the foreground window or input language.

#include "IYI/Ime.h"

#ifdef _WIN32

// raylib.h (through the frame headers) comes first; windows.h gets its three clashing names renamed.
#include "IYI/FrameInput.h"
#include "IYI/FramePacket.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define Rectangle Win32Rectangle
#define CloseWindow Win32CloseWindow
#define ShowCursor Win32ShowCursor
#include <windows.h>
#include <imm.h>
#undef Rectangle
#undef CloseWindow
#undef ShowCursor

#include <cstdio>
#include <cstdint>

namespace {
constexpr wchar_t kWindowClass[] = L"IYIEngineImeNativeTestWindow";
constexpr UINT kProbeMessage = WM_APP + 0x35;
constexpr LRESULT kProbeResult = 0x517;

int g_wmCharCount = 0;
int g_wmImeCharCount = 0;
int g_imeStartCount = 0;
int g_probeCount = 0;

LRESULT CALLBACK TestWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CHAR:
        ++g_wmCharCount;
        return 0;
    case WM_IME_CHAR:
        ++g_wmImeCharCount;
        return 0;
    case WM_IME_STARTCOMPOSITION:
        ++g_imeStartCount;
        return 0;
    case kProbeMessage:
        ++g_probeCount;
        return kProbeResult;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

HWND CreateHiddenTestWindow()
{
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TestWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }

    // WS_POPUP is never shown, so no focus or foreground changes occur.
    return CreateWindowExW(0, kWindowClass, L"", WS_POPUP,
                           0, 0, 320, 200, nullptr, nullptr, instance, nullptr);
}

// The --single-thread frame order of Application::Run: per frame the IME frame is taken, the game step
// records platform requests, and the render step replays them at once, then ends the IME frame.
void GameStep(IYI::FramePacket& packet, IYI::FrameInput& input)
{
    IYI::ImeBeginFrame();
    input.Ime = IYI::ImeTakeFrame();
    IYI::SetGameInput(&input);
    IYI::SetRecordingPacket(&packet);
}

void RenderStep(IYI::FramePacket& packet)
{
    IYI::ReplayPass(packet, IYI::RecordPass::Setup);
    IYI::ImeEndFrame();
    packet.Clear();
}

// The threaded order of Application::Run, driven from one thread. Main iteration k begins the IME frame
// and takes the input for game step k (Begin); step k records into packet k%2; the main thread then
// replays packet k-1 and ends the IME frame (End). So what step k requests reaches the window in
// iteration k+1, and the input of step k+1 does not show it yet.
struct Pipelined
{
    IYI::FramePacket packets[2];
    IYI::FrameInput inputs[2];
    int k = 0;

    IYI::FrameInput& Begin()
    {
        IYI::FrameInput& input = inputs[k % 2];
        IYI::ImeBeginFrame();
        input.Ime = IYI::ImeTakeFrame();
        IYI::SetGameInput(&input);
        IYI::SetRecordingPacket(&packets[k % 2]);
        return input;
    }

    void End()
    {
        if (k > 0) {
            IYI::FramePacket& packet = packets[(k - 1) % 2];
            IYI::ReplayPass(packet, IYI::RecordPass::Setup);
            IYI::ImeEndFrame();
            packet.Clear();
        }
        ++k;
    }
};

bool LiveComposing() { return IYI::ImeTakeFrame().Composing; }
}

int TestImeWindows()
{
    int failures = 0;
#define IME_CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

    const void* ownerA = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x101));
    const void* ownerB = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(0x202));

    HWND hwnd = CreateHiddenTestWindow();
    IME_CHECK(hwnd != nullptr);
    if (!hwnd) {
        return failures;
    }

    IYI::FramePacket packet;
    IYI::FrameInput input;

    IYI::ImeInitialize(hwnd);
    GameStep(packet, input);
    IYI::ImeRequest(ownerA, 12.0f, 18.0f, 20.0f, 320.0f, 200.0f);
    RenderStep(packet);

    // The observer must forward committed and unrelated messages to the original WndProc.
    const int charsBefore = g_wmCharCount;
    const int imeCharsBefore = g_wmImeCharCount;
    const int probesBefore = g_probeCount;
    SendMessageW(hwnd, WM_CHAR, L'X', 1);
    SendMessageW(hwnd, WM_IME_CHAR, L'Y', 1);
    const LRESULT probeResult = SendMessageW(hwnd, kProbeMessage, 7, 9);
    IME_CHECK(g_wmCharCount == charsBefore + 1);
    IME_CHECK(g_wmImeCharCount == imeCharsBefore + 1);
    IME_CHECK(g_probeCount == probesBefore + 1);
    IME_CHECK(probeResult == kProbeResult);

    // START, interim composition, and END in one native poll leave a poll latch for
    // the following BeginFrame even though the live composition has already ended.
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    IME_CHECK(LiveComposing());
    SendMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_COMPSTR);
    IME_CHECK(LiveComposing());
    SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
    IME_CHECK(!LiveComposing());
    IME_CHECK(IYI::ImeKeyboardCaptured());

    GameStep(packet, input);
    IME_CHECK(!input.Ime.Composing);
    IME_CHECK(input.Ime.KeyboardCaptured);
    IME_CHECK(IYI::ImeAcceptText(ownerA));
    IME_CHECK(!IYI::ImeAcceptText(ownerB));
    IYI::ImeRequest(ownerA, 12.0f, 18.0f, 20.0f, 320.0f, 200.0f);
    RenderStep(packet);

    // An empty following poll clears the one-frame keyboard guard.
    GameStep(packet, input);
    IME_CHECK(!input.Ime.KeyboardCaptured);
    IYI::ImeRequest(ownerA, 12.0f, 18.0f, 20.0f, 320.0f, 200.0f);
    RenderStep(packet);

    // A result/end poll belongs to A. If focus changes before text is accepted,
    // the result cannot be accepted by B or leak through the canceled owner.
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    SendMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
    SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
    GameStep(packet, input);
    IME_CHECK(IYI::ImeAcceptText(ownerA));
    IME_CHECK(!IYI::ImeAcceptText(ownerB));
    IYI::ImeCancelOwner(ownerA);
    IYI::ImeRequest(ownerB, 24.0f, 30.0f, 22.0f, 320.0f, 200.0f);
    RenderStep(packet);
    IME_CHECK(!IYI::ImeAcceptText(ownerA));
    IME_CHECK(!IYI::ImeAcceptText(ownerB));

    // The committed character message still traverses the stock HWND path; the bridge
    // does not consume or manufacture it. Acceptance remains blocked for this frame.
    const int staleCharsBefore = g_wmCharCount;
    SendMessageW(hwnd, WM_CHAR, L'Z', 1);
    IME_CHECK(g_wmCharCount == staleCharsBefore + 1);
    IME_CHECK(!IYI::ImeAcceptText(ownerB));

    GameStep(packet, input);
    IME_CHECK(IYI::ImeAcceptText(ownerB));
    IYI::ImeRequest(ownerB, 24.0f, 30.0f, 22.0f, 320.0f, 200.0f);
    RenderStep(packet);

    // Explicit UI-owner cancellation and OS focus loss both clear composition state.
    // The game side stops accepting text at once; the window follows after the render step.
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    IME_CHECK(LiveComposing());
    GameStep(packet, input);
    IME_CHECK(input.Ime.Composing);
    IYI::ImeCancelOwner(ownerB);
    IME_CHECK(!IYI::ImeAcceptText(ownerB));
    RenderStep(packet);
    IME_CHECK(!LiveComposing());
    GameStep(packet, input);
    IYI::ImeRequest(ownerB, 24.0f, 30.0f, 22.0f, 320.0f, 200.0f);
    RenderStep(packet);
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    IME_CHECK(LiveComposing());
    SendMessageW(hwnd, WM_KILLFOCUS, 0, 0);
    IME_CHECK(!LiveComposing());
    IME_CHECK(IYI::ImeTakeFrame().Discard);
    GameStep(packet, input);
    IME_CHECK(!IYI::ImeAcceptText(ownerB));
    RenderStep(packet);

    // Pipelined order: a request or cancel made in game step k reaches the window at the replay of
    // packet k, in main iteration k+1. The game side (input, ImeAcceptText) reacts at once, the
    // window one iteration later.
    IYI::ImeShutdown();
    IYI::ImeInitialize(hwnd);
    {
        Pipelined p;
        for (int step = 0; step < 2; ++step) {      // A requests itself every frame
            IYI::FrameInput& in = p.Begin();
            IME_CHECK(in.Ime.Owner == nullptr);      // step 1 still sees no owner: packet 0 replays after it
            IYI::ImeRequest(ownerA, 12.0f, 18.0f, 20.0f, 320.0f, 200.0f);
            p.End();
            IME_CHECK((IYI::ImeTakeFrame().Owner == ownerA) == (step == 1));
        }
        IME_CHECK(p.Begin().Ime.Owner == ownerA);
        IYI::ImeRequest(ownerA, 12.0f, 18.0f, 20.0f, 320.0f, 200.0f);
        p.End();

        // A commit for A arrives; in step 3 focus moves to B. The game stops accepting for A at once,
        // but the window still serves A after this iteration (the cancel is in packet 3, not replayed yet).
        SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
        SendMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
        SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
        IME_CHECK(p.Begin().Ime.CommitOwner == ownerA);
        IME_CHECK(IYI::ImeAcceptText(ownerA));
        IME_CHECK(!IYI::ImeAcceptText(ownerB));
        IYI::ImeCancelOwner(ownerA);
        IME_CHECK(!IYI::ImeAcceptText(ownerA));
        IYI::ImeRequest(ownerB, 24.0f, 30.0f, 22.0f, 320.0f, 200.0f);
        p.End();
        IME_CHECK(IYI::ImeTakeFrame().Owner == ownerA);

        // In that lag a late commit still reaches A (the window owner); B must not take it.
        SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
        SendMessageW(hwnd, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
        SendMessageW(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
        IME_CHECK(p.Begin().Ime.CommitOwner == ownerA);
        IME_CHECK(!IYI::ImeAcceptText(ownerB));
        IYI::ImeRequest(ownerB, 24.0f, 30.0f, 22.0f, 320.0f, 200.0f);
        p.End();
        IME_CHECK(IYI::ImeTakeFrame().Owner == ownerB);   // packet 3's cancel and request took effect

        IME_CHECK(p.Begin().Ime.Owner == ownerB);
        IME_CHECK(IYI::ImeAcceptText(ownerB));
        IYI::ImeRequest(ownerB, 24.0f, 30.0f, 22.0f, 320.0f, 200.0f);
        p.End();

        // OS focus loss while B composes: the next input carries Discard, and the one after is clean.
        SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
        SendMessageW(hwnd, WM_KILLFOCUS, 0, 0);
        IME_CHECK(p.Begin().Ime.Discard);
        IME_CHECK(!IYI::ImeAcceptText(ownerB));
        IYI::ImeRequest(ownerB, 24.0f, 30.0f, 22.0f, 320.0f, 200.0f);
        p.End();
        IME_CHECK(!p.Begin().Ime.Discard);
        IME_CHECK(IYI::ImeAcceptText(ownerB));
        p.End();
    }

    // Reinitialize repeatedly on the same hidden HWND, then destroy with the subclass
    // installed. WM_NCDESTROY must detach the callback so shutdown remains safe.
    IYI::ImeShutdown();
    IME_CHECK(!LiveComposing());
    for (int i = 0; i < 3; ++i) {
        IYI::ImeInitialize(hwnd);
        GameStep(packet, input);
        IYI::ImeRequest(ownerA, 4.0f, 5.0f, 16.0f, 320.0f, 200.0f);
        RenderStep(packet);
        SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
        IME_CHECK(LiveComposing());
        IYI::ImeShutdown();
        IME_CHECK(!LiveComposing());
    }

    IYI::ImeInitialize(hwnd);
    GameStep(packet, input);
    IYI::ImeRequest(ownerB, 8.0f, 9.0f, 16.0f, 320.0f, 200.0f);
    RenderStep(packet);
    SendMessageW(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    IME_CHECK(LiveComposing());
    DestroyWindow(hwnd);
    IME_CHECK(!LiveComposing());
    IYI::ImeShutdown();

    IYI::SetGameInput(nullptr);
    IYI::SetRecordingPacket(nullptr);

#undef IME_CHECK
    return failures;
}

#else

int TestImeWindows()
{
    return 0;
}

#endif
