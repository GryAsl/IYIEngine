# Automated checks: which one when

One page. Details are in each tool's own doc. Every tool runs isolated (own user data, logs and saves,
no Steam, no focus) and none runs in a shipping build.

| Question | Run | Time | Doc |
|---|---|---|---|
| Does the engine still start? | `build.bat`, then `IYIEngine.exe --project <game> --frames 120` and look for `IYI_RUNTIME_READY` | 1 min | `docs/spec/cli.md` |
| Engine pure logic (paths, tweens, UTF-8, profiler, UI pointer) | `ctest --test-dir build/ninja --output-on-failure` (no ctest on PATH: `build/ninja/IYIEngineTests.exe`) | seconds | `docs/TESTING.md` |
| Did I break a shader? | `IYIEngine.exe --project <game> --check-shaders` | seconds | `docs/spec/cli.md` |
| HR Python logic | `verify-affected.bat` (changed files), `verify-fast.bat` (before a commit), `verify-full.bat` | 1 to 5 min | HR `README.md` Verification |
| What does the game show right now? | `python tools/agent_probe.py run --project <game> --request a.json` | seconds | `spec/cli.md` §3.1 |
| Did a screen change or its UI break? | `python tools/visual_test.py run --project <game>` | minutes | `VISUAL-TESTS.md` |
| Does a bot playing many days hit a bug? Is the balance off? | `python tools/playtest.py run` / `study` | seconds to minutes | `PLAYTEST.md` |
| A bug that happened once: make it repeat, find the cause | `playtest.py verify`, `debug`, `shrink` on its bundle; `video` records it | seconds (`video`: real time) | `PLAYTEST.md` |
| Does a real-click flow work (new run, harvest, sell)? | `python tools/playtest.py ui run --scenario <file>` | seconds | `PLAYTEST.md` |
| Is it slower? | the game's benchmark (one at a time, never in parallel) | minutes | `benchmark.md` |

Rules of thumb:

- Prove with numbers (agent JSON, self-checks, study reports) first; a screenshot only as the last check.
- Every isolated run (agent_probe, visual tests, UI playtests, trailer takes) is a child of its tool and quits
  within half a second of that tool ending, even a hard kill; a lockstep run also quits after 10 minutes
  with no request. A game still running long after its tool is a bug: report it.
- Visual tests answer "does it look right"; playtest answers "does it play right". A UI scenario in
  playtest asserts state after real clicks; a visual test compares pixels with an approved baseline.
- Small playtest studies (`devtools/studies/smoke.json`, about 15 s) are for every change to game rules.
  Large ones (`full.json`, 1400 runs) only when Gry asks.
- Self-checks of the tools themselves: `python tools/playtest.py selfcheck`, `python tools/test_visual_test.py`.
