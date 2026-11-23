# IYI Engine contract: Actors, registry, scene, main loop, threads, fixed updates, assets, Python runtime, hot reload, script contract

Part of the engine contract; index and rules in [../SPEC.md](../SPEC.md). Section numbers are the contract's own.

## 4. `GameActor` (include/IYI/GameActor.h)

Plain virtual base class. Public data members, no getters/setters ceremony.

```cpp
namespace IYI {

class GameActor
{
public:
    GameActor() = default;
    virtual ~GameActor() = default;

    uint64_t    Id   = 0;          // engine-assigned, monotonic, never reused
    std::string Name;              // engine-assigned, unique within its registry
    Vector3     WorldPos { 0.0f, 0.0f, 0.0f };

    bool Active        = true;      // false → no Update/FixedUpdate calls
    bool Visible       = true;      // false → no Drawing call
    bool PendingDestroy = false;
    bool Persistent    = false;     // true → survives change_scene
    bool ScreenSpace   = false;     // true → Drawing runs outside BeginMode2D (HUD)

    std::string Script;             // project-relative .py path, "" for pure native actors

    virtual void Start() {}
    virtual void Update(float dt) {}
    virtual void FixedUpdate30(float dt) {}
    virtual void FixedUpdate60(float dt) {}
    virtual void Drawing() {}

    void Destroy() { PendingDestroy = true; }
    bool IsVisible() const { return Visible; }
};

} // namespace IYI
```

Nothing else. No components, no transform hierarchy, no parenting.

## 5. IDs and names (plan §7)

- ID: one file-static `uint64_t` counter in `ActorRegistry.cpp`, starts at 1,
  monotonic for the whole process run.
- Name: unique **within the owning registry**. If `Enemy` is taken, try
  `Enemy_2`, `Enemy_3`, … until free. An empty requested name becomes
  `Actor` (then `Actor_2`, …).
- The registry keeps a `std::unordered_set<std::string>` of live names; a name is
  released when the actor is actually removed.
- Persistent actors carried into a new scene are re-registered in the new
  registry's name set (and re-uniqued if that name is already taken there).

No GUIDs, no asset identity system.

## 6. `ActorRegistry`

Owns actors: `std::vector<std::unique_ptr<GameActor>> Actors;`

```cpp
GameActor* Spawn(std::unique_ptr<GameActor> actor,
                 const std::string& requestedName,
                 const std::string& script,
                 Vector3 pos);

GameActor* SpawnScripted(const std::string& requestedName,
                         const std::string& script,
                         Vector3 pos);          // base GameActor + Python behavior

void  Destroy(uint64_t id);                      // marks PendingDestroy
GameActor* FindById(uint64_t id);                // nullptr if absent
GameActor* FindByName(const std::string& name);  // nullptr if absent

void StartPending();                             // Start() for actors spawned since last call
void Update(float dt);
void FixedUpdate30(float dt);
void FixedUpdate60(float dt);
void Drawing(bool screenSpacePass);              // draws only actors whose ScreenSpace matches
void RemoveDestroyed();                          // erase + notify PythonRuntime + free names; still-wrapped actors are parked
static void FreeRetired();                       // frees parked actors; only after Python finalize
void DestroyAll(bool keepPersistent);
std::vector<std::unique_ptr<GameActor>> TakePersistent();  // for scene change
void AdoptPersistent(ActorRegistry& from);  // TakePersistent + RegisterActor, keeps a pending Start
```

Rules:

- **Never** erase while iterating. `Destroy` only sets `PendingDestroy`;
  `RemoveDestroyed()` runs once per frame at the very end.
- A removed actor that Python still wraps is set `Alive = false` and parked in one
  process-wide retired list (it survives scene changes), freed on the first
  `RemoveDestroyed()` with no wrapper left; a canvas also waits for its wrapped
  elements. The check is a host hook (`SetActorReferencedHook`, from the bindings).
