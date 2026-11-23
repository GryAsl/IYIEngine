# IYI Engine contract: Logging

Part of the engine contract; index and rules in [../SPEC.md](../SPEC.md). Section numbers are the contract's own.

## 17. Logging system

Centralized engine logging core (`include/IYI/Log.h`, `src/Log.cpp`). All engine
subsystems route diagnostics through `LogWrite` to guarantee column-aligned console
output and a synchronized disk log.

Governing rule: **an event is never logged without its effect.** Announcing an action
(such as recompiling a script) without recording its actual effect on runtime state
(such as how many live actors were notified) masks silent failures.

### 17.1 Line format and level set

Every log line follows a strict column-aligned format:

```text
[HH:MM:SS.mmm] [LEVEL] [category] message
```

- Timestamp: local time with millisecond precision (`HH:MM:SS.mmm`).
- Level: fixed 5-character string: `DEBUG`, `INFO `, `WARN `, `ERROR`.
- Level set:
  - `LogLevel::Debug` (`DEBUG`): detailed internal traces; console dispatch to `std::cout`.
  - `LogLevel::Info` (`INFO `): normal operational milestones; console dispatch to `std::cout`.
  - `LogLevel::Warn` (`WARN `): recoverable anomalies and warnings; console dispatch to `std::cerr`.
  - `LogLevel::Error` (`ERROR`): failures, syntax errors, uncaught exceptions; console dispatch to `std::cerr`.
- Filtering: all log levels are unconditionally recorded to `latest.log`. Console output
  is filtered by minimum level (default: `Info`).
- Multiline messages: messages containing newlines are split per line, each receiving
  an identical timestamp/level/category prefix.

### 17.2 Category set

- `startup`: initialization parameters, executable path, project path resolution, log creation.
- `shutdown`: clean subsystem teardown and session completion summary.
- `python`: script loading, syntax errors, exception tracebacks, stdio redirection.
- `reload`: hot reload announcements, unreachable script warnings, reload dispatch effect.
- `assets`: texture/font/shader/sound/music loading, path resolution failures.
- `ui`: UI canvas, widget layout, styling, and icon fallback warnings.
- `game`: gameplay messages emitted from Python scripts via `engine.log_*`.
- `render`: refused or unbalanced game-side render state (`begin_texture_mode`).

### 17.3 Log file location and rotation policy

- Location: `<project-dir>/logs/latest.log`.
- On engine startup:
  - If `<project-dir>/logs/` does not exist, it is created.
  - If an existing `latest.log` is 0 bytes (empty), it is deleted.
  - If an existing `latest.log` is non-empty, it is renamed to
    `run-YYYYMMDD-HHMMSS.log` based on its last modification timestamp (with
    `-1.log`, `-2.log` collision suffixes if needed).
  - Rotation file cap: the engine scans `<project-dir>/logs/` and counts **only**
    `latest.log` and files matching `run-*.log` (`run-` prefix and `.log` extension).
    Any other files present in `logs/` are ignored. Entries are sorted by write time
    ascending; while the count exceeds 4, the oldest archives are deleted so that
    adding the new `latest.log` maintains a strict maximum of 5 log files.
  - A fresh `latest.log` is opened with truncation. The first line records the
    engine executable, resolved project directory, and startup datetime.

### 17.4 Command-line `--log-level` flag

CLI argument: `--log-level <debug|info|warn|error>` (case-insensitive).
Sets the minimum severity threshold printed to the console (stdout/stderr). If
omitted or invalid, defaults to `info`. Does not filter disk writes to `latest.log`.

### 17.5 Python API

Exposed in the embedded `engine` module:

```python
engine.log_debug(message: str, category: str = "game") -> None
engine.log_info(message: str, category: str = "game") -> None
engine.log_warn(message: str, category: str = "game") -> None
engine.log_error(message: str, category: str = "game") -> None
engine.log_event(kind: str, **fields) -> None   # one line of logs/events.jsonl
```

All functions default `category` to `"game"`. Every scene change is also logged as an Info line in
category `scene` (the scene script's path).

**Structured events.** `logs/events.jsonl` (truncated at start, next to `latest.log`) holds one JSON
object per line, `{"t": "HH:MM:SS.mmm", "frame": n, "kind": ..., ...fields}`. The engine writes
`scene` (`path`), `ui_click` (`element`; `by: "keyboard"` for a focused button activated by a key),
`warning` and `error` (`category`, `message`: every Warn and Error log line) and `quit` (`frames`); a
game adds its own with `engine.log_event` (HR: `save`, `load`). Fields must be JSON-serializable (others
become their `repr`); `t`, `frame` and `kind` are the line's own keys, so a field with one of those names
raises `ValueError`. Every string is written as valid UTF-8 (a stray byte becomes U+FFFD), so every line
parses. The last 500 are kept in memory for the agent channel's `events`.

### 17.6 Python print capture

During `PythonRuntime::Initialize`, `sys.stdout` and `sys.stderr` are replaced with
internal `_StreamCapture` objects:
- `sys.stdout.write(...)` buffers lines and routes completed lines to `engine.log_info(line, "python")`.
- `sys.stderr.write(...)` buffers lines and routes completed lines to `engine.log_error(line, "python")`.
On runtime shutdown (`PythonRuntime::Shutdown`), captured streams are flushed and
original `sys.stdout` and `sys.stderr` references are restored.

### 17.7 Silent-failure detectors

Five specialized detectors protect against silent failures across the runtime:

1. **Unimplemented lifecycle hooks (`src/PythonRuntime.cpp:211`)**: when a script
   is loaded, warns at `LogLevel::Warn` if it defines none of the supported lifecycle
   hooks (`Start`, `Update`, `FixedUpdate30`, `FixedUpdate60`, `Drawing`, `Reload`,
   `GameStart`, `SceneStart`), catching misspelled hook names immediately.
2. **Missing actor script on disk (`src/PythonRuntime.cpp:327`)**: warns once at
   `LogLevel::Warn` when an actor references a script path that does not exist on
   disk, rather than failing silently on every frame.
3. **Unreachable / unimported script modification (`src/PythonRuntime.cpp:463-466`)**:
   warns at `LogLevel::Warn` when a changed `.py` file under `scripts/` is not
   tracked in `m_modules` or `sys.modules`, informing the user that the edit will
   not take effect until imported.
4. **Suppressed error summary (`src/PythonRuntime.cpp:677-683`)**: recurring hook
   errors are logged on first failure and suppressed thereafter to avoid log spam;
   at Python shutdown, warns with the count of additional suppressed failures per
   `(script, hook)` pair.
5. **Session shutdown summary (`src/Application.cpp:338-343`)**: at application
   exit, queries `LogWarnCount()` and `LogErrorCount()`. If zero, logs
   `[shutdown] run complete: 0 warnings, 0 errors` at `Info`; if non-zero, logs
   `[shutdown] run complete: X warning(s), Y error(s)` at `Warn`.
