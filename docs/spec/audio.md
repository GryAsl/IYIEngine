# IYI Engine contract: Audio (`engine.audio`)

Part of the engine contract; index and rules in [../SPEC.md](../SPEC.md). Section numbers are the contract's own.

## 20. Audio subsystem

The audio subsystem provides retained sound effect management via raylib sound aliases, named volume buses with hierarchical math, 2D positional audio attenuation and stereo panning, background music streaming with linear fading, crossfading, a music queue, and persistent per-user JSON bus volume settings.

### 20.1 Asset conventions and ownership

Audio assets reside under the game project's `assets/` directory (e.g. `assets/sounds/`, `assets/music/`):

- **Supported audio formats**: Decoded and streamed via raylib's integrated miniaudio backends: `.wav`, `.ogg`, `.mp3`, `.flac`, `.qoa`, `.xm`, `.mod`.
- **Sounds vs. Music**:
  - **Sound effects** (`AssetManager::GetSound`): Fully decoded into memory as uncompressed audio sample buffers (`Sound` / `Wave`). Intended for short, frequently played sound effects.
  - **Music streams** (`AssetManager::GetMusic`): Streamed incrementally in audio chunks (`LoadMusicStream` / `LoadMusicStreamFromMemory`). In packaged distribution archives (`game.rres`), the raw file bytes are retained in memory (`AssetManager::m_musicBuffers`) for the duration of playback to back the streaming decoder.
- **Asset ownership**: `AssetManager` owns all base `Sound` and `Music` assets and their memory lifetimes. `AudioManager` borrows these assets. When playing sound effects, `AudioManager` creates lightweight alias instances using raylib's `LoadSoundAlias`. Aliases share underlying wave sample data with the base sound without duplicating memory, but maintain independent playheads, volume, pitch, and stereo pan.

### 20.2 Sound handles and instance lifecycle

Sound effects are identified by integer instance handles returned by `engine.audio.play(...)` and `engine.audio.play_at(...)`:

- **Handle allocation**: Handles are positive integers starting at `1` and incrementing sequentially.
- **Invalid handle (`0`)**: A handle value of `0` is strictly reserved to indicate an invalid handle or asset loading / alias allocation failure.
- **Silent no-op guarantee**: Passing an invalid or expired handle to any audio manipulation function (`stop`, `pause`, `resume`, `set_volume`, `set_pitch`, `set_pan`, `set_position`) is safe and silently ignored. Calling `engine.audio.is_playing(h)` with an unknown or expired handle returns `False`.
- **Automatic reaping**: For non-looping sounds (`loop=False`), when playback finishes (`!IsSoundPlaying(alias)` during `AudioManager::Update(dt)`), the engine automatically reaps the instance: `UnloadSoundAlias` is called, and the handle is removed from the active sound map. Any subsequent call referencing that handle becomes a silent no-op.
- **Looping sounds (`loop=True`)**: Loop continuously. When raylib reports playback finished, `AudioManager::Update(dt)` restarts playback via `PlaySound(alias)`. Looping sounds remain alive until explicitly stopped via `engine.audio.stop(handle)` or `engine.audio.stop_all([bus])`.

### 20.3 Bus model and volume hierarchy

Audio playback is organized into named string buses (e.g. `"master"`, `"sfx"`, `"music"`, `"ambient"`, `"ui"`):

- **Built-in buses**: `"master"`, `"music"`, and `"sfx"` are initialized by default.
- **Default bus volume**: Every bus defaults to `1.0` if not explicitly specified. Arbitrary custom bus names can be used dynamically.
- **Volume range and validation**: Bus volumes are strictly clamped to `[0.0, 1.0]`; values above `1.0` do not amplify. `engine.audio.set_bus_volume(name, volume)` rejects non-finite values and clamps finite values to `[0.0, 1.0]`. The same `[0.0, 1.0]` clamp is applied when loading `audio.json` and when applying `ProjectConfig::AudioBusDefaults`.
- **Effective volume hierarchy**:
  $$\text{Effective Sound Volume} = \text{bus\_volume("master")} \times \text{bus\_volume(bus)} \times \text{instance\_volume}$$
  Where $\text{bus\_volume("master")}$ is the master bus multiplier, and $\text{bus\_volume(bus)}$ is the multiplier of the assigned bus. If the assigned bus is `"master"`, its multiplier is $1.0$ (master volume is not squared).
  For music:
  $$\text{Effective Music Volume} = \text{bus\_volume("master")} \times \text{bus\_volume("music")} \times \text{track\_volume} \times \text{fade\_factor}$$
- **Dynamic propagation**: Modifying a bus volume via `engine.audio.set_bus_volume(name, volume)` immediately updates the effective volume of all active sounds assigned to that bus and all playing music streams. Setting `"master"` immediately scales all audio across the engine.
- **Session gain**: `engine.audio.set_bus_gain(name, gain)` is a second multiplier per bus (clamped to `[0.0, 1.0]`, default `1.0`) applied on top of the bus volume, for a capture or a scene that mutes or lowers a bus. It is never written to `audio.json`, so the player's setting stays as the Settings screen left it; only `set_bus_volume` persists.

