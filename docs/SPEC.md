# IYI Engine, Implementation Contract (v1): index

Authoritative spec. Derived from the user's "C++ + Python 2D Game Engine, Full
Implementation Plan". Where this file and the plan disagree, **this file wins**
(it records decisions the user already approved).

For the retained-mode UI subsystem contract, see `docs/SPEC-IYIUI.md`.

Guiding rule from the plan: **simplest thing that works**. No managers, services,
factories, DI, templates-for-their-own-sake, deep inheritance, or generic framework
code. If a small function or class solves it, write that.

Split by area on 2026-11-11 so an agent reads only the part it needs (the whole was about 43K tokens).
**Read the file for the area you touch, not all of them.** Section numbers (§3.1, §12, ...) are
unchanged; a reference like "SPEC.md §3.1" means the file below that holds §3.

| File | Sections | Covers |
|---|---|---|
| [spec/decisions.md](spec/decisions.md) | §0 | Approved decisions |
| [spec/project.md](spec/project.md) | §1, §2, §2b, §2c | Repository layout, project.json, native modules, native games |
| [spec/cli.md](spec/cli.md) | §3 | CLI, agent channel, crash reports |
| [spec/runtime.md](spec/runtime.md) | §4, §5, §6, §7, §8, §8a, §9, §10, §11, §13 | Actors, registry, scene, main loop, threads, fixed updates, assets, Python runtime, hot reload, script contract |
| [spec/python-api.md](spec/python-api.md) | §12 | The `engine` Python module (the whole script API) |
| [spec/build.md](spec/build.md) | §14, §14b, §15, §16 | Build, packaging, validation scope, non-goals |
| [spec/logging.md](spec/logging.md) | §17 | Logging |
| [spec/resources.md](spec/resources.md) | §18 | ResourceStore, shipping, release layouts |
| [spec/localization.md](spec/localization.md) | §19 | Localization (`engine.loc`) |
| [spec/audio.md](spec/audio.md) | §20 | Audio (`engine.audio`) |
| [spec/steam.md](spec/steam.md) | §21 | Steamworks (`engine.steam`) |
| [spec/trailer.md](spec/trailer.md) | §22 | IYITrailer (dev only) |
| [spec/video.md](spec/video.md) | §23 | Video (`engine.video`) |
| [spec/mobile.md](spec/mobile.md) | §24 | Android and iOS targets: build switches, paths, runtime policy, packaging |
| [spec/net.md](spec/net.md) | §25 | Multiplayer (`libraries/net`, `engine.net`): opt-in, host-based, Steam + ENet |
