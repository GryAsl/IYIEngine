# Playtesting, deterministic replay and the Balance Lab

One toolchain for automated play, reproducible bugs and balance probes. It merges two plans
(`IYIENGINE_AI_PLAYTESTING_DETERMINISTIC_REPLAY_PLAN.md` and `IYIEngine_AI_Playtester_and_Balance_Lab_Implementation.md`,
2026-11-06). Development only: nothing here runs in a player's game.

| Part | Where | What |
|---|---|---|
| Replay core | `libraries/playtest/scripts/playtest/core.py` | record, canonical hash, first diff, bundle files, replay, verify, seek, compare, fork, shrink |
| Debugger | `.../debugger.py` | rewind = nearest checkpoint + forward replay; commands shared by the CLI and a game's panel |
| Balance Lab | `.../lab.py` | study matrix, one process per run, distributions, Wilson intervals, alerts, CSV/JSON/HTML/MD |
| UI playtester | `.../ui.py` | scenario steps through the agent channel (SPEC §3.1) in an isolated lockstep game, asserts, trace, replay |
| Failure video | `.../video.py`, `tools/playtest.py video` | a bundle replayed in the real game and recorded by IYITrailer |
| LLM sample | `.../llm.py` | optional Claude policy; never needed for replay |
| CLI | `tools/playtest.py` | `run`, `study`/`batch`, `verify`, `debug`, `shrink`, `compare`, `fork`, `video`, `ui run/replay`, `selfcheck` |
| Agent channel input | `src/AgentChannel.cpp` | `scroll`, `drag`, `key` holds and modifiers, refused clicks (hidden, disabled, offscreen, occluded, ambiguous), `scope`, `protocol: 2` |
| HR adapter | `HumanResources/scripts/game/playtest_adapter.py`, `devtools/` | real commands, observation, oracles, 16 policies, scenarios, studies (Lab hooks in `devtools/adapter.py`), economy balancer |

Design decisions (Gry, 2026-11-06): the deterministic core is Python, not new C++ clock flags; the game's
real fixed step is called directly headless, and in-engine runs use the existing `--fixed-dt --seed
--unthrottled` lockstep. Human play is not recorded through a command dispatcher (UI tests use real
clicks). CLI debugger plus an in-game dev panel. LLM: interface plus a Claude sample.

## Tick order and determinism

At tick N: labelled fault (if any), then the policy's commands in `seq` order, then the game's fixed
step, then oracles and invariants. "State at tick N" is the state after N steps. A bundle replays
from its initial snapshot (or a checkpoint) with the same order. Target: same engine and game build,
same OS/Python, same initial state and commands give the same hashes. Cross-platform bit-identical
floats are not promised; a bundle recorded on another build replays with a `build_warning`.

The canonical view (hash input) is the adapter's `canonical(state)`: sorted-key JSON, floats as
`repr`, RNG integers exact. HR drops only the rendered log, the UI-only job counters, the save mark,
the run id, the wall-clock autosave timer and the debug-taint labels; every economic and RNG value stays.

## Adapter (one module per game)

`<project>/devtools/adapter.py` (or `--adapter`). It sets up headless running for its game and exposes:

```text
TICK_HZ, SCHEMA, GAME
create(scenario, seed) -> state        snapshot(state) / restore(snap)
tick(state) -> [violations]            one real fixed step (+ tick oracles)
actions(state) -> [command]            the legal commands now
execute(state, command) -> (ok, reason[, violations])   through the same functions the UI calls
observe(state) -> dict                 bounded, what a player can see
canonical(state) -> dict               hash view (document every omission)
check(state) -> [{"id", "detail"}]     invariants
metrics(state) -> dict                 per-day balance numbers;  day(state);  progress(state)
finished(state) -> end reason | None   POLICIES {name: choose(obs, legal, rng, memory) -> [command]}
optional: inject(state, spec), features(state) -> set, rng_streams(state)
optional Lab hooks (game-specific study output; generic fallbacks without them):
  FIXTURE (default fixture: run ids and rows), run_row(result), COHORT_KEYS, cohort_stats(rows),
  ALERTS + alerts(rows, groups, agg, thresholds, add), EXPLOITS, REPORT_COLUMNS, SERIES
```

Generic oracles in the core: a rejected command must not change the canonical state
(`rejected_mutation`), exceptions, invariants, soft lock (`progress` flat for `stall_days` while a
policy plays; a policy marked `expects_stall` is exempt), wall-time timeout, replay divergence.
HR adds: cash moved without a matching finance-ledger entry (`cash_without_ledger`), a sale's stock
and cash agree (`sale_stock`, `sale_cash`), the clock does not move at speed 0 (`paused_progress`),
plus every `game.invariants.violations`.

## Bundles