### 20.4 2D positional audio and spatial falloff

Positional audio simulates spatialized sound effects relative to a 2D listener position:

- **Listener position**: Position of the ear/camera in world coordinates, set via `engine.audio.set_listener(x, y)`. Defaults to `(0.0, 0.0)`.
- **Falloff boundaries**: Set via `engine.audio.set_falloff(near, far)`. Defaults: $\text{near} = 100.0$, $\text{far} = 1000.0$. Constraints: $\text{near} \ge 0.0$, $\text{far} \ge \text{near} + 0.001$.
- **Linear distance attenuation**:
  For an emitter at $(x, y)$ and listener at $(l_x, l_y)$ with distance $d = \sqrt{(x - l_x)^2 + (y - l_y)^2}$:
  $$\text{attenuation} = \begin{cases} 1.0 & \text{if } d \le \text{near} \\ 0.0 & \text{if } d \ge \text{far} \\ 1.0 - \dfrac{d - \text{near}}{\text{far} - \text{near}} & \text{if } \text{near} < d < \text{far} \end{cases}$$
  The final sound volume is scaled by $\text{attenuation}$:
  $$\text{Final Volume} = \text{Effective Volume} \times \text{attenuation}$$
- **Stereo panning**: Horizontal offset $dx = x - l_x$ determines stereo balance across $[0.0, 1.0]$ ($0.0 = \text{left}$, $0.5 = \text{center}$, $1.0 = \text{right}$):
  $$\text{pan} = \text{clamp}\left(0.5 + 0.5 \times \frac{dx}{\max(\text{far}, 1.0)},\ 0.0,\ 1.0\right)$$
  This is the engine's pan for every sound, positional or not; from raylib 6.0 on, `ApplyEffectivePan` maps it to raylib's $[-1, 1]$ (§0.1), so games never see the difference.
- **Manual pan vs. positional mode**: Setting manual pan via `engine.audio.set_pan(h, pan)` disables positional spatialization for that instance. Calling `engine.audio.set_position(h, x, y)` re-enables positional tracking.
- **Positional scope constraint**: Positional 2D audio applies exclusively to sound effects created via `engine.audio.play_at(...)` or updated via `set_position`. Background music (`engine.audio.music`) is non-spatialized stereo on the `"music"` bus.

### 20.5 Music streaming, fades, crossfading, and queue

The music system manages background soundtrack playback, linear fades, smooth crossfading between tracks, and track queueing:

- **Playback**: `engine.audio.music(path, fade=0.0, loop=True, volume=1.0)` starts streaming a track.
- **Linear fades**:
  - `fade=0.0` starts playback immediately at target volume.
  - `fade > 0.0` fades in the track linearly from $0.0$ to target volume over `fade` seconds.
- **Crossfading**:
  - Calling `engine.audio.music(new_path, fade=t)` while a track is currently playing initiates a crossfade over `t` seconds.
  - The previous track becomes the "outgoing music" and decays continuously from its current fade factor to $0.0$ over `t` seconds (avoiding volume snaps if interrupted mid-fade), while the new track becomes "current music" and linearly fades in ($0.0 \to 1.0$).
  - Both audio streams decode and update concurrently during the crossfade. Once the outgoing track reaches volume $0.0$, its stream is stopped.
  - Because there is only a single outgoing stream slot, starting a third track while an outgoing stream is already fading out immediately stops and displaces the older outgoing track.
  - Calling `music()` with the same track that is already playing updates looping/volume and reverses fade-out if active, without restarting playback.
- **Stop and pause**:
  - `engine.audio.stop_music(fade=0.0)` stops playback (fading out over `fade` seconds if `fade > 0.0`).
  - `engine.audio.pause_music()` and `engine.audio.resume_music()` pause and resume playing streams in place.
  - `engine.audio.current_music()` returns the asset path of the currently playing track, or `""` if none.
- **Music queue**:
  - `engine.audio.queue(path)` appends a track path to the playback queue.
  - If no music is currently playing and the queue is empty, `queue(path)` plays the track immediately as non-looping using the last used fade duration.
  - When a non-looping track reaches its end, `AudioManager::Update(dt)` pops the next track from the queue and begins playing it non-looping with the last fade duration.
  - `engine.audio.clear_queue()` removes all pending tracks from the queue.

### 20.6 Settings persistence and `project.json` configuration

Bus volume levels are automatically persisted across game sessions:

- **File location**: Saved as JSON to `audio.json` within the per-user data directory:
  ```text
  <user_data_path>/audio.json
  ```
  (e.g. `%LOCALAPPDATA%/<Publisher>/<Name>/audio.json`, resolved via `engine.user_data_path()`).
- **Disk write throttling**: Changing bus volumes flags the configuration dirty. To avoid disk thrashing during rapid slider dragging, `AudioManager::Update(dt)` flushes dirty settings to disk at most once per second. Pending changes are also flushed immediately during engine shutdown.
- **Resolution fallback hierarchy**:
  1. If `audio.json` exists in `user_data_path()`, bus volumes are loaded from it.
  2. If `audio.json` is missing, initial values are loaded from `project.json`'s optional `"audio"` object (`ProjectConfig::AudioBusDefaults`).
  3. If `"master"`, `"music"`, or `"sfx"` are missing from both, they default to `1.0`.

