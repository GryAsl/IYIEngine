# Engine hard rules (full text)

The engine's invariants. The numbers are stable
("hard rule 28" always means rule 28), and code comments cite them by number. Read this
file before editing the area a rule covers.

1. **Never cache a Python lifecycle function across frames.** Hooks are fetched
   from the module dict on every call. This is the mechanism that makes hot reload
   work. Caching them "for performance" breaks the engine's core feature, and the
   symptom is silent: edits simply stop taking effect.
2. **Public headers stay clean.** No `Python.h`, no `pybind11/*`, no
   `nlohmann/json.hpp`, no `rlgl.h` under `include/IYI/`. `PythonRuntime` uses a pimpl for
   exactly this reason. Verify with a grep before you claim you are done.
3. **Never erase actors while iterating.** `Destroy()` sets a flag;
   `RemoveDestroyed()` runs once per frame. Loops go by index against a size
   snapshot taken before the pass.
4. **Do not change shutdown order.** Actors → Python finalize, or a native game's `Game` object (both on the game thread, which then
   ends) → the GL deletions they recorded → raylib resources → audio device → window → Steam
   (`SteamAPI_Shutdown`). Anything else frees resources that are still referenced.
5. **Use raylib directly.** If raylib solves it, bind it thinly. Do not build a
   renderer, a collision system, or an input layer on top of it. The frame packet (a recorded
   raylib call is still a raylib call) and the frame input copy are the threading boundary, not
   such layers: keep them one-to-one with raylib.
6. **No managers, services, factories, DI, singletons-with-lazy-init, template
   metaprogramming, or deep inheritance.** This is not a style preference, it is
   the project's stated requirement. If a small function solves it, write that.
7. **One way to do each thing** in the Python API. Do not add a second spelling of
   an existing operation.
8. **The engine contains no game-specific code**, ever. It must stay reusable for
   unrelated 2D games.
9. **The engine draws nothing of its own.** No FPS counter, no debug overlay.
10. **No new dependencies** beyond raylib, pybind11 and nlohmann/json without an
    explicit decision.
11. **Never store Python callbacks in C++ tweens or sequences.** Use Python-side
    polling or sequence chaining instead.
12. **Element removal must kill attached tweens and sequences.** Canvas owns cleanup.
13. **Scissor stack must remain strictly balanced per frame.** Every `BeginScissorMode`
    must have an unconditional matching `EndScissorMode`.
14. **All pybind11 code stays in `src/*Bindings.cpp`.** No pybind in `IYIUI.h` or
    `IYIUITween.h`.
15. **UI text is only ever drawn through `DrawTextFitted`.** It is the single
    chokepoint that guarantees text cannot leave its box: wrap, bounded
    binary-search shrink, ellipsis, then a scissor clip (skipped only when the fit
    measured the text inside its box). A new widget that calls
    `DrawTextEx` / `DrawTextPro` / `DrawText` directly reintroduces the overflow
    bug, and it will look fine until someone resizes a window.
    The `engine.draw_text` / `draw_text_ex` forwards in `RaylibBindings.cpp` are
    **not** covered by this rule and must stay uncovered: they are the game's own
    world-space drawing, deliberately unclipped. Do not "make them consistent".
16. **Clip rectangles are pushed through `PushScissor`, never `BeginScissorMode`
    directly.** `BeginScissorMode` is a screen-space GPU scissor that ignores the
    rlgl model-view matrix, while every element draws inside one. `PushScissor`
    is what reconciles the two, by transforming the rect with the accumulated
    `s_currentTransform` and taking its axis-aligned bounding box. Bypass it and
    anything tweened, scaled by a hover/pressed style, or rotated will be sliced
    at the edges. `s_currentTransform` must be composed and restored in exact
    symmetry with `rlPushMatrix` / `rlPopMatrix`, or siblings inherit each other's
    transform.
17. **The console reload line is emitted ONLY by `NoteReloaded`.** It is the single
    chokepoint that guarantees printing and recording in `m_reloadedScripts` are
    inseparable. Printing a reload directly without recording it starves live actors
    of their `Reload` hook: helper modules and the entry script will refresh in Python
    while actors are never notified, leaving retained UI and gameplay state silently
    stale.
