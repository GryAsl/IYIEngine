# IYI Engine cheatsheet

One line per public C++ function in `include/IYI`, grouped by header: what it does, so nobody has to
search the engine for it again. This is not a short list of favourites; if you use, add or change a
public engine function, give it a line here.

The Python API is not repeated here (removed 2026-11-11, it duplicated the contract): signatures are in
`typings/engine/*.pyi`, what each call does in [spec/python-api.md](spec/python-api.md) (§12) and
[SPEC-IYIUI.md](SPEC-IYIUI.md); raylib version differences in [spec/decisions.md](spec/decisions.md) §0.1.

## C++ (engine source)

Public functions and methods in `include/IYI`, grouped by header. Private helpers, data members and plain
constructors are omitted; the engine's internal flow is in `docs/SPEC.md`.

### Application.h

| Function | What it does |
|---|---|
| `CurrentApp()` | The process's single `Application`; the bindings reach the engine through it. |
| `Application::WindowSizePinned()` | True under `--window-size`: `SetWindowSize` and `ToggleFullscreen` do nothing. |
| `Application::PinWindowSize(w, h)` | What `--window-size` does, mid-run (agent `window_size`). |
| `CameraMatrix2D(camera, verticalScale)` | raylib's 2D camera matrix with an extra vertical scale after rotation (1 = unchanged). |
| `WorldCameraToGl(camera)` | The raylib `Camera3D` for a world camera whose z is up: z negated, because the spaces are mirrored. |
| `WorldToScreen3D(camera, world, screen)` | Projects a world point (z up) through that camera; false when it is behind. |
| `Application::Initialize(argc, argv, game = nullptr)` | Parses the CLI, loads the project, starts raylib, logging, audio, Steam and Python (or the native `game`); false on failure. |
| `Application::Run()` | Main loop: polls input, hands each frame to the game side and replays its packet on the render thread. |
| `Application::Shutdown()` | Stops the game and frees Python, GL resources, audio, the window and Steam in the required order. |
| `Application::RequestSceneChange(scene)` | Makes `scene` (a script path, or a native game's registered name) the next scene at the end of the current frame. |
| `Application::RegisterScene(name, sceneStart)` | A native game's scene: `sceneStart` runs when `name` is applied. |
| `Application::CallAgentRequest(json)` | The game's agent keys: `Game::AgentRequest` or the entry script's `AgentRequest`. |
| `Application::SetCamera(camera, verticalScale = 1.0f)` | Draws the world pass through a 2D camera, with an optional screen-space vertical scale. |
| `Application::SetCamera3D(camera)` | Draws the world pass through a 3D camera (z up), depth tested; replaces the 2D camera. |
| `Application::WorldToScreen(world, screen)` | Screen position of a world point through the active 2D or 3D camera; false behind a 3D one. |
| `Application::ClearCamera()` | Turns the world camera off. |
| `Application::SetBackgroundColor(color)` | Background colour the world pass clears to. |
| `Application::SetPostprocessShader(shader, strength, radialWarp = 0.0f, worldOnly = false)` | Full-screen postprocess; `worldOnly` leaves the HUD drawn after it, unchanged. |
| `Application::GetPostprocessRadialWarp()` | The warp UI hit tests must undo; 0 when the HUD is not drawn through the shader. |
| `Application::ResolveProjectPath(relPath)` | Absolute path for a project-relative path. |
| `Application::ToggleFullscreen()` | Toggles borderless-windowed fullscreen; main thread only. Unpins raylib 5.5's topmost flag (6.x no longer sets it). |
| `Application::SetVsync(interval)` | Swap interval: 0 off, 1 every refresh, 2 every second refresh; main thread only. |
| `Application::RequestQuit()` | Ends the run after the current frame. |
| `Application::SetCloseIntercept(enabled)` | On: the close button and Alt+F4 raise a request instead of exiting. |
| `Application::TakeCloseRequest()` | True once per intercepted close; answer with `RequestQuit`. |
| `Application::RequestScreenshot(path)` | Saves the next finished frame (world + HUD, after postprocess) to `path`. |
| `Application::BeginUserTextureMode(target)` | Starts game-side offscreen drawing; false inside the draw pass or when a texture mode is already open. |
| `Application::EndUserTextureMode()` | Closes the mode `BeginUserTextureMode` opened; a no-op when none is open. |
| `Application::GetScene()` | The active scene (its `Actors` and script path). |
| `Application::FrameCount()` | Game steps run so far. |
| `Application::GetAssets()` | The engine's asset cache. |
| `Application::GetConfig()` | The loaded `project.json` values. |

### Game.h (native games, SPEC §2c)

| Function | What it does |
|---|---|
| `Game::Start()` | The native game's `GameStart`: settings, `RegisterScene`, the first `Api::ChangeScene`. |
| `Game::AgentRequest(json)` | Development builds: the game's agent keys, JSON text in, the reply's `"game"` as JSON text out (default `"null"`). |
| `RunGame(argc, argv, game)` | `main()` of a game exe: Initialize, Run, Shutdown; `IYIEngine.exe` passes no game (Python). Returns the exit code. |
| `RegisterScene(name, sceneStart)` | `Api::ChangeScene(name)` runs `sceneStart` after the non-persistent actors are cleared. |
| `SceneActors()` | The current scene's `ActorRegistry`. |
| `Spawn<T>(name, pos, args...)` | Spawns a native actor (a `GameActor` subclass built from `args`); set `Persistent` / `ScreenSpace` on the result. |
| `FindActor<T>(name)` | The live actor of that name if it is a `T`, else nullptr. |

### Api.h (`IYI::Api`: the `engine.*` functions in C++)

One implementation behind both the Python bindings and a native game. raylib's names and arguments where
raylib has the call; always qualify (`Api::X`), since an unqualified name may be raylib's own, which would
touch GL or input on the game thread. Pure raylib helpers (raymath, collision, `MeasureText`, `GetTime`) and
plain engine C++ (`ProfileBegin`, `LogWrite`) are called directly.

| Function | What it does |
|---|---|
| `ClearBackground`, `BeginShaderMode`, `EndShaderMode`, `BeginMode2D`, `EndMode2D` | raylib's, recorded into the current pass. |
| `SetShaderValue(shader, name, value)` / `SetShaderTexture(shader, name, texture)` | Sets a uniform (1 to 4 floats) / sampler, flushing the batch first; set textures last. |
| `DrawTexture`, `DrawTextureV`, `DrawTextureRec`, `DrawTexturePro` | raylib's texture draws, recorded; pass the cached texture (`LoadTexture`), never a copy. |
| `DrawRectangle`, `DrawRectangleRec`, `DrawRectangleLines`, `DrawRectangleLinesEx`, `DrawRectanglePro`, `DrawRectangleRounded`, `DrawRectangleGradientV/H/Ex`, `DrawCircleGradient` | raylib's rectangle draws, recorded. |
| `DrawCircle`, `DrawCircleV`, `DrawCircleLines`, `DrawCircleSector`, `DrawRing`, `DrawSphere`, `DrawPoly` | raylib's round shapes, recorded. |
| `DrawLine`, `DrawLineEx`, `DrawLineStrip`, `DrawSplineLinear`, `DrawTriangle`, `DrawTriangleFan`, `DrawTriangleStrip` | raylib's lines and triangles, recorded (spline: mitered joints). |
| `DrawBillboards(texture, camera, centers, size, tint)` | Camera-facing quads of one texture at world points, one quad stream. |
| `DrawText(text, x, y, size, color)` / `DrawTextEx(font, text, pos, size, spacing, color)` | Unclipped world/HUD text, recorded. |
| `MeasureTextEx(font, text, size, spacing)` | `DrawTextEx`'s size with the same font view. |
| `BeginTextureMode(target)` / `EndTextureMode()` | Game-side offscreen drawing (rule 24); false during the draw pass. |
| `SetBackgroundColor`, `SetPostprocessShader`, `SetSpriteStackCulling`, `SetSpriteStackInstancing` | Frame look settings. |
| `IsKeyDown`, `IsKeyUp`, `IsKeyPressed`, `IsKeyReleased`, `GetKeyPressed`, `IsImeComposing` | Keyboard from this frame's input copy (keys the IME owns read as up). |
| `IsMouseButtonDown/Pressed/Released`, `GetMousePosition`, `GetMouseDelta`, `GetMouseWheelMove`, `GetMouseWheelMoveV` | Mouse from this frame's input copy. |
| `IsGamepadAvailable`, `IsGamepadButtonDown/Pressed`, `GetGamepadAxisMovement` | Gamepads 0..3 from this frame's input copy. |
| `GetFrameTime`, `GetFPS`, `GetScreenWidth`, `GetScreenHeight`, `GetMonitorResolutions`, `GetMonitorRefreshRate`, `IsWindowFullscreen`, `IsWindowFocused`, `GetClipboardText` | The window as of this frame. |
| `SetWindowSize`, `SetWindowTitle`, `ToggleFullscreen`, `SetVsync`, `SetTargetFPS`, `SetClipboardText`, `SetWindowIcon(path, frame, count)` | Window requests the render side runs before it draws (show next frame). |
| `Quit`, `SetCloseIntercept`, `TakeCloseRequest`, `TakeScreenshot(path)` | Run control. |
| `SetCamera(camera, verticalScale)`, `SetCamera3D`, `ClearCamera` | The world camera. |
| `GetWorldToScreen2D`, `GetScreenToWorld2D`, `GetScreenToWorldRay`, `GetWorldToScreen` | Camera math (3D: z up; nullopt behind the camera). |
| `LoadTexture`, `LoadSpriteStack`, `LoadRenderTexture`, `LoadFont`, `LoadShader` | Cached assets (nullptr on failure). |
| `ProjectPath(rel)`, `UserDataPath(subdir)`, `IsShipping()` | Paths and build kind. |
| `RunPath(rel)` | This run's own folder (`Application::RunDir`: --user-data, else the project; user data when shipped). |
| `Api::IsAgentRun()`, `Api::RunSeed(seed)` | `--agent-run`; the `--seed` value (false without it). |
| `ChangeScene(scene)` | A registered scene name or a scene script path, applied at the end of the step. |
| `Steam*` (`SteamBridge.h`), `Audio*` (`AudioManager.h`), `UISpawnCanvas` / `UIPointerOverUI` (`IYIUI.h`), `NavStep` (`NavSim.h`), `DrawSpriteStacks` (`SpriteStack.h`), `MakeCellTerrain` / `DrawCellTerrain` (`CellTerrain.h`), `DrawParticles` (`Particles.h`) | The subsystems' `engine.*` calls, listed under their headers below. |

### Scene.h

Data only: `ActorRegistry Actors` and `std::string ScriptPath` (`""` for the implicit first scene). No functions.

### GameActor.h

| Function | What it does |
|---|---|
| `GameActor::Start()` | Called once when the actor starts (a script's `Start`). |
| `GameActor::Update(dt)` | Called every frame while `Active`. |
| `GameActor::FixedUpdate30(dt)` | Called at 30 Hz while `Active`. |
| `GameActor::FixedUpdate60(dt)` | Called at 60 Hz while `Active`. |
| `GameActor::Drawing()` | Called every frame while `Visible`; world or screen space by `ScreenSpace`. |
| `GameActor::Destroy()` | Sets `PendingDestroy`; the registry removes it later (and clears `Alive`). |
| `GameActor::IsVisible()` | The actor's `Visible` flag. |

### ActorRegistry.h

| Function | What it does |
|---|---|
| `ActorRegistry::SetPythonRuntime(runtime)` | The runtime the registry dispatches actor hooks through. |
| `ActorRegistry::Spawn(actor, requestedName, script, pos)` | Adds `actor` under a unique name, with a script and world position; returns it. |
| `ActorRegistry::SpawnScripted(requestedName, script, pos)` | Creates a `GameActor`, spawns it with `script`, and returns it. |
| `ActorRegistry::Destroy(id)` | Marks the actor for deferred removal at the end of the frame. |
| `ActorRegistry::FindById(id)` | The actor with that id, or `nullptr`. |
| `ActorRegistry::FindByName(name)` | The actor with that unique name, or `nullptr`. |
| `ActorRegistry::StartPending()` | Calls `Start` on actors spawned since the last pass. |
| `ActorRegistry::Update(dt)` | Calls `Update(dt)` on active actors. |
| `ActorRegistry::FixedUpdate30(dt)` | Calls the 30 Hz fixed-update hook on active actors. |
| `ActorRegistry::FixedUpdate60(dt)` | Calls the 60 Hz fixed-update hook on active actors. |
| `ActorRegistry::Drawing(screenSpacePass)` | Calls `Drawing` on visible actors for the world (`false`) or screen (`true`) pass. |
| `ActorRegistry::DispatchReload(reloadedScripts)` | Calls `Reload` on matching actors; a reloaded helper module not owned by any actor notifies every scripted actor. |
| `ActorRegistry::RemoveDestroyed()` | Erases actors marked `PendingDestroy` (still-wrapped ones are parked until Python lets go); once per frame. |
| `ActorRegistry::FreeRetired()` | Frees every parked actor; only after Python is finalized. |
| `SetActorReferencedHook(fn)` | Installs the host check that keeps a removed actor parked while Python still wraps it. |
| `ActorRegistry::DestroyAll(keepPersistent)` | Marks and removes every actor; persistent ones are kept when `keepPersistent`. |
| `ActorRegistry::TakePersistent()` | Moves persistent actors out of the registry and returns them. |
| `ActorRegistry::RegisterActor(actor)` | Adds an existing actor without running spawn logic. |
| `ActorRegistry::AdoptPersistent(from)` | Moves every persistent actor out of `from`, keeping a still-pending `Start`. |
| `ActorRegistry::GetActors()` | The registry's storage, in insertion order. |

### ProjectConfig.h

| Function | What it does |
|---|---|
| `ProjectConfig::Load(projectPath, resources)` | Loads `project.json` and resolves the project through `resources`; false on failure. |
| `ProjectConfig::Load(resources)` | Reads `project.json` through the mounted backend; no path discovery. |
| `ProjectConfig::Load(projectPath)` | Loads from a project path without a resource store. |
| `ProjectConfig::UserDataRoot()` | Per-user writable folder for saves, config and logs. |

### ResourcePath.h

| Function | What it does |
|---|---|
| `ResourceFsPath(path)` | Forward-slash, lexically normalized path with case preserved. |
| `ResourceContainedPath(root, relPath)` | Joins `relPath` to `root`; empty when it would escape the root. |
| `ResourceKey(path)` | The archive key: `ResourceFsPath` lowercased. |

### ResourceStore.h

| Function | What it does |
|---|---|
| `GetDataDir()` | The shipping data folder beside the executable (`<Name>_Data`, or inside a macOS `.app`). |
| `IResourceBackend::Exists(relPath)` | True when `relPath` exists in this backend. |
| `IResourceBackend::ReadBytes(relPath, outBytes)` | Reads a file into bytes; false when missing or unreadable. |
| `IResourceBackend::ReadText(relPath, outText)` | Reads a file into a string. |
| `IResourceBackend::GetPhysicalPath(relPath)` | The real filesystem path when there is one, else `""`. |
| `DevResourceBackend::Exists / ReadBytes / ReadText / GetPhysicalPath` | The interface reads, project first, then each library root in order. |
| `DevResourceBackend::GetProjectDir()` | The project root this backend was built for. |
| `PackagedResourceBackend::Exists / ReadBytes / ReadText / GetPhysicalPath` | The interface reads from the packaged archive. |
| `PackagedResourceBackend::GetArchivePath()` | The archive this backend opened. |
| `PackagedResourceBackend::IsValid()` | True when the archive was found and scanned. |
| `ResourceStore::SetBackend(backend)` | Replaces the backend the store reads through. |
| `ResourceStore::GetBackend()` | The backend in use, or `nullptr`. |
| `ResourceStore::HasBackend()` | True when a backend is attached. |
| `ResourceStore::Exists(relPath)` | True when the backend has `relPath`. |
| `ResourceStore::ReadBytes(relPath, outBytes)` | Reads a file through the backend. |
| `ResourceStore::ReadText(relPath, outText)` | Reads a text file through the backend. |
| `ResourceStore::GetPhysicalPath(relPath)` | The real path when the backend has one, else `""`. |
| `ResourceFsPath(path)` (`ResourcePath.h`) | Filesystem form: forward slashes, normalized, case preserved. |
| `ResourceKey(path)` (`ResourcePath.h`) | Archive form: `ResourceFsPath` lowercased. |

### AssetManager.h

| Function | What it does |
|---|---|
| `AssetManager::SetProjectDir(dir, libraryDirs = {})` | Sets the project root and the ordered library roots searched after it. |
| `AssetManager::ResolvePath(relative)` | Absolute path for a project-relative asset path. |
| `AssetManager::GetTexture(relPath)` | Cached texture, loaded on first use; `nullptr` when missing. |
| `AssetManager::GetFont(relPath, fontSize, glyphText = "")` | Cached font at `fontSize`, adding `glyphText`'s codepoints; `nullptr` on failure. |
| `AssetManager::FontForText(font, text)` | A cache-owned version of `font` with every glyph of `text`, safe to keep in a recorded draw. |
| `AssetManager::FontGeneration()` | Grows whenever a `FontForText` answer could change; keep an answer while it is unchanged. |
| `AssetManager::GetShader(vsRelPath, fsRelPath)` | Cached vertex/fragment shader pair; `nullptr` on compile failure. |
| `AssetManager::GetSound(relPath)` | Cached sound; `nullptr` on failure. |
| `AssetManager::GetMusic(relPath)` | Cached music stream; `nullptr` on failure. |
| `AssetManager::GetRenderTexture(name, width, height)` | Engine-owned offscreen target, cached as `name#WxH`. |
| `AssetManager::GetSpriteStack(layerPaths)` | Packs equal-sized layer images (bottom first) into one cached sprite stack. |
| `AssetManager::Stats()` | Cache counts, render-texture keys and an estimated GPU size. |
| `AssetManager::UpdateMusicStreams()` | Refills the active music stream each frame. |
| `AssetManager::UnloadAll()` | Frees every cached GPU/audio object; render side, at shutdown. |
| `AssetManager::ForgetFailedPaths()` | Lets paths that failed to load be tried again after a hot reload. |
| `AssetManager::GetResourceStore()` | The resource backend store behind the cache. |

### AudioManager.h

| Function | What it does |
|---|---|
| `Api::AudioPlay(path, bus, volume, pitch, loop)` / `Api::AudioPlayAt(path, x, y, ...)` | engine.audio's play calls from the game side: a handle at once (0: file missing), the play recorded into the Audio pass. |
| `Api::AudioStop/Pause/Resume/IsPlaying/SetVolume/SetPitch/SetPan/SetPosition(handle, ...)`, `Api::AudioStopAll(bus)` | Per-sound calls, recorded; queries answer from the game side's view of the audio state. |
| `Api::AudioSetListener`, `Api::AudioSetFalloff`, `Api::AudioSetBusVolume`, `Api::AudioGetBusVolume`, `Api::AudioSetBusGain` | Listener, 2D falloff, bus volumes and session gains. |
| `Api::AudioMusic`, `Api::AudioStopMusic`, `Api::AudioPauseMusic`, `Api::AudioResumeMusic`, `Api::AudioCurrentMusic`, `Api::AudioQueue`, `Api::AudioClearQueue` | Music streaming, fades and the queue. |
| `AudioManager::Get()` | The process's single audio manager. |
| `AudioManager::Initialize(assets, config)` | Stores the assets, bus defaults and user-data folder; `Application` opens the device first. |
| `AudioManager::Shutdown()` | Stops every voice and track and flushes bus settings; `Application` closes the device afterwards. |
| `AudioManager::Update(dt)` | Reaps finished voices, restarts loops, advances fades and the track queue, and saves dirty bus settings. |
| `AudioManager::Play(handle, path, volume = 1.0f, pitch = 1.0f, pan = 0.5f, loop = false, bus = "sfx")` | Starts a non-positional sound; the game side allocates `handle` before recording the call. |
| `AudioManager::PlayAt(handle, path, x, y, volume = 1.0f, pitch = 1.0f, loop = false, bus = "sfx")` | Starts a positional sound at world `(x, y)`. |
| `AudioManager::PlayingHandles()` | Handles currently playing, for the frame input's audio snapshot. |
| `AudioManager::BusVolumes()` | Current volume of every bus. |
| `AudioManager::Stop(handle)` | Stops one voice. |
| `AudioManager::Pause(handle)` | Pauses one voice. |
| `AudioManager::Resume(handle)` | Resumes one paused voice. |
| `AudioManager::IsPlaying(handle)` | True while that voice is playing. |
| `AudioManager::SetVolume(handle, volume)` | Sets one voice's volume. |
| `AudioManager::SetPitch(handle, pitch)` | Sets one voice's pitch. |
| `AudioManager::SetPan(handle, pan)` | Sets one voice's pan (0 left, 0.5 centre, 1 right); mapped to raylib 6.x's -1..1 internally. |
| `AudioManager::SetPosition(handle, x, y)` | Moves a positional voice. |
| `AudioManager::StopAll(bus = "")` | Stops every voice, or only those on `bus`. |
| `AudioManager::SetListener(x, y)` | Listener position for positional falloff. |
| `AudioManager::SetFalloff(nearDist, farDist)` | Near and far distances of the positional falloff. |
| `AudioManager::PlayMusic(path, fade = 0.0f, loop = true, volume = 1.0f)` | Starts `path`, crossfading from the current track over `fade` seconds. |
| `AudioManager::StopMusic(fade = 0.0f)` | Fades the current track out over `fade` seconds. |
| `AudioManager::PauseMusic()` | Pauses the current track. |
| `AudioManager::ResumeMusic()` | Resumes the paused track. |
| `AudioManager::CurrentMusic()` | Path of the track playing now, or `""`. |
| `AudioManager::Queue(path)` | Plays `path` after the current track ends. |
| `AudioManager::ClearQueue()` | Drops the queued tracks. |
| `AudioManager::SetBusVolume(name, volume)` | Sets a bus volume and marks it for saving. |
| `AudioManager::GetBusVolume(name)` | A bus's current volume. |
| `AudioManager::SetBusGain(name, gain)` | Sets a session-only bus multiplier (not saved). |

### PythonRuntime.h

| Function | What it does |
|---|---|
| `PythonRuntime::Initialize(projectDir, entryScript, hotReloadEnabled, resources = nullptr, libraryDirs = {}, nativeDirs = {})` | Starts CPython, puts the project, libraries and native folders on `sys.path`, imports `engine` and records the entry script. |
| `PythonRuntime::Shutdown()` | Finalizes CPython. |
| `PythonRuntime::CallGameStart()` | Runs the entry script's `GameStart`; false when it failed (a missing hook is success). |
| `PythonRuntime::CallSceneStart(scriptRelPath)` | Runs a scene script's `SceneStart`; false when it failed. |
| `PythonRuntime::CallActorHook(actor, hook, dt)` | Runs one lifecycle hook of `actor`, timed under its script path. |
| `PythonRuntime::CallAgentRequest(requestJson)` | Calls the entry script's `AgentRequest`; JSON reply, `"null"` when missing, an error object when it raised. |
| `PythonRuntime::PollHotReload(dt)` | Checks script mtimes and reloads changed modules. |
| `PythonRuntime::EnableFaultStacks(crashFile, hangFile)`, `ArmHangDump(now)` | faulthandler stacks for the crash report and the hang watchdog (SPEC §3.2). |
| `PythonRuntime::TakeReloadedScripts()` | Returns and clears the paths reloaded since the last call. |
| `PythonRuntime::OnActorDestroyed(actorId)` | Drops the runtime's per-actor Python state. |

### FrameInput.h

| Function | What it does |
|---|---|
| `TakeFrameInput(in)` | Render side: copies raylib's input and window state for one game step. |
| `SetGameInput(input)` | Game side: sets the copy the next step reads. |
| `GameInput()` | The input copy for the current step. |
| `InputBit(bits, index)` | A bit of one of the input bitsets; false for an out-of-range index. |
| `PopInput(queue)` | Pops one queued key or char code; 0 when empty. |

### FramePacket.h

| Function | What it does |
|---|---|
| `FramePacket::Clear()` | Empties every pass and resets the frame state. |
| `Record(command)` | Appends a GL call to the packet's current pass. |
| `RecordState(command)` | Appends a call that draws nothing (matrix, scissor); an open text run stays open. |
| `RecordText(command)` | Appends a text draw that calls `BeginTextDraw` itself; consecutive ones share one text blend. |
| `RecordTo(pass, command)` | Appends to a named pass regardless of the current one. |
| `RecordSetup(command)` | Appends a window call or GPU upload to the Setup pass. |
| `RecordAudio(command)` | Appends an `AudioManager` call to the Audio pass. |
| `SetRecordingPacket(packet)` | Chooses the packet `Record` fills. |
| `SetRecordPass(pass)` | Chooses the pass `Record` appends to. |
| `CurrentRecordPass()` | The pass `Record` appends to. |
| `ReplayPass(packet, pass)` | Render side: runs one pass in recorded order. |
| `ReplayingWorld3D()` | True while the world pass is replayed through a 3D camera. |
| `SetRenderSide(on)` | Marks the calling thread as the render side. |
| `OnRenderSide()` | True on the thread acting as the render side. |
| `RenderOwned(object)` | A shared owner for a GL-holding object that deletes it on the render side. |

### FixedStep.h

| Function | What it does |
|---|---|
| `ClampFrameDelta(dt)` | Clamps a frame delta once for every fixed clock; non-positive and NaN become 0. |
| `AdvanceFixedClock(acc, dt, step)` | Adds a clamped delta to an accumulator and returns the fixed steps to run (at most 4). |

Constants: `kFixedStep60`, `kFixedStep30`, `kFixedMaxCatchUpSteps`, `kFixedMaxFrameDelta`.

### SpriteStack.h

| Function | What it does |
|---|---|
| `Api::DrawSpriteStacks(items, pass)` | Records the culled, depth-sorted stack draw (rule 25) with its profile scope. |
| `SpriteStackList::Set(index, item)` | Replaces one item of a kept list, with the edit bookkeeping the static set needs. |
| `SpriteStack::LayerRect(layer)` | Source rectangle of one layer in the packed atlas. |
| `BuildSpriteStack(layers, out, atlas)` | Packs equal-sized RGBA layers into an atlas and fills the stack; false on mismatch. |
| `BuildLayer(build, layer, layers)` | Alpha and drop offset of a layer while a stack is being built (0 = none, 1 = finished). |
| `SwayLean(item, pass)` | Degrees the wind adds to an item's lean in this pass; 0 without sway. |
| `SpriteStackOffTarget(item, pass, mvp)` | True when none of the item's quads can land in clip space. |
| `DrawSpriteStacks(items, pass)` | Culls, sorts and draws a list of stacks in one raylib batch. |
| `SetSpriteStackCulling(enabled)` | Turns stack culling off for measurement; on by default. |

### Nav.h

| Function | What it does |
|---|---|
| `NavPlanner::InitGrid`, `SetBase`, `SetBaseCells`, `SetExtra`, `Cell`, `SetGraph`, `SetEdge`, `FindGrid` | The planner's validated setters and grid search (throw `std::invalid_argument` / `std::out_of_range`). |
| `FindNavGridPath(grid, query)` | Grid A* with cell costs, extra-bit costs and clearance; string-pulled points. |
| `FindNavGraphPath(graph, query)` | Road graph A* over open, wide enough edges, priced by kind, hazard and delay. |
| `BuildNavLinks(graph)` | Fills each node's outgoing edge list after the edges change. |
| `NavPlanner::Request(query)` / `Poll(wait)` / `Pending()` | Queue a search on the worker (on a snapshot); collect results in ticket order. |

### StackLighting.h

| Function | What it does |
|---|---|
| `StackLightingHandle(settings, shaders, lightSprites)` | A lighting pass a game owns; records its setup. |
| `StackLightingHandle::Render(moving, layerHeight, sun, ambient, lamps, lights, stat, ground, sunArea)` | Records this frame's sun, contact and light maps. |
| `StackLightingHandle::DrawTerrain`, `DrawGround`, `DrawGroundLight`, `DrawGlow`, `DrawWet`, `DrawSilhouettes`, `DrawStacks` | Record the lit draws. |
| `StackLightingHandle::SetMarks`, `SetClouds`, `SetHeightShade`, `SetWind`, `SetWash`, `StaticChanged(key)` | Ground marks and look settings (clamped); whether the static set changed. |
| `InitStackLighting(fx, settings)` | Creates the lighting targets and caches the shader uniform locations. |
| `RenderStackLighting(fx, moving, layerHeight, sun, ambient, lamps, lights, stat, ground = nullptr, sunArea = Rectangle{})` | Offscreen passes: static cache, shadows, contact and light map for this frame. |
| `DrawStackLightingGround(fx)` | World pass: contact and sun shadows over the flat ground. |
| `DrawStackLightingGroundLight(fx)` | World pass: the night light map over the ground. |
| `DrawStackLightingWet(fx, patches, wetness, viewRotation, sky, eye = nullptr)` | World pass: wet ground patches, darkened and reflecting the light map. |
| `DrawStackLightingGlow(fx, strength)` | World pass: a wide glow round the lights, from the light map. |
| `DrawLitStacks(fx, items, pass)` | World pass: stacks lit per texel, with clouds and the sun map. |
| `DrawLitTerrain(fx, terrain, texture, area, wall)` | World pass: terrain through the terrain shader, with sun-map shadows and contact. |

### CellTerrain.h

| Function | What it does |
|---|---|
| `Api::MakeCellTerrain(columns, rows, cell, origin, corners, water = {})` | Validates and builds a render-owned terrain (throws `std::invalid_argument`). |
| `Api::DrawCellTerrain(terrain, texture, area, wall, upsideDown = true)` | Records the terrain draw. |
| `BuildCellTerrainWalls(terrain)` | Finds every cliff wall from the corner heights; call after filling `Corners`. |
| `CellTerrainHeight(terrain, x, y)` | Bilinearly interpolated ground height at world `(x, y)`; 0 off the grid. |
| `DrawCellTerrain(terrain, texture, area, wall, upsideDown, shader = nullptr)` | Draws the cells overlapping `area`: textured tops and coloured walls; a shader's `uTerrainHeights` gets the height map (unit 15) for per-pixel cliff shade. |

### Particles.h

| Function | What it does |
|---|---|
| `Api::DrawParticles(pool, viewRotation = 0, heightScale = 0, zUp = false, ground = nullptr)` | Records the particle draw. |
| `EmitParticle(pool, p)` | Adds a particle; when the pool is full, replaces the oldest. |
| `UpdateParticles(pool, dt)` | Ages, moves and drops particles; a particle never sinks below the ground. |
| `ParticleLook(p)` | The particle's diameter and alpha at its age. |
| `DrawParticles(live, viewRotation, heightScale, zUp = false, ground = nullptr)` | World pass: every particle as a camera-facing soft disc, lifted by its height. |
| `UnloadParticleTexture()` | Frees the shared puff texture at shutdown. |

### LocalizationManager.h

| Function | What it does |
|---|---|
| `LocalizationManager::Get()` | The process's single localization manager. |
| `LocalizationManager::Initialize(assets, fallback = "en")` | Loads locales through the asset manager and sets the fallback language. |
| `LocalizationManager::SetLanguage(code)` | Switches the active language and loads its catalog. |
| `LocalizationManager::Revision()` | Grows whenever any text could change (language, fallback, reload, hot reload); UI text caches by it. |
| `LocalizationManager::GetLanguage()` | The active language code. |
| `LocalizationManager::SetFallbackLanguage(code)` | Sets the language used when a key is missing. |
| `LocalizationManager::GetFallbackLanguage()` | The fallback language code. |
| `LocalizationManager::Text(key, args = {})` | The localized string for `key`, with `${name}` placeholders filled from `args`. |
| `LocalizationManager::Has(key)` | True when the active or fallback catalog has `key`. |
| `LocalizationManager::Languages()` | The discovered locales, in catalog order. |
| `LocalizationManager::GetLanguageInfo(code)` | Info for a language code, or `nullptr`. |
| `LocalizationManager::MissingKeys()` | Keys requested since the last reload but found nowhere. |
| `LocalizationManager::Reload()` | Reloads the active and fallback catalogs from disk. |
| `LocalizationManager::PollForChanges(dt)` | Dev only: reloads a locale file whose mtime changed. |

### Profiler.h

| Function | What it does |
|---|---|
| `ProfileSetThread(thread)` | Selects the tree (game or render) this thread's sections go to. |
| `ProfileBegin(name, script = false)` | Opens a nested section; `script` marks one opened from Python. |
| `ProfileEnd(script = false)` | Closes the innermost open section; a script may only close its own. |
| `ProfileDepth()` | Open-section depth on this thread. |
| `ProfileOpenSection(thread, wait)` | That thread's innermost open section path, read from any thread (watchdog, crash report). |
| `ProfileEndTo(depth)` | Closes every section above `depth`. |
| `ProfileEndFrame()` | Ends the frame for this tree, closing anything left open. |
| `ProfileCount(counter, n)` | Adds `n` to a named counter on the innermost open section. |
| `ProfileResults()` | A copy of the last one-second window of both trees. |
| `ProfileFps()` | Measured render frames per second, or the game tree's when nothing renders. |
| `ProfilePublishPartial()` | Publishes a window shorter than a second (short `--frames` runs, at exit). |
| `ProfileInitGpu()` | Looks up GL timestamp queries; once, on the render thread, after the window exists. |
| `ProfileSetGpu(enabled)` | Turns GPU timing on or off; false when the driver cannot. |
| `ProfileScope(name)` | Times a C++ scope as a section, closing it on every exit. |

### Log.h

| Function | What it does |
|---|---|
| `LogInit(projectDir)` | Opens `<projectDir>/logs/latest.log` (the user-data folder in shipping) and rotates old logs. |
| `LogShutdown()` | Flushes and closes the log. |
| `LogWrite(level, category, message)` | Writes one aligned line at `level` in `category`; all engine output goes here. |
| `LogSetMinLevel(level)` | Drops messages below `level`. |
| `LogParseLevel(text, ok)` | Parses `debug`, `info`, `warn` or `error`; `ok` says whether it matched. |
| `LogWarnCount()` | Warnings logged since start. |
| `LogErrorCount()` | Errors logged since start. |
| `LogRecentLines()` | The last 40 Info/Warn/Error lines, for the agent channel health report. |
| `LogEvent(kind, fields)` / `LogEventJson(kind, object)` | One structured event in logs/events.jsonl (fields as strings / as a JSON object). |
| `LogSetFrame(frame)`, `LogEventCount()`, `LogEventsSince(mark)` | The frame events carry; events so far; the lines after a count (last 500 kept). |
| `UI::WarpedScreenPoint(screenPoint)` | Where a UI-drawn screen point shows after the postprocess radial warp (caret, agent clicks). |
| `ApplyAgentInput(channel)` | Game side, before the hooks: lays the agent's next click/key/type/scroll/drag step over GameInput(). |

### AgentChannel.h

| Function | What it does |
|---|---|
| `UpdateAgentChannel(channel, dt)` | Polls for a request four times a second and advances the one being served; input keys `click`, `type`, `key` (string or `{key, hold_frames, modifiers}`), `scroll`, `drag`, `scope` refuse hidden/disabled/offscreen/occluded/ambiguous targets (SPEC §3.1, reply `protocol` 2). |
| `TrackUi(names)` | Game side: the named UI elements as this frame draws them (rect, opacity, visible), JSON. |
| `ProfileJson()` | The profiler's last window as JSON (also used by the `--profile` shutdown log). |

### ImageCompare.h

| Function | What it does |
|---|---|
| `CompareImages(base, current, options, diff)` | Two same-size RGBA8 images: changed pixels past a tolerance, regions of non-noise tiles, block SSIM, masks; optional diff image (visual tests, agent `compare`). |
| `CompareImagesCli(jobPath)` | `--compare-images`: runs a JSON job of PNG pairs and writes the results (docs/VISUAL-TESTS.md). |

### Utf8Text.h

| Function | What it does |
|---|---|
| `ClassifyCodepoint(cp)` | Reject, Letter, Digit or Symbol for a codepoint; controls and surrogates are rejected. |
| `AppendUtf8(out, cp)` | Appends `cp` to `out` as UTF-8. |
| `PopLastCodepoint(s)` | Removes the last codepoint and its continuation bytes; false when empty. |
| `CountCodepoints(s)` | Number of UTF-8 codepoints (lead bytes). |
| `FilterPaste(clip, letters, digits, symbols)` | Keeps only pasted codepoints the input allows; caps at 1024 and drops invalid bytes. |
| `AcceptCodepoint(cp, letters, digits, symbols)` | Whether a TextInput's allow_* flags keep a codepoint; shared by typing and `FilterPaste`. |

### Ime.h

| Function | What it does |
|---|---|
| `ImeInitialize(window)` | Attaches the IME to the window and its message hook. |
| `ImeShutdown()` | Detaches and releases the IME. |
| `ImeBeginFrame()` | Starts the per-frame IME latch, after the previous poll. |
| `ImeEndFrame()` | Ends the latch, before the next `EndDrawing` poll. |
| `ImeKeyboardCaptured()` | True while the IME owns the keyboard this frame. |
| `ImeTakeFrame()` | The IME state copied into the game step's frame input. |
| `ImeRequestNow(owner, x, y, height, viewportWidth, viewportHeight)` | Main thread: places the composition/candidate window at the caret now. |
| `ImeCancelOwnerNow(owner)` | Main thread: cancels composition for `owner` now. |
| `ImeRequest(owner, x, y, height, viewportWidth, viewportHeight)` | Game side: asks for the IME window at the caret, as a recorded platform request. |
| `ImeCancelOwner(owner)` | Game side: cancels `owner` before focus changes or it is removed. |
| `ImeAcceptText(owner)` | True when this step's committed text belongs to `owner`. |

### SteamBridge.h

| Function | What it does |
|---|---|
| `Api::SteamAvailable()`, `SteamStatsReady()`, `SteamHasAchievement(name)`, `SteamSetAchievement(name)`, `SteamStoreStats()`, `SteamOpenStore(appId)` | engine.steam for C++; safe stubs without Steam (`HasAchievement`: nullopt = unknown). |
| `Api::SteamUploadLeaderboardScore`, `SteamRequestLeaderboard`, `SteamRequestStatus`, `SteamLeaderboardEntries`, `SteamRelease` | Async leaderboard requests by handle. |
| `Api::SteamCloudEnabled`, `SteamCloudFiles`, `SteamCloudRead`, `SteamCloudWrite`, `SteamCloudDelete` | Steam Cloud files, synchronous. |

### CrashReport.h

| Function | What it does |
|---|---|
| `CrashReportInstall(logsDir, gameName, dialog)` | Crash handler: `crash.dmp`, `crash.txt`, an optional message box (SPEC §3.2). |
| `WatchdogStart(agentioDir)`, `WatchdogFrame()`, `WatchdogStop()` | Dev-only hang watchdog: `heartbeat.json`, stall log lines. |
| `ClampToInt32(v)` | Saturates an int64 to the int32 range; never wraps. |
| `NextFreeHandle(counter, inUse)` | Next positive request handle, skipping handles still in use. |
| `SteamStartup(appId)` | Initializes Steamworks for `appId`. |
| `SteamRunFrame()` | Pumps Steam callbacks; call once per frame. |
| `SteamShutdown()` | Shuts Steamworks down; last in the shutdown order. |

### Trailer.h (development builds only)

| Function | What it does |
|---|---|
| `TrailerConfigure(shot, configPath)` | Reads `--trailer-shot` / `--trailer-config`; false on a bad config. |
| `TrailerGameStep(dt, runtimeReady)` | Spawns the shot actor once the runtime is ready; fails the shot on a logged error or the timeout. |
| `TrailerExitCode()` | Process exit code of a shot run: 0 done or no shot, 2 shot error, 3 never finished. |
| `TrailerDefaults()` | The settings `engine.trailer.start` starts from (config values, else built-in). |
| `TrailerStatusNow()` | Copy of the recording status (state, elapsed, mp4, report, error). |
| `TrailerMarkState(state)` | Sets the status state from the game side before the render side acts. |
| `TrailerCanvasSize(width, height)` | A shot run's offscreen canvas size (the configured recording size); false outside a shot run. |
| `TrailerStart(settings)` | Render side: starts the FFmpeg encoder and the audio tap. |
| `TrailerCaptureFrame()` | Render side: reads the finished frame (the canvas in a shot run) and queues it at real-time pace. |
| `TrailerStop()` | Render side: finishes the video, muxes AAC audio, writes capture.json; the final encode uses `TrailerSettings::VideoArgs` (`docs/VIDEO-ENCODING.md`). |
| `TrailerTrackUi(names)` | Game side, before a start: the elements whose rects the next recording reports (`ui`). |
| `TrailerAddUiSample(ranges, t, sample)` | Extends the last `TrailerUiRange` when the element looks the same at t, else starts one. |
| `TrailerSpawn(args, logPath, pipeInput, out)` | Starts a child process with its output in `logPath`, optionally with a stdin pipe. |
| `TrailerWrite(process, data, size)` | Writes all bytes to the child's stdin; false when the pipe broke. |
| `TrailerWait(process)` | Closes stdin and waits; returns the exit code (-1 on failure). |
| `TrailerFrameRepeats(seconds, fps, nextSlot)` | How many output frames a frame captured at `seconds` fills (0 = surplus, skipped). |
| `TrailerSnapSampleRate(measured)` | The standard audio rate closest to a measured one, or 0 when none is within 3%. |
| `TrailerQuoteArgs(args)` | Windows command line for `args` (CommandLineToArgvW quoting). |

### Video.h

| Function | What it does |
|---|---|
| `OpenVideo(relPath)` | Game side: a `Video` (Video.h) shared with its draw commands, deleted on the render side. |
| `Video::Play/Pause/Resume/Stop/Close/SetVolume/DrawFit` | Game side: record the render-side work; the state shows at once. |
| `Video::DrawRegion(src, dst, tint)` | Records the current frame's src part into dst (a UI image's `video`). |
| `Video(store, relPath)` | Opens through a given ResourceStore (tests); `Video(relPath)` uses the running game's. |
| `UpdateVideos(dt)` | Render side, once per frame: ticks every started video (clock, frame upload, audio feed). |

### Sight.h, NavSim.h, Life.h, PyExact.h (`IYIEngineSim`: no window, draw, input, audio or GPU)

| Function | What it does |
|---|---|
| `SightGrid::SetGround`, `SetBlockers`, `SetBlockerCells`, `SetFoliage`, `Ground`, `Top`, `Visible(a, b, &foliage)`, `Ray` | The sight grid behind `engine.SightGrid`; `SightPair` is one observer/target query. |
| `NavSim` (`NavMap`, `NavWorld`, `NavClasses`, `NavWalker` / `NavVehicle` / `NavHeli`) | The traffic, walker and aircraft simulation behind `engine.Nav` (library `nav`); `Api::NavStep` is the game-side call. |
| `Scheduler`, `EventLog`, `Facts`, `LifeZone`, `Population::Manage(nav, focus, hour)` | The Life simulation behind `engine.Scheduler`, `EventLog`, `Facts` and `Population` (beliefs fade with `Confidence`; `Shock` raises fear in a zone). |
| `PyExact::Random`, `hypot`, `floordiv`, `mod`, `pow`, `ifloor`, `iceil`, `Sha512` | C++ that reproduces Python's `random`, `math` and integer rules bit for bit, so native and Python runs agree (`engine.PyRandom`). |

### ReleaseSecret.h

| Function | What it does |
|---|---|
| `ReleaseSecret::Get()` | The reassembled null-terminated secret; `""` when empty. |
| `ReleaseSecret::Size()` | Secret length in bytes, excluding the terminator. |
| `ReleaseSecret::IsEmpty()` | True when the secret has no bytes. |
| `ReleaseSecret::Wipe()` | Zeroes the buffer with volatile writes and clears it. |

### IYIUI.h

| Function | What it does |
|---|---|
| `Api::UISpawnCanvas(name, script, refW, refH, match, persistent)` | Spawns a `UICanvas` actor into the current scene. |
| `Api::UIPointerOverUI()` | True when any live canvas of the scene has the pointer. |
| `DrawTextFitted(font, text, box, fontSize, spacing, color, hAlign, vAlign, fit, memo = nullptr)` | Draws text so it always stays inside `box`: wrap, shrink, ellipsis, then scissor. |
| `CreateModernTheme()` | Returns the built-in rounded, soft-shadow theme. |
| `CreateClassicTheme()` | Returns the built-in square, beveled retro theme. |
| `CreateClassicDarkTheme()` | Returns the classic layout in a dark charcoal palette. |
| `WithOpacity(color, o)` | Returns `color` with its alpha multiplied by `o` (clamped 0..1). |
| `SetUIElementReferencedHook(fn)` | Installs the host check that keeps a retired subtree alive while anything outside holds it. |
| `DrawIcon(name, area, color)` | Draws a procedural icon into `area`; an unknown name draws nothing and warns once. |
| `GetIconNames()` | Returns the names `DrawIcon` accepts. |
| `UITransform2D::Identity()` | Returns the identity transform. |
| `UITransform2D::Translate(x, y)` | Returns a translation-only transform. |
| `UITransform2D::ScaleRotateAround(origin, scale, degrees)` | Returns scale plus clockwise rotation about `origin`. |
| `UITransform2D::Compose(child)` | Returns this transform multiplied by `child`. |
| `UITransform2D::Apply(p)` | Maps a point through the transform. |
| `UITransform2D::ApplyInverse(p)` | Maps a point back; used to turn the mouse into logical space. |
| `UITextMemo::NeedsUpdate(text, boxW, boxH, reqFontSize, font, fit)` | True when any fitted-text input changed since the memo was computed. |
| `UITextMemo::UpdateInputs(text, boxW, boxH, reqFontSize, font, fit)` | Records the inputs the memo result belongs to and marks it valid. |
| `UITheme::Get(styleClass)` | Returns the style for a class, creating it on demand. |
| `UITheme::Resolve(styleClass)` | Returns the style for a class, falling back when it is absent. |
| `UITheme::Extend(styleClass, base)` | Makes a class a copy of `base` and returns it (throws on an unknown base). |
| `UITheme::ApplyTypeScale(textScale)` | Sets every `FontRole` style's size to its `TypeScale` size times `textScale`. |
| `UICanvas::SetTextScale(scale)` / `TextScale()` | The canvas text scale; setting it re-applies the type scale. |
| `UICanvas::Problems()` | The UI rules' findings on the shown elements (overflow, clipped or shrunk text, missing glyphs, overlapping buttons). |
| `UICanvas::MissingGlyphs(element, text)` | The characters of `text` the element's font has no glyph for. |
| `UIElement::TextHeightAt(width)` | `FitHeight`'s text height at a width, padding included (< 0 without text). |
| `UIElement::GetOrCreateCustomStyle()` | Returns this element's own style override, creating it on first call. |
| `UIElement::TypeName()` | Returns the element's type name, e.g. `"Button"`. |
| `UIElement::Remove()` | Removes the element and its whole subtree; no-op once retired. |
| `UIElement::ClearChildren()` | Removes all descendants and keeps the element. |
| `UIElement::BringToFront()` | Moves the element last among its siblings (drawn on top). |
| `UIElement::SendToBack()` | Moves the element first among its siblings. |
| `UIElement::MoveTo(x, y, duration, ease = "linear")` | Tweens anchored position and returns the handle. |
| `UIElement::TranslateTo(x, y, duration, ease = "linear")` | Tweens the visual translation (no re-layout) and returns the handle. |
| `UIElement::SizeTo(w, h, duration, ease = "linear")` | Tweens size delta and returns the handle. |
| `UIElement::ScaleTo(sx, sy, duration, ease = "linear")` | Tweens scale and returns the handle. |
| `UIElement::RotateTo(degrees, duration, ease = "linear")` | Tweens rotation and returns the handle. |
| `UIElement::FadeTo(opacity, duration, ease = "linear")` | Tweens opacity and returns the handle. |
| `UIElement::ColorTo(color, duration, ease = "linear")` | Tweens the background colour and returns the handle. |
| `UIElement::TextColorTo(color, duration, ease = "linear")` | Tweens the text colour and returns the handle. |
| `UIElement::DrawContent(style, opacity)` | Virtual paint hook every widget overrides; the base draws nothing. |
| `UIPanel::DrawContent(style, opacity)` | Draws nothing; the canvas paints the panel's shadow, background and border. |
| `UIText::Fit()` | Returns this element's wrap/shrink settings as a `UITextFit`. |
| `UIText::EnsureFitted()` | Recomputes the text memo when any of its inputs changed. |
| `UIText::FittedFontSize()` | Font size the fitter actually used. |
| `UIText::MeasuredWidth()` | Width of the laid-out text in logical units. |
| `UIText::MeasuredHeight()` | Height of the laid-out text in logical units. |
| `UIText::DrawContent(style, opacity)` | Draws the fitted, localized text inside the box. |
| `UIImage::DrawContent(style, opacity)` | Draws the texture with `Tint`, `SourceRect` and aspect options. |
| `UIButton::Fit()` | Returns the label's wrap/shrink settings as a `UITextFit`. |
| `UIButton::HitRect()` | Pointer rect: `ResolvedRect` grown about its centre by `HitScale` (per axis). |
| `UIButton::EnsureFitted()` | Recomputes the label memo when any of its inputs changed. |
| `UIButton::FittedFontSize()` | Font size the fitter actually used for the label. |
| `UIButton::MeasuredWidth()` | Width of the laid-out label in logical units. |
| `UIButton::MeasuredHeight()` | Height of the laid-out label in logical units. |
| `UIButton::DrawContent(style, opacity)` | Draws the icon and/or fitted label over the button surface. |
| `UIProgressBar::DrawContent(style, opacity)` | Draws the fill up to `Value` in `FillDirection`; the trough is the element background. |
| `UISlider::DrawContent(style, opacity)` | Draws the fill and knob at `Value`; the trough is the element background. |
| `UIScrollView::GetThumbRect()` | Returns the scroll indicator rect in logical space. |
| `UIScrollView::DrawContent(style, opacity)` | Draws the scroll indicator; the canvas clips the children and layout applies `ScrollOffset`. |
| `UITextInput::Fit()` | Returns a non-wrapping fit with shrink and an 8-unit floor. |
| `UITextInput::DrawContent(style, opacity)` | Draws the text or dimmed placeholder and the blinking caret. |
| `UICanvas::Update(dt)` | Per frame: reclaims retired subtrees, clears transient flags, lays out, hit-tests, navigates focus, then advances transitions, tweens and sequences. |
| `UICanvas::Drawing()` | Draws the element tree in the HUD pass, outside the world camera. |
| `UICanvas::ScaleFactor()` | Logical-to-screen scale from the reference size and the window. |
| `UICanvas::LogicalWidth()` | Canvas width in logical units. |
| `UICanvas::LogicalHeight()` | Canvas height in logical units. |
| `UICanvas::Root()` | Returns the root element. |
| `UICanvas::Add(type, name, parent)` | Creates an element of `type` under `parent` (the root when null); nullptr when the parent is retired. |
| `UICanvas::Find(name)` | Returns a live element by name, or nullptr. |
| `UICanvas::FindById(id)` | Returns a live element by id, or nullptr. |
| `UICanvas::ClearUI()` | Removes every element and kills all tweens, sequences, focus and hover. |
| `UICanvas::SetTheme(theme)` | Copies `theme` as the canvas's own theme. |
| `UICanvas::Theme()` | Returns the canvas's theme (the const overload is read-only). |
| `UICanvas::GetFocused()` | Returns the focused element, or nullptr. |
| `UICanvas::GetHovered()` | Returns the element under the pointer, kept while pressed. |
| `UICanvas::PointerOverUI()` | True when this canvas would take the mouse at its current position. |
| `UICanvas::PointerOverUIAt(logicalPos)` | Same question at a logical canvas point. |
| `UICanvas::PickElement(logicalPos, outHoveredScroll=nullptr)` | The element that would get the pointer at a logical point (null: none); the agent channel uses it to refuse an occluded click. |
| `UICanvas::SetFocused(element)` | Focuses `element`; a retired element clears focus. |
| `UICanvas::AddPanel(name = "", parent = nullptr)` | Creates a panel, under the root when `parent` is null. |
| `UICanvas::AddText(name = "", parent = nullptr)` | Creates a text element. |
| `UICanvas::AddImage(name = "", parent = nullptr)` | Creates an image element. |
| `UICanvas::AddButton(name = "", parent = nullptr)` | Creates a button. |
| `UICanvas::AddProgressBar(name = "", parent = nullptr)` | Creates a progress bar. |
| `UICanvas::AddSlider(name = "", parent = nullptr)` | Creates a slider. |
| `UICanvas::AddScrollView(name = "", parent = nullptr)` | Creates a scroll view. |
| `UICanvas::AddTextInput(name = "", parent = nullptr, placeholder = "", allowLetters = true, allowDigits = true, allowSymbols = true)` | Creates a text input with its filter flags. |
| `UICanvas::StartTween(targetElementId, prop, toValues, count, duration, ease)` | Tweens `prop` from its current value; returns the tween id. |
| `UICanvas::KillTweensFor(element, includeDescendants)` | Kills tweens and sequences touching the element (and optionally its descendants). |
| `UICanvas::KillAllTweens()` | Kills and drops every tween. |
| `UICanvas::UpdateTweens(dt)` | Advances tweens with `dt` clamped to 0.1 s. |
| `UICanvas::IsTweenActive(id)` | True while the tween is alive, unpaused and not completed. |
| `UICanvas::IsTweenCompleted(id)` | True once the tween finished its loops. |
| `UICanvas::GetTweenProgress(id)` | Elapsed fraction 0..1; 1 when completed or unknown. |
| `UICanvas::SetTweenDelay(id, delay)` | Sets the delay before the tween runs (clamped to >= 0). |
| `UICanvas::GetTweenDelay(id)` | Remaining delay in seconds. |
| `UICanvas::SetTweenLoops(id, loops)` | Loops: 0 = once, positive = count, negative = forever. |
| `UICanvas::GetTweenLoops(id)` | Returns the tween's loop setting. |
| `UICanvas::SetTweenYoyo(id, yoyo)` | Makes alternate loops play backwards. |
| `UICanvas::GetTweenYoyo(id)` | Returns the tween's yoyo flag. |
| `UICanvas::KillTween(id)` | Marks one tween dead. |
| `UICanvas::PauseTween(id)` | Freezes the tween until resumed. |
| `UICanvas::ResumeTween(id)` | Continues a paused tween. |
| `UICanvas::Sequence()` | Creates a sequence and returns a handle to it. |
| `UICanvas::CreateSequence()` | Creates a sequence and returns its id. |
| `UICanvas::UpdateSequences(dt)` | Advances playing sequences and spawns their due steps. |
| `UICanvas::KillSequence(seqId)` | Kills one sequence and its spawned tweens. |
| `UICanvas::KillAllSequences()` | Kills every sequence. |
| `UICanvas::IsSequenceActive(seqId)` | True while the sequence is playing. |
| `UICanvas::IsSequenceCompleted(seqId)` | True once the sequence has finished. |
| `UICanvas::PlaySequence(seqId)` | Starts the sequence from time 0. |
| `UICanvas::AddSequenceStep(seqId, step, isJoin)` | Queues a step after the queue, or at the previous step's start when `isJoin`. |
| `UICanvas::AddSequenceInterval(seqId, seconds)` | Adds dead time to the sequence queue. |
| `UICanvas::NotifySubtreeRemoving(subtree)` | Kills tweens and clears focus, hover, capture and names for a removed subtree. |
| `UICanvas::RetireSubtree(subtree)` | Parks a detached subtree, marked dead, until nothing outside references it. |
| `UICanvas::ReclaimRetired()` | Frees retired subtrees that no outside wrapper still references. |
| `UICanvas::AnyElementReferenced()` | True while any live or retired element of the canvas is still wrapped. |
| `UICanvas::RetiredCount()` | Number of subtrees waiting for reclamation. |
| `UICanvas::AllocateId()` | Returns the next element id. |
| `UICanvas::UniqueName(requestedName, type)` | Returns an unused name, suffixing `_2`, `_3`, … when taken. |
| `UICanvas::ReleaseName(name)` | Releases a name so a later element can reuse it. |

### Engine libraries' runtime C++ (`libraries/<lib>/src`, built into `IYIEngineCore`)

Include as `"<lib>/src/X.h"`; each library's Python module wraps the same C++ (`src/LibraryBindings.cpp`, `src/GeoWorldBindings.cpp`).

| Function | What it does |
|---|---|
| `Daylight::Elevation`, `SunAt`, `Ambient`, `Lamps`, `AmbientColour`, `ShadowTint`, `Mist`, `Haze`, `GradeAt` (`sprite_stack/src/Daylight.h`) | Time of day (0..24) -> sun, ambient, lamps, shadow tint, mist, haze, colour grade. |
| `Fx(lights, FxOptions)`, `Fx::Lighting`, `Fx::SetGroundDetail`, `Fx::SetMarks`, `Fx::LightSprites`, `Fx::At` (`sprite_stack/src/Fx.h`) | The lit look: shaders, the `StackLightingHandle`, ground detail, incremental marks, light sprites, a spot in an object's space. |
| `Post()`, `Post::Apply(PostLook)`, `Post::Unload()` (`sprite_stack/src/Post.h`) | The world-only postprocess (grade, mist, tilt-shift, rain, heat, vignette, grain). |
| `Select::Drag`, `Project`, `Pick`, `InBox`, `Apply`, `Group` (`rts_select/src/Select.h`) | RTS click/box selection, additive selection and control groups over a `Select::State`. |
| `GeoWorld(name, worldJson, heights, grid)`, `GeoWorld::LoadDirectory(dir)` (`geo_world/src/World.h`) | A `geo_world` package in C++ (throws `std::invalid_argument` on a bad one). |
| `GeoWorld::Height`, `Flags`, `BuildingAt`, `BuildingsNear`, `BuildingById`, `RoadById`, `ToLonLat`, `FromLonLat` | World queries: ground height, grid bits, buildings, roads, WGS84. |
| `GeoWorld::SetCondition`, `ConditionOf`, `RoadOpen`, `SetDamage`, `Damage`, `ChangesSince`, `Blast`, `SetTrees` | Building/road conditions (grid bits updated), damage, tree blasts with a scorch. |
| `GeoWorld::MakeMark`, `AddMark`, `SetMarks`, `SaveState`, `LoadState` | Ground marks and save-game state (`LoadState` returns unknown ids). |
| `GeoWorld::BlockerHeights`, `Foliage`, `MakeSightGrid`, `UpdateSight`, `TerrainCorners` | Sight grid and terrain inputs built from the world. |
| `ProfilerOverlay::Rows`, `Summary`, `Split`, `Update`, `Draw`, `ProfilerOverlay::Actor` (`profiler_overlay/src/Overlay.h`) | The F3 profiler table and its native actor. |
| `Api::NetHost(NetHostOptions)`, `NetJoin(target, playerName)`, `NetFind(transport)`, `NetLeave()`, `NetSimulate(latencyMs, jitterMs, loss)` (`net/src/Net.h`, built with `IYI_NET`) | engine.net session: host (enet/steam/loopback), join an address or a found lobby, LAN/Steam search, leave, simulated bad network. |
| `Api::NetState`, `NetIsHost`, `NetLocalPlayer`, `NetPlayers`, `NetTick`, `NetGetStats`, `NetSetLobbyData`, `NetEvents` | Session state, roster with rtt/loss, host tick, traffic stats, lobby data, events since the last call. |
| `Api::NetDefine`, `NetKindField`, `NetSpawn`, `NetObjects`, `NetExists`, `NetObjectKind`, `NetObjectOwner`, `NetObjectOwnerAuthority`, `NetCanWrite` | Replicated kinds and objects: define fields, spawn (host), list, inspect. |
| `Api::NetSet`, `NetGet`, `NetSample`, `NetSetPriority`, `NetSetVisible`, `NetDespawn` | Write (authority only, clamped), latest value, interpolated value, send priority, interest, despawn. |
| `Api::NetDefineMessage`, `NetMessageField`, `NetSend`, `NetReceive` | Messages (RPC) to `kNetHost`, `kNetAll`, `kNetOthers` or a player. |
| `Api::NetDefineInput`, `NetInputField`, `NetSendInput`, `NetInputs`, `NetInputAck`, `NetRewind` | Client inputs (seq, resent until acked), host's queue per player, last acked seq, lag-compensation rewind (1 s). |

### IYIUITween.h

| Function | What it does |
|---|---|
| `ParseEase(name)` | Turns an ease name such as `"out_cubic"` into a `UIEase`; unknown names give `Linear` and one warning. |
| `EvaluateEase(ease, t)` | Evaluates an ease curve at `t` in 0..1. |
| `UITweenHandle::Active()` | True while the tween is alive, unpaused and not completed. |
| `UITweenHandle::Completed()` | True once the tween finished its loops. |
| `UITweenHandle::Progress()` | Elapsed fraction 0..1; 1 when the tween is gone. |
| `UITweenHandle::Delay(seconds)` | Sets the delay and returns the handle for chaining. |
| `UITweenHandle::GetDelay()` | Remaining delay in seconds. |
| `UITweenHandle::Loops(count)` | Sets loops (0 = once, negative = forever) and returns the handle. |
| `UITweenHandle::GetLoops()` | Returns the loop setting. |
| `UITweenHandle::Yoyo(enable = true)` | Makes alternate loops play backwards; returns the handle. |
| `UITweenHandle::GetYoyo()` | Returns the yoyo flag. |
| `UITweenHandle::Kill()` | Kills the tween. |
| `UITweenHandle::Pause()` | Freezes the tween until resumed. |
| `UITweenHandle::Resume()` | Continues a paused tween. |
| `UISequenceHandle::Active()` | True while the sequence is playing. |
| `UISequenceHandle::Completed()` | True once the sequence has finished. |
| `UISequenceHandle::AppendMove(element, x, y, duration, ease = "linear")` | Queues a move after everything already queued; returns the handle. |
| `UISequenceHandle::JoinMove(element, x, y, duration, ease = "linear")` | Starts a move with the previous step. |
| `UISequenceHandle::AppendSize(element, w, h, duration, ease = "linear")` | Queues a size change after the queue; returns the handle. |
| `UISequenceHandle::JoinSize(element, w, h, duration, ease = "linear")` | Starts a size change with the previous step. |
| `UISequenceHandle::AppendScale(element, sx, sy, duration, ease = "linear")` | Queues a scale change after the queue; returns the handle. |
| `UISequenceHandle::JoinScale(element, sx, sy, duration, ease = "linear")` | Starts a scale change with the previous step. |
| `UISequenceHandle::AppendRotate(element, degrees, duration, ease = "linear")` | Queues a rotation after the queue; returns the handle. |
| `UISequenceHandle::JoinRotate(element, degrees, duration, ease = "linear")` | Starts a rotation with the previous step. |
| `UISequenceHandle::AppendFade(element, opacity, duration, ease = "linear")` | Queues a fade after the queue; returns the handle. |
| `UISequenceHandle::JoinFade(element, opacity, duration, ease = "linear")` | Starts a fade with the previous step. |
| `UISequenceHandle::AppendColor(element, color, duration, ease = "linear")` | Queues a background colour tween after the queue; returns the handle. |
| `UISequenceHandle::JoinColor(element, color, duration, ease = "linear")` | Starts a background colour tween with the previous step. |
| `UISequenceHandle::AppendTextColor(element, color, duration, ease = "linear")` | Queues a text colour tween after the queue; returns the handle. |
| `UISequenceHandle::JoinTextColor(element, color, duration, ease = "linear")` | Starts a text colour tween with the previous step. |
| `UISequenceHandle::AppendInterval(seconds)` | Adds dead time to the queue; returns the handle. |
| `UISequenceHandle::Play()` | Starts the sequence from time 0. |
| `UISequenceHandle::Kill()` | Kills the sequence and its tweens. |