- Iterate by index with a snapshot of the size taken before the loop, so actors
  spawned mid-iteration are not visited in the same pass.
- Skip `!Active` for Update/FixedUpdate; skip `!Visible` for Drawing; skip
  `PendingDestroy` for everything. `Drawing(screenSpacePass)` additionally skips
  any actor whose `ScreenSpace != screenSpacePass`.
- `StartPending()` keeps a `std::vector<uint64_t>` of ids awaiting `Start()`.
  It drains that list, calling native `Start()` then the Python `Start` hook.
  Actors spawned during the drain get `Start` on the next frame.
- Every lifecycle method calls the native virtual **first**, then, if
  `!actor.Script.empty()`, the matching Python hook.

`ActorRegistry` needs a `PythonRuntime*` to dispatch script hooks. Pass it in via
a plain setter (`void SetPythonRuntime(PythonRuntime*)`) or constructor argument.
`ActorRegistry.cpp` includes `PythonRuntime.h`, which is pybind-free.

## 7. `Scene`

```cpp
class Scene
{
public:
    ActorRegistry Actors;
    std::string   ScriptPath;   // "" for the implicit initial scene
};
```

That is the whole scene system. No scene graph, no serialization, no editor.

Scene change (`engine.change_scene(path)`) is **deferred**: the request is stored
on `Application` and applied at the end of the frame, after `RemoveDestroyed()`:

1. Take persistent actors out of the old registry.
2. Destroy the rest (Python per-actor state for them is freed).
3. Build a fresh `Scene`, move the persistent actors in, re-unique their names.
4. Set `ScriptPath`, then `PythonRuntime::CallSceneStart(path)`.

Only one pending scene change is kept (last request wins).

## 8. `Application` and the main loop

`Application` owns: `ProjectConfig`, `AssetManager`, `PythonRuntime`,
`std::unique_ptr<Scene>`, the fixed-step accumulators, background color,
camera state, and the pending-scene-change slot.

There is exactly one live `Application`; expose it to the binding code through a
file-static pointer inside `EngineBindings.cpp`/`Application.cpp`
(`IYI::Application& CurrentApp()`), not a singleton class with lazy init.

Startup order:

1. Parse CLI.
2. Load `project.json`.
3. `InitWindow(w, h, name)`, `SetTargetFPS(target_fps)`, `InitAudioDevice()`. While the window is
   unfocused or minimized the loop drops to 30 FPS (not in `--frames` runs) and restores `target_fps` on return.
   Also `SetExitKey(KEY_NULL)` so ESC does not close the game silently.
4. `AssetManager::SetProjectDir(...)`.
5. `PythonRuntime::Initialize(projectDir, entryScript, hotReloadEnabled)`:
   this adds `<project>` and `<project>/scripts` to `sys.path` and imports the
   built-in `engine` module.
6. Create the initial `Scene`.
7. Load the entry script and call `GameStart()` once (if defined).
8. Main loop.
9. Shutdown: destroy all actors → `PythonRuntime::Shutdown()` (both on the game thread, which
   then ends) → the GL deletions they recorded → `AssetManager::UnloadAll()` → `CloseAudioDevice()`
   → `CloseWindow()`. (Python must die before raylib resources are unloaded.)

Main loop (`Application::Run`). A frame has three parts (§8a):

```cpp
// main thread (render side)                  // game thread (game side), step k
PlatformFrame(input[k]);   // poll result:   //
                           // IME frame, close, Alt+Enter,
                           // FrameInput copy
hand input[k] to the game  ---------------->  GameStep(packet[k]):
wait for packet[k-1]                             Steam callbacks; PollHotReload; StartPending
RenderPacket(packet[k-1]):                       RunFixedUpdates (60 Hz, then 30 Hz); Update
    Setup pass (window requests, uploads)        agent channel
    Audio pass + AudioManager::Update            Drawing(world), Drawing(screen) recorded
    Offscreen pass (texture modes)               RemoveDestroyed; ApplyPendingSceneChange
    BeginDrawing; world pass in the camera;      ready marker; frame count (--frames)
    screen pass; postprocess; screenshot;     <-  packet[k] done
    EndDrawing (swap, frame-rate wait, poll)
```

