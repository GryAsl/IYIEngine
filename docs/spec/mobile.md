# §24 Mobile platforms (Android, iOS)

Approved by Gry 2026-11-15 (decisions in `docs/mobile/STATUS.md`; plan in `docs/mobile/plan/`). One engine
codebase: desktop and web behaviour do not change; mobile is a build target, not a fork.

## 24.1 Build switches

- CMake sets `IYI_PLATFORM_ANDROID` or `IYI_PLATFORM_IOS`, and `IYI_MOBILE`, from `CMAKE_SYSTEM_NAME`
  (`Android`, `iOS`). In any `#if` chain the mobile branch comes first: Android also defines `__linux__`,
  iOS defines `__APPLE__`.
- Mobile forces `IYI_STEAM=OFF` (`Steam.cpp` builds as its no-Steam variant) and builds no desktop exe,
  tests, tools, trailer or packer. Core and Sim are PIC static libraries.
- Android output is `libiyigame.so` on raylib's own Android platform (`PLATFORM=Android`, GLES 3.0,
  NativeActivity calling `main()`); one entry point. iOS links the pinned ghera/raylib-iOS
  `raylib.xcframework` with ANGLE `libEGL`/`libGLESv2` xcframeworks given by cache paths.
- Target CPython comes from `IYI_PYTHON_TARGET_ROOT` (`include/python3.14`, `lib/libpython3.14.so`); a
  mobile configure never runs or links the host interpreter.
- No GLFW on mobile: monitor queries return the screen, GPU timer queries are off (CPU profiling stays),
  `StackLighting` calls GLES3 functions directly.

## 24.2 Paths and resources

- `src/MobilePaths.h` is the one place that knows the app container: `MobileDataRoot()` (Android
  `internalDataPath`, iOS `Application Support`) feeds user data, logs and saves; `MobileAssets()` is the
  Android `AAssetManager`.
- Android reads `game.rres` in place from the APK through an AAsset byte source behind the existing
  packaged-archive parser (stored uncompressed); iOS reads it from the app bundle. Nothing is written
  beside the bundle.
- Android ships the CPython stdlib as APK assets `python/**` with `python/manifest.txt` (content hash, then
  one path per line). At start the engine copies them to `<data>/python` when the hash changes (staging
  folder, stamp last, then rename); `PyConfig` is isolated with that home, no bytecode writes, no user site.
  Scripts still import from `game.rres` through the shipping packaged finder.

## 24.3 Runtime

- Android uses the desktop threaded frame loop; iOS uses the single-thread callback path. Mobile has no CLI
  and loads the project from the packaged resource store. `Application::Frame()` lets iOS drive one frame.
- While the app is paused or unfocused the simulation does not step and no time accumulates.
- `LogWrite` also writes to logcat (tag `IYIEngine`). Agent channel, file watcher, hot reload and crash
  watchdog are off on mobile.

## 24.4 Packaging

`py tools/release.py --game <game> --platform android` packs `game.rres`, builds the host `iyipack`, runs
the Gradle project `platform/android/` (minSdk 24, targetSdk 36, arm64-v8a) and checks 16 KB ELF
alignment, `zipalign -P 16`, a stored `game.rres` and no Steam/GLFW symbols. Per-game store values come
from the game's `release.json` `android` block. Pins: `tools/mobile/dependency-lock.json`.

Android release packaging uses `exe_icon` from `release.json` to generate launcher icons at all five
Android densities and an API 26 adaptive icon with a black background. Without an icon the Android
shell uses the system default.

Android uses an `IYIActivity` NativeActivity subclass: immersive sticky system bars, a focused input
connection (committed IME text is queued for the engine; Backspace/Enter go to the native key queue) and
the Google Play bridge below.

## 24.5 Google Play (`engine.play`)

`available()`, `unlock(name)`, `cloud_write(name, data, modified_ms)`, `cloud_fetch(name)` then poll
`cloud_result(name) -> (status, data, modified_ms)` (status `pending|ready|missing|error|off`),
`cloud_names()` (snapshot names listed after sign-in; `None` until then),
`request_review()`, `license_state()` (`off|pending|ok|denied`). Java (`PlayServices.java`,
`License.java`) runs every async call and stores results; Python only polls, no callbacks. Desktop, iOS
and any feature whose `release.json` field is empty return the off values.

- Play Games Services v2: automatic sign-in only (no prompt). `unlock` takes the game's own achievement
  key; `play_achievement_ids` maps it to the Play ID. Saved Games snapshots keep the newest server copy.
- Licensing: LVL client-side check (Play Integrity needs our own server). Only a signed, nonce-matching
  `NOT_LICENSED` answer is `denied`; errors, timeouts and a missing Play Store are `ok`.

`release.json` `android` fields (empty = feature off):

| Field | Play Console page |
|---|---|
| `play_games_app_id` | Play Games Services > Setup and management > Configuration: the numeric project ID |
| `play_achievement_ids` | Play Games Services > Achievements: `{ "<game key>": "<achievement ID>" }` |
| `license_public_key` | Monetize with Play > Monetization setup > Licensing: the Base64 public key |
