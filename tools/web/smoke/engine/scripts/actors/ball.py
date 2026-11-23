import math
import engine


def Start(actor):
    actor.state["t"] = 0.0
    actor.state["updates"] = 0


def Update(actor, dt):
    actor.state["t"] += dt
    actor.state["updates"] += 1
    if actor.state["updates"] in (1, 60, 300):
        print(f"GATE3 ball updates={actor.state['updates']} t={actor.state['t']:.2f} fps={engine.get_fps()}")


def Drawing(actor):
    t = actor.state["t"]
    engine.draw_circle(int(480 + 200 * math.cos(t)), int(270 + 100 * math.sin(t)), 30, engine.ORANGE)
