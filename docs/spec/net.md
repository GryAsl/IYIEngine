# IYI Engine contract: Multiplayer (`libraries/net`, `engine.net`)

Part of the engine contract; index and rules in [../SPEC.md](../SPEC.md). Section numbers are the contract's own.

## 25. Multiplayer (`libraries/net`)

**Status: implemented (e8bda6a, 2026-11-21), desktop only.** Gry approved the scope on 2026-11-21 (all
three layers, Steam + ENet, interpolation and owner prediction, host migration from replicated state,
usable from Python and C++, present only in games that list it). Tested: `loopback` (native suite:
replication, loss, budget, messages, inputs, authority, interest, refusal, random bytes, migration)
and `enet` on 127.0.0.1 (two `tests/net_example` processes: join, replication, migration). Not yet
tested: `steam` (never compiled against the Steamworks SDK), ENet LAN discovery, ENet across real
networks and NAT, and `net_predict` inside the engine (checked only in plain Python). A detail later
use proves wrong is changed here deliberately and named in the report.

Lessons taken from SuperStrategy's host-authoritative netcode (2026-07): keep its intent → validate →
authoritative state flow, its ping/timeout tracking, its loopback transport for tests and its bounded
decoding. Leave behind its full-state snapshot every tick, its hand-written per-game codec, its
Steam-only transport, `printf` logging and the transport factory/interface hierarchy.

### 25.1 What it is for, and what it is not

- **For**: host-based games of any genre with up to 16 players: strategy, co-op, party games,
  casual and co-op FPS or action. One player's game is the host; the others are clients.
- **Not for** (left out on purpose): dedicated servers, anti-cheat, competitive FPS (the host has an
  advantage and sees everything), deterministic lockstep or rollback (fighting games), MMO-scale
  interest management, the web build (no UDP there).

### 25.2 Opt-in: a library with its own build switch

The library has the usual layout (§2) plus native code: `libraries/net/src/` (the C++),
`libraries/net/scripts/net_predict.py` (the Python helper of §25.9). It is absent from every game that does not
use it:

- **Build switch `IYI_NET`** (`AUTO`/`ON`/`OFF`, CMake cache, like `IYI_STEAM`). `OFF`: none of
  `libraries/net/src/` and no ENet is compiled, no `engine.net` binding is registered. `AUTO` (the
  default) is `ON` when `libraries/net/src/Net.h` exists, so a development build (`build.bat`) has it
  and a tree without the library still builds. Android, iOS and the web force it `OFF`.
- **The game asks for it** by listing `"../IYIEngine/libraries/net"` in `project.json` `libraries`.
  `tools/release.py` passes `-DIYI_NET=ON` only then; every other shipping build is `OFF`.
- **Development matches shipping**: in a development build, `import engine.net` fails with a clear
  error (`ImportError: engine.net needs "../IYIEngine/libraries/net" in project.json "libraries"`) on the
  first use of `engine.net.<anything>` when the project does not list
  the library, so a game cannot use it by accident and break only when packaged.
- **Native games** (§2c) link it from their own CMake tree with `IYI_NET=ON`; without it the code is
  not there. Its C++ header is `"net/src/Net.h"`, like the other libraries (§2c).
- **Platforms**: desktop (Windows verified first). ENet itself runs on Android and iOS, but v1 lists the
  library under `desktop_libraries` (§2): mobile follows when a mobile game needs it. Steam is desktop
  only (§24).

### 25.3 Threads and frame order

No new thread. Everything runs on the game thread (sockets are not GL, GLFW, live input or audio, so
rule 29 allows it; rule 30 is untouched):

```
GameStep(k): Steam callbacks; net receive (decode, apply snapshots, queue messages and events);
             PollHotReload; StartPending; RunFixedUpdates; Update;
             net send (host: replicate after each network tick; everyone: flush queued messages);
             Drawing ...
```

