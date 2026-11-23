# Windows IME input

The engine observes the existing raylib/GLFW window with `SetWindowSubclass`.
Every native message continues through `DefSubclassProc`. Windows renders its own
composition and candidate windows; the engine positions them at the focused visible
text field's transformed caret, including UI scale and the application's radial warp.
Committed Unicode continues through raylib's `GetCharPressed` only. No second commit
queue, dependency fork, IME implementation, or custom candidate renderer is added.

The original Windows input context is borrowed, attached while a text field is drawn
focused, and restored before shutdown. Focus changes, element removal and window
focus loss cancel composition. Keyboard UI actions and Python key queries are guarded
during composition and the frame that commits/cancels it. `engine.is_ime_composing()`
reports live Windows composition, rather than that broader frame guard. This query
returns `False` on other platforms. Android has a separate soft-keyboard bridge;
its [device verification and limits](mobile/ISSUES.md) are not covered by this guide.

## Research and choice

The [raylib IME discussion](https://github.com/raysan5/raylib/issues/1945),
[raylib draft integration](https://github.com/raysan5/raylib/pull/2809),
[GLFW discussion](https://github.com/glfw/glfw/issues/2097) and
[clear-code sample](https://github.com/clear-code/RaylibIMEInputSampleApp) demonstrate
the broader route of extending GLFW with preedit APIs. The sample was consulted,
not copied. This implementation uses the smaller native Windows default-processing
route because the game needs reliable single-line entry with system candidate UI.

Microsoft documents default processing for
[WM_IME_COMPOSITION](https://learn.microsoft.com/en-us/windows/win32/intl/wm-ime-composition)
and translation of
[WM_IME_CHAR](https://learn.microsoft.com/en-us/windows/win32/intl/wm-ime-char)
to normal character messages. Positioning uses
[ImmSetCompositionWindow](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immsetcompositionwindow)
and [ImmSetCandidateWindow](https://learn.microsoft.com/en-us/windows/win32/api/imm/nf-imm-immsetcandidatewindow).

## Verification on 2026-10-14

- Release build and complete native engine suite passed. Hidden HWND tests cover
  message forwarding, same-poll start/end latching, owner rejection, cancellation,
  focus loss, repeated initialization/shutdown and window destruction.
- An actual Human Resources raylib/GLFW window received synthetic Win32 messages.
  Native `WM_IME_CHAR` delivered `中日한` exactly once to the onboarding field.
  Composition guarded Backspace, Enter and Escape; ordinary Backspace then removed
  one complete Unicode character. Scene removal cleared composition and the next
  field had no stale text. The report is
  [HR IME runtime receipt](../../HumanResources/docs/localization-reviews/ime-runtime.json).
- API typing synchronization passed with no undocumented symbols. An independent
  review found no lifecycle or poll-ordering defect in the bridge (it follows Win32's
  default IME path: `DefSubclassProc` shows system composition/candidate UI, commits
  arrive as `WM_CHAR`). Real Korean, Japanese and Chinese IMEs were not tested.
- Human Resources syntax, fast and full verification passed. The full suite was
  proportionate here because the Windows bridge and shared keyboard API changed.

These checks use synthetic messages, not installed CJK IME providers. No system
language or input-method settings were changed. Native candidate appearance and
provider-specific behavior require the following manual check by the owner.

## Manual provider check

Use your own Simplified Chinese, Japanese and Korean Windows input methods in turn.

1. Open New Game and the company-name field. Compose a name and choose a candidate.
   Preedit/candidates should be readable near the field and the committed name should
   appear once. Try `中文公司`, `株式会社`, and `한국회사` with the corresponding methods.
2. Confirm a candidate with Enter. That Enter must not start the run. Use a separate
   Enter after composition finishes to submit the name.
3. While composing, Backspace should edit preedit and Escape should cancel it without
   deleting the committed name or leaving the screen. After composition, Backspace
   should delete one committed Unicode character at a time.
4. Click another field/button, return to the menu, and switch away from the window
   during composition. No unfinished text should arrive in another field.
5. Repeat at a smaller resolution and in fullscreen with the desired CRT effect.
   Candidates should follow the field and remain visible. Windows/provider placement
   policy may adjust the requested position near screen edges.
6. Repeat in company rename and manual save-name fields. Numeric-only seed input
   should reject committed letters, just as it rejects pasted letters.

Text fields still append at the end. Selection, movable editing caret, grapheme-aware
deletion, custom inline preedit, shaping, bidi and non-Windows IME positioning are
outside this change.