`<project>/artifacts/playtest/<stamp>-<policy>-s<seed>/` (git-ignored): `manifest.json` (format
`iyi-replay` schema 1, seeds, policy, scenario, config, builds, runtime, project), `initial_state.json.gz`,
`actions.jsonl` (`tick, seq, action, ok, reason`, rejections included), `hashes.jsonl` (every 30 ticks
and after commands), `checkpoints/` + `checkpoints.json` (every 900 ticks and the end), `report.json`;
for a failure also `repro.jsonl` (shrunk commands) and `report.md`. A newer schema or a checkpoint path
outside the bundle is refused.

## Commands

```bash
python tools/playtest.py run --project ../HumanResources --policy adversarial --seed 3 --days 20
python tools/playtest.py run --project ../HumanResources --policy aggressive_growth --inject-tick 2500   # acceptance fault
python tools/playtest.py verify <bundle> --runs 3
python tools/playtest.py debug <bundle> --do "failure; diff; step; check; rng"   # no --do: interactive
python tools/playtest.py shrink <bundle>
python tools/playtest.py compare <bundle-a> <bundle-b>
python tools/playtest.py fork <bundle> --tick 1200 --out <folder>
python tools/playtest.py video <bundle> [--out failure.mp4]   # needs the game's marketing/shots/playtest_replay.py
python tools/playtest.py study --project ../HumanResources --study ../HumanResources/devtools/studies/smoke.json [--dry-run] [--baseline aggregates.json]
python tools/playtest.py ui run --project ../HumanResources --scenario ../HumanResources/devtools/scenarios/first_harvest_ui.json
python tools/playtest.py ui replay --trace <ui run folder>
python tools/playtest.py selfcheck
```

JSON on stdout. Exit: 0 ok, 1 usage or config, 3 a run failed, 4 replay diverged or did not
reproduce, 5 a UI assertion failed. Nothing starts a game except `ui`.

Debugger commands: `seek N, step [n], back [n], next, prev, failure` (the tick before it), `start,
end, show PATH, obs, metrics, check, rng, diff` (every changed path vs the previous tick), `actions [n],
where, fork FOLDER`. HR's in-game panel uses the same session (dev builds only).

## Studies (Balance Lab)

Keys (unknown keys are refused): `study, days, run_seeds {start, count}, policy_seeds, policies` (list or
`"all"`), `ceos, modes, scenario | scenarios` (fixtures are labelled setup), `overrides` (sandboxed
balance values), `runs` (only these `{"policy", "seed"}` pairs of the matrix), `workers, decision_interval_ticks` (the attention budget), `hash_every_ticks,
snapshot_every_ticks, stall_days, wall_seconds_per_run, keep` (`failures | all | none`), `verify_sample,
shrink, alerts` (thresholds), `faults`.

Each run is its own process with a hard timeout (a hang or crash is still a result). Failures are
replayed once (`reproducible`), shrunk (time-boxed), and grouped by signature. Output in the study
folder: `runs.csv, daily.csv, aggregates.json` (per cohort, by default policy / fixture: median, p10/p90,
min, max, mean, n; survival and failure rates with 95% Wilson intervals; the adapter adds its own,
HR: CEO / mode cohorts, bankruptcy rate, days to first CEO, automation, million, bankruptcy),
`alerts.json` (generic: near-zero success, soft lock, crash, divergence, performance against
`--baseline`; HR's adapter adds death spiral, CEO spread, over-reliance on one organ, frustration,
never-bought upgrade, economy exploit; each with run, seed, policy, value, threshold, repro),
`failures.json, summary.json` (sessions, game days, commands, run ends, unique feature states,
replay divergence, CPU s per 1000 game days, peak memory, bundle bytes), `report.html, report.md,
repros/<run>.json` (each job is its own repro recipe). Balance probes, not proof of fun; matched seeds
are not identical events.

## UI playtester

Scenario steps (`ui.py` doc lists them): `ui.click/type/key/scroll/drag, wait.frames, wait_until`
(engine scene/visible, or an allowlisted state comparison polled frame-locked), `observe, screenshot,
assert` (state, visible, scene, text, no_errors, ui_problems: the visual tests' UI rules,
`docs/VISUAL-TESTS.md`), `fixture` (labelled), `project.action` (a domain
command, labelled, never a UI click), `simulation.step`, `request` (raw). The run is isolated, hidden and
lockstep (`--fixed-dt 0.0333333 --seed <run_seed> --unthrottled`). Output under
`artifacts/playtests/<run>/`: `manifest.json, trace.jsonl` (each step's request, input result, state
hash), `observations.jsonl, screenshots/, errors.txt, report.json, report.html`. `ui replay` reruns the
steps in a fresh game and fails at the first step whose state hash differs.

Game keys a game answers in `AgentRequest` for the UI playtester: `read_state` (paths), `playtest_hash`,
`playtest_observe`, `playtest_legal`, `playtest_action`, `playtest_step` (agent runs only), `test_state`.