The **network tick** (`tick_rate`, default 30) is one of 60, 30, 20, 15 or 10 and counts on the 60 Hz
fixed clock (§9), so it adds no new timeline: the host replicates after every 60/`tick_rate`-th
`FixedUpdate60`. A stalled game step stalls networking too; peers time out only after `timeout`
(default 10 s), so a loading hitch does not drop players.

### 25.4 Sessions and transports

One session at a time. Transports:

| `transport` | Connects | Finds games | NAT |
|---|---|---|---|
| `"steam"` | Steam IDs over Steam Networking Messages | Steam lobbies (public, friends, invite) | Steam relays, nothing to open |
| `"enet"` | IPv4 address and UDP port | Direct address; LAN discovery by UDP broadcast on port 47777 | Host must be reachable (LAN, port forward) |
| `"loopback"` | Sessions inside one process | — | — |

`loopback` exists for the native suite. An agent run never uses Steam (cli.md) and binds ENet only to
127.0.0.1, so agent runs stay off the real network.

Every transport carries two channels: **reliable ordered** (messages, spawns, despawns, events) and
**unreliable sequenced** (snapshots, inputs). One packet stays under 1200 bytes; a reliable message up
to 64 KiB is fragmented by the transport, an unreliable payload never is (§25.6 keeps it small).

Packet header (12 bytes): protocol magic and version, packet type, session id, host epoch (§25.8),
sequence. A packet with a wrong magic, version, session or an older epoch is dropped and counted. A
snapshot with nothing changed is 21 bytes (header, tick, input ack, end bit).

### 25.5 Python API (`engine.net`)

Polling only: nothing is a callback, so nothing caches a Python function across frames (rule 1).

```python
from engine import net

# --- session ---
net.host(transport="enet", port=7777, max_players=8, tick_rate=30, name="", public=True, data={},
         player_name="", timeout=10.0, migration_timeout=3.0) -> bool
net.join(target, player_name="") -> bool   # "1.2.3.4:7777", a "found" event dict, "steam:<lobby>"
net.simulate(latency_ms=0, jitter_ms=0, loss=0)   # outgoing latency/jitter/loss, development builds
net.find(transport="steam") -> None   # starts a search; results arrive as "found" events
net.leave() -> None
net.state() -> str             # "offline" | "hosting" | "joining" | "connected" | "migrating"
net.is_host() -> bool
net.local_player() -> int      # 0 while offline; the host is not necessarily player 1 after migration
net.players() -> list[dict]    # [{"id", "name", "rtt_ms", "loss", "is_host"}]
net.tick() -> int              # host: authoritative tick; client: estimated host tick now
net.stats() -> dict            # bytes in/out per second, packets, drops, per-peer rtt and loss
net.set_lobby_data(key, value) # host; visible in "found" results

# --- events (once each, in order, since the last call) ---
net.events() -> list[dict]     # {"type": "connected"|"joined"|"left"|"found"|"invited"|"host_changed"
                               #          |"became_host"|"disconnected", ...}

# --- replicated objects (§25.6) ---
net.define(kind, **fields) -> None   # once per kind, same on every peer; field types below
obj = net.spawn(kind, owner=0, authority="host", priority=1.0, **values)  # host only
net.objects(kind=None) -> list[net.Object]
net.get(object_id) -> net.Object | None
obj.id, obj.kind, obj.owner          # owner: player id, 0 = the host's
obj.exists                           # False once despawned or hidden
obj.authority                        # "host" or "owner" (owner writes, host relays and clamps)
obj.x = 3.5                          # write: host (or owner if authority="owner"); else ValueError
obj.x                                # read: the latest received value
obj.sample("x")                      # read: interpolated at the client's render time (§25.7)
obj.hide_from(player) / obj.show_to(player)   # host: interest, default visible to all
obj.despawn()                        # host only

# --- messages (RPC) ---
net.message(name, **fields) -> None  # declare once, same on every peer
net.send(name, to=net.HOST, reliable=True, **values)   # to: net.HOST, net.ALL, net.OTHERS, or a player id
net.receive() -> list[dict]          # {"name", "from", "tick", **values}, in arrival order

# --- inputs (for prediction, §25.9) ---
net.define_input(**fields) -> None   # once, same on every peer
net.send_input(**values) -> int      # client: returns its seq; unreliable, resent until acked
net.inputs(player) -> list[dict]     # host: inputs taken since the last call, each with "seq" and "tick"
net.input_ack() -> int               # client: the last input seq the host had taken when it sent the
                                     # newest snapshot (the state in that snapshot includes it)

# --- lag compensation ---
net.rewind(tick) -> dict[int, dict]  # host: {object_id: {field: value}} as replicated at that tick
                                     # (kept for 1 s; older ticks clamp to the oldest kept)
```