18. **All engine output goes through `LogWrite`, never raw `std::cout` or `std::cerr`.**
    The log file is the record an agent reads and anything bypassing it is invisible
    there. The single accepted exception is the CLI usage text (`PrintUsage` in `src/Application.cpp`),
    which prints before the log exists.
19. **Layout cache signatures cover every input read by layout resolution.** When
    changing `ResolveElementLayout`, `ResolveElementContent`, or `ApplyContainerLayout`,
    update the signature too; transforms and opacity are still resolved every frame. A hidden
    element's subtree is left out of it (its rects may be stale while hidden; SPEC-IYIUI §20).
20. **Postprocess geometry and UI hit testing must share one warp coefficient.** Map
   the pointer at `UICanvas::Update` before all hit tests when a shader moves pixels.
21. **Filesystem paths keep their case; only archive keys are lowercased.** Open files with
    `ResourceFsPath`, hash packaged IDs with `ResourceKey`
    (`ResourceKey`); both live in `include/IYI/ResourcePath.h`, shared with `tools/iyipack`.
22. **A removed `UIElement` is retired, and freed only by `UICanvas::ReclaimRetired` once no Python
    wrapper references any node of its subtree** (hook set from the bindings; never free on remove).
    Every `UIElement` mutator must check `Alive`, and `Find`/`FindById` walk only the live tree.
23. **Fixed-step clocks share one clamped delta** (`ClampFrameDelta`, `FixedStep.h`); never clamp per clock.
24. **Game-side texture mode goes only through `Application::BeginUserTextureMode` / `EndUserTextureMode`**,
    which refuse it during the draw pass and close a forgotten one before drawing; a raw
    `BeginTextureMode` elsewhere drops the camera transform and the postprocess target. The one exemption is
    `StackLighting.cpp`, whose passes replay on the render side, where the chokepoint only records.
25. **Sprite stacks draw only through `DrawSpriteStacks`** (`SpriteStack.cpp`): one quad per layer in
    raylib's batch, nothing per object that would flush it (no uniforms, no shader switches). Per-object
    data a shader needs travels in vertex attributes, as the ground point does in the normal. Culling lives there too (`SpriteStackOffTarget`, always on): games pass whole lists, never cull stacks in Python.
26. **Library roots resolve like the project, everywhere**: resources (`DevResourceBackend::Locate`),
    `sys.path`, hot reload (`ScriptFiles`) and packaging (`release.py content_files`) all put the project
    first and libraries after it in listed order. Change one and you change all four.
27. **IME commits come only from raylib's character queue**: forward all native messages, preserve the composition frame guard, and cancel ownership through `ImeCancelOwner` before changing or removing focus.
28. **A game's native module is project-scoped** (`<project>/native/`, SPEC §2b): a plain C++ core with
    no `Python.h` and a thin pybind11 `bindings.cpp`; only plain data (numbers, strings, tuples,
    lists, `bytes`, float buffers) crosses the Python boundary, never an engine object; the core may
    include `IYI/*.h` headers of `IYIEngineSim` and link it (built with `cmake/iyi_native.cmake` and
    `tools/build_native.bat`); `IYIEngineSim` stays free of Python, window, GPU, audio and actors; no
    hot reload (a replaced module needs a restart); and the engine never builds, contains or links a
    game's module (its `CMakeLists.txt` must not depend on a project's `native/`).
29. **Game-side code never calls GL, GLFW, raylib's live input or the AudioManager** (SPEC §8a). Python
    and every hook run on the game thread; a binding or IYIUI records the call (`Record`, `RecordSetup`
    for window requests and uploads, `RecordAudio`) and reads input and window state from
    `GameInput()`. A recorded closure captures values or AssetManager cache pointers, never game
    state that can change before the replay, and render-side code (a replayed pass) never records.
30. **The render side never runs Python**, and the main thread owns the window: every GLFW call and
    `PollInputEvents` stay there. Objects with GL resources that both sides reference are shared
    and deleted on the render side (`RenderOwned`).
31. **A removed actor Python still wraps is parked, never freed** (`ActorRegistry` `s_retired`, freed once no
    wrapper is left), and every bound actor type calls `GuardDeadActorAccess` so a dead one raises `ReferenceError`.
