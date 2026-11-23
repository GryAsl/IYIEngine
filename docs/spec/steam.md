# IYI Engine contract: Steamworks (`engine.steam`)

Part of the engine contract; index and rules in [../SPEC.md](../SPEC.md). Section numbers are the contract's own.

## 21. Steamworks subsystem (`engine.steam`)

The `engine.steam` subsystem provides an optional, minimal bridge to Valve's Steamworks SDK (targeting SDK 1.65+). It handles user statistics and achievements, async leaderboard scores, and Steam client lifecycle integration.

### 21.1 Configuration & Lifecycle

- **Configuration**: `project.json` contains an optional integer key `steam_app_id` (default `0`). When `0` or omitted, Steam support remains disabled and is never initialized.
- **Initialization timing**: If `steam_app_id != 0`, initialization runs after `project.json` is parsed and before `InitWindow()` so that the Steam overlay hooks properly into the graphics swapchain.
  - **Development (non-shipping)**: Sets the environment variables `SteamAppId` and `SteamGameId` to the configured app ID prior to `SteamAPI_InitEx`, removing the need for a loose `steam_appid.txt` file.
  - **Shipping (`IYI_SHIPPING_BUILD`)**: Calls `SteamAPI_RestartAppIfNecessary(steam_app_id)`. If this returns `true`, the game process terminates cleanly without creating a window or running gameplay.
  - **Failure fallback**: If `SteamAPI_InitEx` fails, a warning is logged via `LogWrite`, and the engine continues running normally with `engine.steam.available() == False`.
- **Per-frame pump**: `SteamAPI_RunCallbacks()` is invoked once per frame from `Application::Run()`.
- **Shutdown order (Hard rule 4)**: `SteamAPI_Shutdown()` is executed as the very last step of application termination after the window and audio devices are closed:
  `Actors` -> `Python finalize` -> `AudioManager` -> `AssetManager (raylib resources)` -> `CloseAudioDevice` -> `CloseWindow` -> `SteamAPI_Shutdown`.

### 21.2 Async Handle & Request Model

Leaderboard operations run asynchronously via Steamworks `CCallResult`:
- **Handle namespace**: Requests return a positive integer handle (`> 0`). A return value of `0` indicates immediate rejection (e.g. Steam unavailable or invalid parameters).
- **Leaderboard caching**: Leaderboard handles (`SteamLeaderboard_t`) are automatically resolved via `FindOrCreateLeaderboard` and cached by name. Subsequent requests for the same leaderboard reuse the cached handle.
- **Status polling**: Games poll the request status with `engine.steam.request_status(handle)`:
  - `"pending"`: Operation is still in flight with the Steam backend.
  - `"ok"`: Request completed successfully.
  - `"failed"`: Network failure, invalid leaderboard, or backend error.
  - `"unknown"`: Handle is invalid, already released, or not tracked.
- **Leaderboard entries**: Retrieved via `engine.steam.leaderboard_entries(handle)` returning a list of dicts `[{"rank": int, "name": str, "score": int, "details": list[int]}]`. Returns an empty list unless status is `"ok"`.
- **Release**: Calling `engine.steam.release(handle)` frees the internal request record and cancels any pending callback.
- **Exception safety**: Steam failures never throw exceptions into Python; operations fail gracefully returning safe defaults (`None`, `False`, `0`, `[]`, `"failed"`).

### 21.3 Stub Behaviour

When the Steamworks SDK is missing at compile time or `steam_app_id` is 0:
- The build succeeds without error.
- All `engine.steam` functions remain callable safe no-ops:
  - `available()` -> `False`
  - `stats_ready()` -> `False`
  - `has_achievement(name)` -> `None`
  - `set_achievement(name)` -> `False`
  - `store_stats()` -> `False`
  - `open_store(app_id)` -> `False`
  - `upload_leaderboard_score(...)` -> `0`
  - `request_leaderboard(...)` -> `0`
  - `request_status(handle)` -> `"unknown"`
  - `leaderboard_entries(handle)` -> `[]`
  - `release(handle)` -> `None`

### 21.4 Python API Surface

All symbols in `engine.steam` are exposed as functions (never module attributes):

```python
engine.steam.available() -> bool
engine.steam.stats_ready() -> bool
engine.steam.has_achievement(name: str) -> bool | None
engine.steam.set_achievement(name: str) -> bool
engine.steam.store_stats() -> bool
engine.steam.open_store(app_id: int) -> bool
# Opens that app's store page in the Steam overlay. False when Steam is unavailable, app_id is not a
# valid App ID, or the overlay is disabled or not hooked into the window yet (a few seconds after
# start): the caller falls back (retry, then e.g. a steam:// or https store URL).
engine.steam.upload_leaderboard_score(name: str, score: int, details: list[int] = [], ascending: bool = False) -> int
# score and each detail are saturated to the signed int32 range natively (logged once), never wrapped.
# A Python int outside int64 raises TypeError.
engine.steam.request_leaderboard(name: str, around_user: bool = True, limit: int = 10, ascending: bool = False) -> int
engine.steam.request_status(handle: int) -> str
engine.steam.leaderboard_entries(handle: int) -> list[dict]
engine.steam.release(handle: int) -> None
# Steam Cloud (ISteamRemoteStorage, synchronous): the game syncs its own small files, under the quota
# set in the Steamworks panel. False / [] / None when Steam or the player's cloud is off.
engine.steam.cloud_enabled() -> bool             # Steam up, cloud on for the account and the game
engine.steam.cloud_files() -> list[dict]         # {name, size, timestamp (Unix seconds)}
engine.steam.cloud_read(name: str) -> bytes | None
engine.steam.cloud_write(name: str, data: bytes) -> bool
engine.steam.cloud_delete(name: str) -> bool
engine.steam.avatar_texture(size: int = 0) -> Texture2D | None
# The player's own Steam avatar (64 x 64; size > 0 resizes it to size x size), engine-owned and cached
# per size. None without Steam, without an avatar, or while Steam downloads it: ask again later.
```