**Field types** (`net.define`, `net.message`): `net.bool`, `net.u8`, `net.u16`, `net.u32`, `net.i8`,
`net.i16`, `net.i32`, `net.f32`, `net.fixed(min, max, step)` (quantized to the fewest bits),
`net.angle(bits)`, `net.vec2(min, max, step)`, `net.vec3(min, max, step)`, `net.text(max_bytes)`,
`net.bytes(max_bytes)`, `net.player` (a player id), `net.ref` (an object id). Angles are in degrees.
Writes outside a type's range clamp and log once; a field's type never changes after `define`.
Reserved field names: objects `id`, `kind`, `owner`, `authority`, `priority`, `exists`, `sample`,
`hide_from`, `show_to`, `despawn`; messages and inputs `name`, `from`, `tick`, `seq`.

Failures never raise inside the session's own work: a call that cannot run (not hosting, unknown kind,
peer gone) returns `False`/`None`/`[]` and logs once (category `net`). Only programmer errors raise
(`TypeError` for a wrong field type, `ValueError` for a write without authority).

### 25.6 Replication

- **Host-authoritative by default.** The host owns every object's state. A client writes only objects
  spawned with `authority="owner"` for itself; the host clamps those writes to the field types and
  relays them. Everything else a client wants goes to the host as a message or input, and the host's
  game code decides.
- **Delta per client.** For each client the host keeps the tick of the last snapshot that client
  acked. A snapshot carries, per visible object, only the fields that changed since that baseline
  (one dirty bit per field), bit-packed at each field's quantized width. A client with no usable
  baseline (just joined, or older than the 32-tick history) gets full objects.
- **Spawns and despawns** go on the reliable channel before the first snapshot that names the object.
- **Bandwidth budget.** One snapshot is at most one packet per client per network tick. Objects that do
  not fit wait; each object's `priority` times the ticks since it was last sent orders the queue, so
  nothing starves. `max_bandwidth` (bytes/s per client, default 64 KiB) caps the rate.
- **Interest.** `hide_from`/`show_to` decide which clients get an object at all (fog of war, other
  rooms). Hidden objects are despawned on that client and respawned in full when shown.
- **Costs are C++.** Per-tick diffing, packing and the per-client queues never run in Python; Python
  only writes fields. This is the reason the library is C++ (rule: "runs every frame" alone is not).

### 25.7 Time and interpolation

The host stamps every snapshot with its tick. Each client estimates the host tick from RTT and keeps
the last 32 received states per object. `obj.sample(field)` returns the value at
`render_time = estimated host time - interpolation delay` (default 2 network ticks + jitter), linear
for numbers, vectors and fixed, shortest-path for angles, the older value for every other type.
`obj.x` stays the raw latest value. The host's `sample` returns the current value.

### 25.8 Host migration

From replicated state only, no backup stream:

1. A client that hears nothing from the host for `migration_timeout` (default 3 s), or gets the
   host's leave, enters `"migrating"`.
2. **Election** is the same on every peer: the connected player who joined earliest (Steam: whoever
   Steam made the new lobby owner, which wins over the order if they differ).