The game step of frame k runs while the main thread draws step k-1. Two inputs and two packets
are used in turn; a packet is drawn exactly once, in order. `--single-thread` runs PlatformFrame,
GameStep and RenderPacket of the same frame one after another on the main thread.

**Runtime-ready marker.** The engine logs exactly one line, category `startup`,
message `IYI_RUNTIME_READY` (e.g. `[13:32:42.306] [INFO ] [startup] IYI_RUNTIME_READY`),
only after: Python initialized; `GameStart()` returned without a load/exception
failure; the scene `GameStart()` requested via `change_scene` was applied; that
scene's `SceneStart()` succeeded; and one further complete update/draw frame ran
with the scene live. (If `GameStart()` requests no scene, it is logged after the
first complete frame.) A failing `GameStart()` or `SceneStart()` suppresses it
permanently. Release smoke tests wait for this marker; the older
`m_python.CallGameStart() completed.` line only proves the entry script ran.

The engine draws nothing of its own, no FPS counter, no debug overlay.

## 8a. Threads: game side and render side

**Ownership.** The main thread owns the window, raylib's input state, the OpenGL context, GLFW and
(until an audio thread exists) the AudioManager: the render side. The game thread owns the embedded
interpreter for its whole life (initialize, `GameStart`, every hook, hot reload, finalize), the
actors, scenes and IYIUI: the game side. Python runs only on the game thread; the render side never
calls Python.

**Frame packet (`FramePacket.h`).** Game-side code never makes a GL, GLFW or AudioManager call. It
records each one, in order, as a closure (`Record`, `RecordSetup`, `RecordAudio`); the render side
replays the packet. `RecordState` records a call that draws nothing (matrix, scissor) and `RecordText`
a text draw: consecutive text draws then share one text-mask blend switch, and the next `Record`
closes the run (`BeginTextDraw` / `EndTextDraw` switch only when the blend differs). Passes: Setup (window requests, GPU uploads; first), Audio, Offscreen (texture
modes from Start, Update and other hooks), World and Screen (the Drawing hooks). A closure captures
values, or pointers into the AssetManager cache, never game-side state that may change before the
replay. Engine objects holding GL resources that both sides reference (`StackLighting`,
`CellTerrain`) are shared and deleted on the render side (`RenderOwned`). A recording call reached
on the render side runs at once and warns once.

**Frame input (`FrameInput.h`).** Right after the poll the main thread copies raylib's input
(key/mouse/gamepad state and edges, the key and char queues), the window (size, render size, focus,
fullscreen, monitor modes and refresh), the IME's frame state and the audio side's state (playing
handles, current music, bus volumes). Every game-side reader (`engine.is_key_*`, IYIUI's hit tests,
`get_screen_width`, ...) reads this copy. One copy per game step: a press and its release between
two steps are both seen, each once.

**Consequences for scripts.**
- Window changes (`set_window_size`, `toggle_fullscreen`, `set_vsync`, `set_target_fps`, `set_clipboard_text`,
  `set_window_icon`) run before the render side draws that frame. Threaded, the getters show them
  two game steps later (the next step's input is taken before this step's packet is drawn); with
  `--single-thread`, one. A script that saves what it just asked for keeps its own value.
- `load_*` return at once with the file decoded (sizes known); the GL object is made by the Setup
  pass before the frame draws, so a GL id read on the game side may still be 0 (that read races the
  upload: an int written once, harmless, but do not rely on it). A missing or undecodable file still
  returns None; a shader that fails to compile is only known on the render side: it returns a
  handle, logs a warning there, and draws with raylib's default shader.
