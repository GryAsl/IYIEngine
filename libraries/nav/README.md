# nav: dynamic navigation and traffic

Vehicles, walkers and helicopters on a `geo_world` map whose roads, bridges and buildings change
during play. It is C++ in the engine (`IYI/NavSim.h`, `src/NavSim.cpp`, static library
`IYIEngineSim`, SPEC §12.3) bound as `engine.Nav`. In play the whole step runs on its own thread
(below) and route searches on `engine.NavPlanner`'s search threads (up to 4). This library keeps only what is not code: `classes.json` (the movement class table, for
offline tools such as COIN's `tools/vehicle_stacks.py`) and `checks.py`. This file is the overview,
the contract with the game, and the limits; the API is the engine's `typings/engine/__init__.pyi` and SPEC §12.3.

```python
nav = engine.Nav(w, seed=7)                    # w: the geo_world world dict; wait=True for headless runs
nav.spawn_vehicle("v1", "truck", x, y, heading, side="player")
nav.drive("v1", goal)
nav.step(dt, focus=(camera_x, camera_y))       # each frame
for e in nav.events(): ...                     # what happened
nav.walkers["w1"].x                            # typed units in dict-like views (also vehicles, helis)
```

| Part | What it does |
|---|---|
| Map | Reads the world dict live (grid bits, conditions, changes) and prices cells and edges per unit class; `sync` (run by `step`) follows map changes; obstacles, hazards, route requests and answers, `open_near`, `snap`, `landing_site` |
| Vehicles | Lane, following, junction reservations, overtaking, giving way, swerving, convoys, doctrines, collisions |
| Walkers | Crowds, wandering, flee, shelter, cover, dodging moving vehicles, walking round standing ones |
| Helicopters | Cruise, a legal landing site, a recheck on the way down |
| `step(dt, focus)` | Everything above in order, once a frame |
| `checks.run(w)` | Dynamic obstruction proofs on any map (bridge, collapse, walker, landing, load), `traffic` (below) and that `classes.json` equals `engine.nav_classes()` |

The living-world classes that sit on it (`engine.Population`, `Scheduler`, `EventLog`, `Facts`) are
in SPEC §12.3; a game's `Population` pool people and cars are ordinary `nav.walkers` and `nav.vehicles`.

## Contract with the game

- **The nav thread.** `nav.step` waits for the step in flight, applies what the game wrote, copies
  the result into the units the game reads (`nav.walkers`, `vehicles`, `helis`, `time`, `events`),
  reads the map's changes and starts the next step on the nav thread. So a unit read from Python is
  the last finished step (one frame old). Writing a field (`v.goal = ...`) changes that copy at once
  and the live unit when the step ends. Calls that change or ask the live state (`drive`, `walk`,
  `spawn_*`, `request`, `passable`, `set_obstacle`, `Population.manage`, ...) first wait for the
  step in flight; `nav.join()` only waits. The nav thread never touches Python. `wait=True`
  (headless) steps inline.
- **Junctions.** A junction is a node with three or more arms, or a cluster of such nodes whose
  boxes overlap. A car asks for it on the approach and enters when no holder has a crossing
  movement and its exit has room (first come first served, priority first). Rings of cars waiting
  for or touching each other are broken by backing one off (the car behind it backs off with it);
  its routes then keep off that junction for 60 s (`AVOID_COST`), and backed off twice within 30 s
  it goes round the others on open ground. Calm traffic routes round links shorter than
  `TIGHT_LINK` between junction boxes (`TIGHT_COST`). Vehicles never pass through each other.
- **Player vehicles go to the click.** A road route gets a tail to the clicked point (straight, or
  a tight grid route round things); a road arrival more than 3 m from it finishes on the grid.
  Civilians pull over for a higher priority vehicle; a soldier overtakes civilian queues, waits at
  most 10 s in one, and its grid routes go round vehicles standing within 40 m.

- **The map changes through geo_world.** `geo_world.world.set_condition` destroys a building or a
  bridge; `nav.sync` (run by `step`) reads the change. A destroyed building spills debris
  `SPILL` metres onto the street (closed to vehicles, slow for walkers, the edge narrowed). What
  the game places itself (a wreck, a barricade) goes through `nav.set_obstacle` under a stable id.
- **Danger goes through `nav.report_hazard`.** It makes edges near it costly (more for civilians,
  medical and logistics) and alarms everyone who hears it after their reaction time.
- **nav only reports.** Collisions, hits and building contacts are events with ids, speed and
  position. Damage, deaths and morale are the game's.
- **Daily life vs a crisis.** Calm drivers keep the right-hand lane on the road graph ("road"
  mode). Blocked or alarmed, they leave the lane rules: swerve onto the verge, go across open ground
  on the grid ("free" mode), reverse or turn round. Walkers always use the grid.
- **Walkers keep to the side.** On foot a road cell costs `FOOT_ROAD` (a little more than open ground) and
  one with road all round it (its middle, a planner-only bit) `FOOT_MIDDLE` times that: people walk beside
  the road where the ground is open, else at its edge, and cross it where they must. Idle civilians wander
  to a spot off the road near a junction (`Kerb`), not onto the junction itself.
