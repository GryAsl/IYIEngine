#pragma once

namespace IYI {
// Windows uses the system composition/candidate UI. Other platforms are no-ops.
// Owner is an opaque identity, never dereferenced. Committed text stays in raylib.

// The IME's state for one frame, copied into the game step's FrameInput.
struct ImeFrame
{
    bool Composing = false, KeyboardCaptured = false, Discard = false;
    const void* Owner = nullptr;         // the field the IME serves
    const void* CommitOwner = nullptr;   // the field committed text belongs to, or none
};

// Main (window) thread (Ime.cpp).
void ImeInitialize(void* window);
void ImeShutdown();
// Begin after the previous EndDrawing poll; End before the next EndDrawing poll.
// The capture latch must cover every UI/game hook, including a same-poll commit/end.
void ImeBeginFrame();
void ImeEndFrame();
bool ImeKeyboardCaptured();
ImeFrame ImeTakeFrame();
void ImeRequestNow(const void* owner, float x, float y, float height, float viewportWidth, float viewportHeight);
void ImeCancelOwnerNow(const void* owner);

// Game side (FrameInput.cpp): answered from the frame input; window work goes as platform requests.
// Request each draw from the visible focused caret in viewport pixels, not UI units.
void ImeRequest(const void* owner, float x, float y, float height, float viewportWidth, float viewportHeight);
// Cancel before focus changes or owner removal, while its identity is still available.
void ImeCancelOwner(const void* owner);
bool ImeAcceptText(const void* owner);
}