- `engine.audio` calls run on the audio side in order. `play` returns its handle at once (0 when the
  file does not exist; a file that fails to decode plays nothing). `is_playing`, `current_music` and
  `get_bus_volume` answer from the frame input, except for what the game side asked since, which
  holds until the audio side has run that call. `stop_all` shows there only then.
- The agent channel answers a screenshot request once the render side has drawn that game step, so
  its `regions`/`pixels` describe the image while `health` and game keys describe a slightly later
  step.
- `get_clipboard_text()` returns the clipboard only in a frame where Ctrl+V (Cmd+V) was pressed.
- What is drawn lags the game step by one frame (pipelining); input to screen adds one frame.

**Profiler.** Each side has its own tree (`game/...`, `render/...`); `game/wait` and `render/wait`
are the time one side waited for the other. Their times overlap: never add the two roots up as one
frame.

## 9. Fixed updates (plan §10)

Two accumulators, nothing generic (`include/IYI/FixedStep.h`, pure functions):

```cpp
inline constexpr float kFixedStep60 = 1.0f / 60.0f;
inline constexpr float kFixedStep30 = 1.0f / 30.0f;
inline constexpr int   kFixedMaxCatchUpSteps = 4;
inline constexpr float kFixedMaxFrameDelta = kFixedStep60 * kFixedMaxCatchUpSteps; // 1/15 s
```

The frame delta is clamped **once** (`ClampFrameDelta`, which also maps negative/NaN
to 0) and that single clamped value is added to both accumulators
(`AdvanceFixedClock`), so a stalled frame (breakpoint, window drag, multi-second
hitch) discards exactly the same time on the 60 Hz and 30 Hz timelines. Each clock
still caps its own catch-up at `kFixedMaxCatchUpSteps` steps per frame. `dt` passed
to the hook is the fixed step, not the frame time; variable `Update(dt)` still
receives the real, unclamped frame time. No scheduler, no other rates.

## 10. `AssetManager`

One cache per resource type, keyed by the project-relative path (forward slashes,
case preserved; packaged archives resolve any casing through the lowercase `ResourceKey`). Returns a pointer into the cache; `nullptr` if the
file is missing (log a warning once per path, never crash).

```cpp
void SetProjectDir(const std::string& dir);
std::string ResolvePath(const std::string& relative) const;   // projectDir + "/" + relative

Texture2D* GetTexture(const std::string& relPath);
Font*      GetFont(const std::string& relPath, int fontSize, const std::string& glyphText = ""); // path + size + canonical glyph set
Shader*    GetShader(const std::string& vsRelPath, const std::string& fsRelPath); // "" = default stage
Sound*     GetSound(const std::string& relPath);
Music*     GetMusic(const std::string& relPath);

void UpdateMusicStreams();   // UpdateMusicStream on every cached Music that IsMusicStreamPlaying
void UnloadAll();            // unload every cached resource, clear maps
```

Repeated calls with the same path must return the **same** resource. No async
loading, no import pipeline, no asset database, no reference counting.

## 11. `PythonRuntime` (public header is pybind-free)

```cpp
namespace IYI {

class GameActor;

enum class ActorHook { Start, Update, FixedUpdate30, FixedUpdate60, Drawing };

class PythonRuntime
{
public:
    PythonRuntime();
    ~PythonRuntime();
    PythonRuntime(const PythonRuntime&) = delete;
    PythonRuntime& operator=(const PythonRuntime&) = delete;

    bool Initialize(const std::string& projectDir,
                    const std::string& entryScript,
                    bool hotReloadEnabled);
    void Shutdown();

    bool CallGameStart();                                  // entry script, once only; false = load/exception failure
    bool CallSceneStart(const std::string& scriptRelPath); // false = load/exception failure (a missing hook is success)
    void CallActorHook(GameActor& actor, ActorHook hook, float dt);

    void PollHotReload(float dt);
    void OnActorDestroyed(uint64_t actorId);               // frees that actor's state dict

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;   // all pybind11 state lives here
};

} // namespace IYI
```