### 20.7 Engine lifecycle integration

The audio subsystem integrates cleanly into the core application lifecycle:

- **Initialization** (`Application::Initialize`):
  1. `InitAudioDevice()` opens the raylib / miniaudio output device.
  2. `AudioManager::Get().Initialize(&m_assets, m_config)` loads `audio.json` or `project.json` defaults and registers initial buses.
- **Per-frame update** (`Application::RenderPacket`, the render side, §8a): the packet's Audio pass
  (the game step's `engine.audio` calls, in order), then `AudioManager::Get().Update(dt)`:
  1. Flushes dirty bus configuration to `audio.json` if the 1-second throttle timer has elapsed.
  2. Reaps completed non-looping sound aliases and unloads their raylib resources.
  3. Restarts completed looping sound aliases.
  4. Updates positional pan and distance attenuation for all active spatial sounds.
  5. Advances music fade timers and adjusts stream volumes.
  6. Pushes decoded audio buffer chunks to raylib via `UpdateMusicStream` for active current and outgoing streams.
  7. Advances the music queue when non-looping tracks conclude.
- **Shutdown order** (`Application::Shutdown`):
  1. All actors and Python runtime are shut down (on the game thread, which then ends; actors still
     parked for Python are freed right after finalize), and the GL deletions they recorded run on the render side.
  2. `AudioManager::Get().Shutdown()` stops and unloads all sound aliases, stops current and outgoing music streams, clears the music queue, and flushes dirty config to `audio.json`.
  3. `AssetManager::UnloadAll()` unloads base textures, fonts, shaders, sounds, and music streams.
  4. `CloseAudioDevice()` closes the raylib audio hardware context.
  5. `CloseWindow()` destroys the display window.
  *Invariant*: Sound aliases are always unloaded before base sounds, and music streams are always stopped before the audio device is closed.

### 20.8 Python API (`engine.audio`)

The `engine.audio` submodule exposes 22 functions:

```python
# Sound effect playback
engine.audio.play(path: str, volume: float = 1.0, pitch: float = 1.0, pan: float = 0.5, loop: bool = False, bus: str = "sfx") -> int
engine.audio.play_at(path: str, x: float, y: float, volume: float = 1.0, pitch: float = 1.0, loop: bool = False, bus: str = "sfx") -> int

# Sound instance control (safe silent no-op on expired or invalid handle)
engine.audio.stop(h: int) -> None
engine.audio.pause(h: int) -> None
engine.audio.resume(h: int) -> None
engine.audio.is_playing(h: int) -> bool
engine.audio.set_volume(h: int, v: float) -> None
engine.audio.set_pitch(h: int, p: float) -> None
engine.audio.set_pan(h: int, p: float) -> None
engine.audio.set_position(h: int, x: float, y: float) -> None
engine.audio.stop_all(bus: str = "") -> None

# 2D spatial listener & falloff configuration
engine.audio.set_listener(x: float, y: float) -> None
engine.audio.set_falloff(near: float, far: float) -> None

# Music playback & crossfade
engine.audio.music(path: str, fade: float = 0.0, loop: bool = True, volume: float = 1.0) -> None
engine.audio.stop_music(fade: float = 0.0) -> None
engine.audio.pause_music() -> None
engine.audio.resume_music() -> None
engine.audio.set_music_pitch(pitch: float) -> None  # speed + pitch of every stream, 1 normal; kept across tracks
engine.audio.current_music() -> str

# Music queue
engine.audio.queue(path: str) -> None
engine.audio.clear_queue() -> None

# Bus volume control
engine.audio.set_bus_volume(name: str, v: float) -> None
engine.audio.get_bus_volume(name: str) -> float
engine.audio.set_bus_gain(name: str, gain: float) -> None   # session only, never saved
```

### 20.9 Examples

#### UI Button click sound

```python
import engine

# Play a one-shot UI click on the "sfx" bus
handle = engine.audio.play("assets/sounds/ui_click.wav", volume=0.8, bus="sfx")
```

#### Looping background music and crossfade

```python
import engine

# Start playing ambient music with a 2.0s fade-in
engine.audio.music("assets/music/ambient.ogg", fade=2.0, loop=True, volume=0.7)

# Later, trigger combat music with a 1.5s crossfade
engine.audio.music("assets/music/combat.ogg", fade=1.5, loop=True, volume=0.9)
```

#### 2D Positional audio

```python
import engine

# Configure listener (e.g. at player position) and falloff ranges
engine.audio.set_listener(player.x, player.y)
engine.audio.set_falloff(near=50.0, far=600.0)

# Play spatial sound at campfire coordinates
fire_handle = engine.audio.play_at(
    "assets/sounds/campfire.wav",
    x=campfire.x,
    y=campfire.y,
    loop=True,
    bus="sfx"
)
```
