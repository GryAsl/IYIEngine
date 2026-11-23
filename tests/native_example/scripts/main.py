# Fixture entry script for the engine's project-scoped native modules (docs/spec/project.md §2b).
# It does nothing else: spawn one actor (so the engine's verification line prints) and prove that the
# game's own compiled module imports and returns plain data in development and in a packaged build.
import engine
import native_example


def GameStart():
    engine.spawn_actor(name="NativeProbe", script="scripts/probe.py", persistent=True)
    ticks, remainder = native_example.accumulate(0.5, [0.2, 0.2, 0.2, 0.2, 0.2])
    engine.log_info(
        "native_example: add(2, 3) = {}; accumulate(0.5s, 5x0.2s) = {} ticks, {:.1f}s left".format(
            native_example.add(2, 3), ticks, remainder))