Pimpl is mandatory: `PythonRuntime.h` must compile without Python headers.

### Interpreter startup (must be explicit, do not rely on defaults)

An embedded interpreter does **not** reliably find the CPython standard library
on its own; when it cannot, `Py_Initialize` fails with
`ImportError: initialization failed` and the engine is useless. Initialization
must therefore set the Python home explicitly:

1. CMake passes the discovered interpreter prefix to the compiler as a definition,
   e.g. `target_compile_definitions(IYIEngineCore PUBLIC IYI_PYTHON_HOME="<prefix>")`,
   where `<prefix>` is the interpreter's `sys.base_prefix` (queried from `Python3_EXECUTABLE`).
   No absolute path may be hard-coded in a `.cpp` file.
2. At runtime, resolve the home in this order and use the first that exists:
   an explicit `PYTHONHOME` environment variable, then the compiled-in
   `IYI_PYTHON_HOME`. If neither directory exists, fall back to letting Python
   configure itself rather than forcing a bad path.
3. Initialize through `PyConfig` (`PyConfig_InitPythonConfig`,
   `PyConfig_SetString(&config, &config.home, ...)`, then
   `py::scoped_interpreter guard{&config}`), and always `PyConfig_Clear`.
4. If initialization still fails, print the resolved home, the reason, and a
   one-line hint, then exit cleanly, never leave a half-open window.

Startup must be verified by actually running the engine against a project and
seeing Python-side output, not by inspection.

### Script module loading

Scripts are loaded by the engine, **not** by `import`, so that reload semantics
stay ours:

1. Read the file as UTF-8.
2. `compile(source, absPath, "exec")`, a `SyntaxError` here means the file is
   rejected and any previously loaded version stays active.
3. Create a fresh module object via `types.ModuleType(uniqueName)`, set
   `__file__` to the absolute path, then `exec(code, module.__dict__)`.
4. On success store it in `std::unordered_map<std::string, py::object> m_modules`
   keyed by the normalized project-relative path (`scripts/actors/player.py`).

`uniqueName` is derived from the path, e.g. `iyi_script__scripts_actors_player`.
These modules are **not** registered in `sys.modules`.

Because `<project>/scripts` is on `sys.path`, helper modules still work through
ordinary `import movement_helpers`.

### Hook dispatch

`CallActorHook` looks up the module for `actor.Script`, loading it on first use.
It then fetches the attribute named exactly `Start` / `Update` /
`FixedUpdate30` / `FixedUpdate60` / `Drawing` from the module's dict **at call
time** (never cached across frames, this is what makes hot reload work), and
calls it with `(actor)` for `Start`/`Drawing` and `(actor, dt)` for the three
update hooks. A missing attribute is a silent no-op.

The `GameActor&` is passed to Python as a non-owning reference
(`py::cast(&actor, py::return_value_policy::reference)`); Python must never own
or delete an actor. A removed actor that Python still wraps is parked, not freed, until
no wrapper is left (`SetActorReferencedHook`), so a retained reference never reads freed memory.

### Per-actor script state (plan §14)

`std::unordered_map<uint64_t, py::dict> m_actorState;`

`actor.state` in Python returns that dict, created lazily on first access. It is
owned by the engine, survives every module reload, and is erased in
`OnActorDestroyed`. Never store per-actor state in module globals.

### Error handling (plan §19, approved refinement)

Every call into Python is wrapped. On `py::error_already_set`:

1. Build a key: `"<scriptRelPath>|<hookName>"`.
2. If that key is already in `m_suppressed`, swallow silently.
3. Otherwise print `[IYI] Python error in <script>.<hook>:` followed by the full
   traceback (use `traceback.format_exception`), then insert the key.