- **Walkers under orders.** A walker with `steady` set (a soldier the player sent somewhere)
  still feels an alarm (its panic rises) but does not run for cover or shelter; the game clears it
  when the order ends. `pace` multiplies its speed (a wounded soldier walks slower). A walker
  that gets no closer to its next waypoint for `STUCK` seconds (pushed onto a wall corner by a
  crowd) routes again from where it stands (event `replan`, why `stuck`). `nav.open_near(cls, p)`
  is the nearest point to p open to cls, the same one a route's goal is moved to.

## Events (`nav.events()`, which also drains them; with no step in flight it also has what direct calls emitted)

Every event has `type` and `time`.

| Type | Fields | When |
|---|---|---|
| `route` | vehicle, mode, length | A vehicle took a new route |
| `route_failed` | cls, start, goal, mode | No route exists (a request falls back from road to free first) |
| `replan` | vehicle or walker, why | The map closed its route ahead |
| `blocked` | vehicle, doctrine, by | Blocked for good: the doctrine decides next |
| `reversing` | vehicle, distance | Backing off |
| `deadlock` | vehicle, ring | A ring of `ring` vehicles waiting for each other; this one backs off |
| `give_way` | vehicle, to | A waiter standing in a junction holder's way backs off for it |
| `abandoned` | vehicle | A frightened civilian left the car (it becomes parked) |
| `alarmed` | vehicle, level | A driver heard a hazard |
| `collision` | a, b, speed, x, y | Two vehicles closing at `TOUCH` (0.5 m/s) or more, or a vehicle and a building (b is the building id); slower touches in a queue are only pushed apart |
| `hit` | vehicle, walker, speed | A vehicle hit a walker (over 1 m/s; slower is a nudge) |
| `disabled` | vehicle, x, y | `nav.disable`: now a wreck obstacle |
| `arrived` | vehicle | At the goal |
| `sheltered` / `emerged` | walker, building | A civilian hid in a building / came out when it was quiet |
| `landed` / `landing_aborted` / `no_landing_site` | heli (x, y) | Helicopter landing |

## Determinism

`engine.Nav(w, seed, wait=True)` steps inline and makes every route answer arrive on the next step
(the step waits for the search threads, whose answers are sorted by ticket), and every unit's randomness is `engine.PyRandom` seeded from `"{seed}:{id}"`, so the
same seed, `dt` and inputs give the same events. The C++ was ported line by line from the earlier
Python; the 2026-11-07 movement rewrite changed the fingerprints. With `wait=False`
(live play) answers arrive when they are ready, so runs differ a little. `step` caps `dt` at
`Nav.MAX_DT` (0.1 s) so a long frame never makes a unit jump through a wall or past a trigger.

## Cost

`step` reports to the engine's frame profiler, each section with its counts: `routes`
(`requests` since the last step), `vehicles` and `walkers` (`active`: stepped this frame,
`dormant`: far and calm, skipped; walkers' `near`: the neighbours checked to keep apart) and `air`.
Calm units farther than `Nav.WALK_FAR` (500 m) or `Nav.DRIVE_FAR` (700 m) from `focus` step every
4th frame. `nav.worker_ms` is the last step's time on the nav thread (profile count `worker_ms`).
Measured on COIN's Jowhar map (2026-11-07): the stress scene live costs the game thread about 0.8 ms
(it was 17 ms before the nav thread) and the nav thread 2 to 2.6 ms; 500 cars and 2000 walkers cost
the nav thread 5.4 ms per step. A walker's grid route costs about 4.5 ms on a search thread.

`checks.traffic(w, cars, seconds, walkers, soldiers, wait)` (agent `{"nav_check": {"traffic": {...}}}`)
drives calm cars between junctions, soldiers to points off the road and walkers between open points;
it fails when a unit stalls 60 s, a soldier's trips end more than 3 m from the click (80% must not)
or too few cars arrive, and reports what the cars are doing (`doing`) and what holds the stalled ones.

## Risks and limits

- **Culverts are a guess.** A road that crosses water without an OSM bridge tag is treated as a
  culvert or ford (passable). If the map has a real gap there, fix it in the map's
  `overrides.json`, not here. Tagged bridges are excluded, so destroying one closes its water.
- **Some calm traffic still touches.** With 150 cars for 300 s: about 40 vehicle touches and 80
  building contacts (mostly while backing off), 4 to 14 walkers hit. Vehicles keep room for their width
  off walls on grid routes, except the tight tail to a player's click.
- **A few calm cars still stall.** Over 12 seeds of 150 cars for 300 s (1800 cars), 4 cars ended
  standing 60 s or more in a junction knot that backing off had not untied yet (0 in 10 of the
  runs, 3 in the worst), and 1 walker. There is no fallback that lets them through each other.
- **A full town queues.** At 500 cars about a third wait in queues or at junctions; nothing stalls.
- **A river with one crossing has no detour.** Closing the only bridge gives an honest
  `route_failed`; the game must handle it (wait, ford, another objective).
- **Live runs are not reproducible.** Only `wait=True` is; the live game uses `wait=False` so the
  worker never stalls a frame.
- **Many walkers at once wait for their first route.** 2000 spawned together take a few seconds to
  get all their routes (4 search threads).
- **Setting up a big scenario is one long frame.** Building `engine.Nav` and the first routes take
  a moment on the game thread (the `MAX_DT` cap keeps units from jumping afterwards).
- **Helicopters do not fly physics.** Straight lines at cruise height; no obstacles in the air.

Next: untie the remaining junction knots (pairs that meet again across a junction box, a car
crawling through a junction while holding its waiters); widen geo_world's building trim beyond
buildings that close a road; look at the walker hits.