3. The new host keeps every object its replica had, takes authority, raises the **epoch** by one and
   continues the tick count. It gets a `became_host` event; the others get `host_changed`.
4. The others reconnect to it: Steam by Steam id (relays, no NAT issue); ENet by the address the old
   host shared with everyone on join. A peer that cannot reach the new host within `timeout` gets
   `disconnected`.
5. The game rebuilds what only the old host had (AI plans, RNG, queues) in its `became_host` handling.

**Lost in a migration**: objects hidden from the new host, reliable messages and inputs in flight to
the old host, and any host-only state the game did not replicate. ENet migration works when the new
host is reachable from the others (LAN, open port); on the internet use Steam.

### 25.9 Python helper (`libraries/net/scripts/net_predict.py`)

Small, opt-in, built only on §25.5:

- **`net_predict.Predictor(step, snap_distance=64, smoothing=0.15)`**: client-side prediction for the
  local player's host-authoritative object. The game passes one pure function
  `step(state, input, dt) -> state`; `apply(input, dt, host_state)` sends the input, drops inputs up
  to `net.input_ack()`, replays the rest over the host state and returns the prediction;
  `shown(state)` adds a fading correction so mispredictions under `snap_distance` glide. A separate
  module, not `engine.net.predict`, so `import` never shadows `engine.net`.
- **`net.lobby_ui`**: none in v1. The engine draws nothing (rule 9); a lobby screen is game UI.

Interpolation is not a helper: it is `obj.sample` in C++ (§25.7), so smoothing many objects costs no
Python.

### 25.10 C++ API (`IYI::Api`, `"net/src/Net.h"`)

The same functions as §25.5, one C++ implementation that the binding calls (`Api::NetHost`,
`Api::NetJoin`, `Api::NetEvents`, `Api::NetDefine`, `Api::NetSpawn`, `Api::NetSet`/`NetGet`/
`NetSample`, `Api::NetSend`/`NetReceive`, `Api::NetSendInput`/`NetInputs`, `Api::NetRewind`, ...).
Values cross as a small `NetValue` (variant of bool, integer, float, `Vector2`, `Vector3`, string,
bytes). `Net.h` includes no ENet, Steam, Python or JSON header (rule 2 and the library's own rule).
Each function gets a CHEATSHEET line when it is written.

### 25.11 Limits, logging, safety

- Limits: 16 players, 65 535 live objects, 64 fields per kind, 256 kinds, 256 message names, a
  message up to 64 KiB, text fields up to 1 KiB.
- Logging through `LogWrite`, category `net` (connects, leaves, migrations, drops by reason once per
  reason per peer). Profiler zones `net_receive` and `net_send`.
- Decoding is bounded like SuperStrategy's codec: every count, length and enum is checked against the
  schema before use, and a peer that sends an invalid packet more than a few times is dropped.
- Message and input packets per peer are capped at 240/s; excess is dropped and counted.
- `net.define`/`net.message` schemas are hashed into the join handshake: peers with different
  schemas (a different game build) are refused with a clear reason.

### 25.12 Testing

- **Native suite**: two or more sessions over `loopback` in one process: delta correctness against
  full state, quantization bounds, the bandwidth queue never starving, migration election and epoch,
  bounded decoding fed random bytes.
- **Simulated network**: `net.simulate(latency_ms=80, jitter_ms=20, loss=0.05)` (development builds
  only) for every transport.
- **Test game `tests/net_example/`**: the smallest game that uses the library. Each player is a
  coloured square moved with the arrow keys; the window title says host or client. It starts with
  `net.join("127.0.0.1:7777")` and hosts instead when nothing answers within one second, so two copies
  started on one machine play together with no setup. Closing the first copy shows migration. The
  gate: two `--frames` runs started together on 127.0.0.1 both log `IYI_RUNTIME_READY`, see each
  other's square, and log bytes per network tick.