4. Reloading that file clears every suppressed key whose prefix is that path.

The native engine never terminates because of a Python exception.

### Hot reload (plan §17–§20)

- Only when `hotReloadEnabled`.
- Poll every **0.25 s** of accumulated `dt`, not every frame.
- Recursively walk `<project>/scripts` for `*.py`, comparing
  `std::filesystem::last_write_time`. No extra dependency; no `FILE_NOTIFY` API.
- New files are recorded, deleted files are dropped from the watch map (and from
  `m_modules`).
- On a changed file:
  - If it is in `m_modules` (an actor / scene / entry script we loaded): re-run
    the compile+exec sequence above into a **new** module object. Replace the old
    one only on full success. On failure, print the traceback and keep the old
    module. Record the new mtime either way, so a broken file does not retry every
    poll.
  - Else, if the file corresponds to a module name present in `sys.modules`
    (a helper module): `importlib.reload` it, guarded by the same error handling.
    This mutates the existing module object in place, which is why game code should
    prefer `import movement_helpers` over `from movement_helpers import f`.
    The reload is atomic all the same: the module's `__dict__` is snapshotted first and
    restored if the reload raises, so a file that fails halfway through leaves the
    previous module exactly as it was (traceback printed, new mtime recorded).
  - Print one line per successful reload: `[IYI] reloaded scripts/actors/player.py`.
    `NoteReloaded` is the only place that emits the console reload line, guaranteeing
    that every reload announced is also recorded in `m_reloadedScripts`.
- Reload hook notification (`ActorRegistry::DispatchReload`): actors whose `Script`
  matches a reloaded path are notified. If a reloaded file matches no actor
  (a helper module or entry script), every live actor receives the `Reload` hook,
  because the engine does not track which actor imported which module. Each actor
  receives at most one `Reload` hook call per dispatch.
  - Accepted consequence: a helper reload rebuilds retained UI, so transient UI
    state such as focus and scroll position resets. This is a deliberate trade-off,
    not a bug.
- What hot reload does **not** re-run:
  - Reloading the entry script must **not** call `GameStart()` again.
  - Reloading a scene script must **not** call `SceneStart()` again.
- State held in the persistent C++ per-actor dictionary (`actor.state`) survives
  a reload and is not migrated. Actors, IDs, names, `WorldPos`, `Active`, `Visible`,
  scene, and game time are all untouched by a reload.
- Known gap: there is no automated regression test for `DispatchReload` ownership
  and broadcast logic; it was verified manually and by a scratch project.

No dependency graph, no module topology tracking.

## 13. Game-side script contract

A native game (§2c) has the same contract in C++ (`include/IYI/Game.h`): `Game::Start()` is
`GameStart`, a registered scene function is `SceneStart`, `GameActor`'s virtuals are the actor hooks
and `Game::AgentRequest` is `AgentRequest`.

### Entry script (`scripts/main.py`)

```python
def GameStart():
    ...
```

Called exactly once, after the window and the initial scene exist. Optional.
Never re-run on reload.

```python
def AgentRequest(request):   # optional, development builds only (§3.1)
    return {...}             # anything JSON can hold; it comes back as the reply's "game"
```

Applies the game-specific keys of an agent request (camera, speed, debug state). Looked up
fresh on every request, like every hook.

### Scene script (`scripts/scenes/<name>.py`)

```python
def SceneStart():
    ...
```

Called once when `engine.change_scene("scripts/scenes/<name>.py")` is applied.
Optional.

### Actor script (`scripts/actors/<name>.py`)

```python
def Start(actor): ...
def Update(actor, dt): ...
def FixedUpdate30(actor, dt): ...
def FixedUpdate60(actor, dt): ...
def Drawing(actor): ...
```

Every function is optional; missing ones do nothing. Functions are looked up
fresh on each call, so editing the file changes behavior immediately. Persistent
per-actor data goes in `actor.state`, never in module globals.