## Safety

Headless runs never open a window, audio or Steam; HR runs are debug-tainted, never autosave and write
saves to a temp folder (`tests/conftest.py`). UI runs use their own `--user-data` and never start Steam.
Commands come only from the adapter's legal set or are rejected; scenario predicates are comparisons,
never `eval`; the LLM sample answers with an index into the legal list. The agent channel is compiled
out of shipping builds; HR's panel is hidden there.

## Done (2026-11-06)

- Replay core with self-check (`selfcheck`, fake 30-line game: record, replay x3, seek forward/back,
  compare, rejected mutation, fault, shrink to 1 command, fork, schema refusal).
- HR adapter, 14 policies (`ceo_rush, new_player, aggressive_growth, conservative_cash, automation_first,
  market_timer, research_focused, retention_focused, faction_cooperative, faction_opportunist,
  recovery_downsize, idle_or_stalled, adversarial, coverage`); `tools/balance_sim.py` runs `ceo_rush`.
- HR measured: about 3,000 ticks/s headless (15 game days in 2 s); 20-day run verified x3 identical;
  injected warehouse fault found, reproduced and shrunk from 309 commands to 1 in 4.5 s; smoke study
  (5 policies x 3 seeds x 15 days, 4 workers) in 14.5 s, 0 failures, determinism sample reproducible.
- UI gate: new run, Capture, Harvest, Sell by real clicks with state asserts (4.8 s); its replay matched
  every step hash; a missing element fails with a structured reason and a failure screenshot.
- `full` study stopped by Gry at 177 of 1400 runs (ceo_rush x100, new_player x77, 60 days each): 0 failures;
  ceo_rush median $1.03M at day 60, 18 bankruptcies; new_player median $16.6K, 8 bankruptcies; about
  650 ticks/s per run with 10 workers.
- Three HR tests in `tests/test_run_rng.py` (replay x2 and divergence, seek, fault + shrink).
- 2026-11-07: study key `runs` (only the given policy/seed pairs); HR applies a study's `overrides` (allowlisted,
  restored every run) and adds a 15th policy `economic_optimizer`; HR's economy balancer on top of the Lab
  (`HumanResources/docs/economy_balancer.md`).
- 2026-11-08: HR adds a 16th policy `novice` (basic loop; buys only what the CEO advisor and tutorial hints point at).
- 2026-11-08: `playtest.py video <bundle>`: the bundle replayed in the real game and recorded by IYITrailer (a 1501-tick HR failure bundle: 52.0 s, 3.5 MB, every recorded hash matched live).

## Failure video

`playtest.py video <bundle> [--out file.mp4]` is explicit, never automatic. It runs `tools/trailer.py render` with the
project's shot `marketing/shots/playtest_replay.py` and `IYI_PLAYTEST_BUNDLE=<bundle>`; the take lands in `<bundle>/video/`
(`--out` copies the .mp4) and the command prints `ok`, `video`, `seconds`, `bytes`, `errors`. The shot is a few lines of
game glue: start the run (HR: `hr_bridge.begin` / `play`), `playtest.video.begin(adapter, bundle, live_state)`, switch the
game's own simulation actor off (`actor.active = False`) and call `playtest.video.step(...)` from `FixedUpdate30`. `step`
runs core.replay's order (the tick's commands, then the adapter's exact step) on the live state and checks every
recorded hash on the way: a live game that diverges raises, which fails the take. HR's shot: `marketing/shots/playtest_replay.py`.
A bundle must come from the current build (an older state shape breaks the UI).

## Known limits

- Viewport and locale in a UI scenario are recorded, not enforced (the agent channel has no
  resolution switch); occlusion is checked within the clicked element's own canvas only.
- Replay is same-build only; float results across machines may differ.
- Shrinking is time-boxed (180 s); a long adversarial trace may stop before minimal (`complete: false`).
- The LLM sample was not run against the API here (the `anthropic` package is not installed); without
  it, it plays its fallback policy.
- HR only: no automation-bypass oracle and no explicit "contract paid exactly once" oracle beyond
  `game.invariants`; finance metrics exist because the checked-out build has them.
- `video` runs in real time (30 ticks/s: a 1500-tick failure is a 52 s clip) and needs the game's shot (below); only HR has one.
- `verify`/`debug` load the adapter of the project a bundle's manifest names: only open bundles you or
  your own tools made. A fork ends at the last recorded hash at or before its tick.
- HR's panel opens a bundle with one replay (a few seconds on long bundles) and never ticks a bundle's
  last step (where a run ends and writes its final save): it lands on the end checkpoint instead.
- Fixed: the 3 HR tests that failed then pass now (all 770 HR tests pass, 2026-11-08).

## Next

- A second game adapter (Synthopia) when it is wanted.
