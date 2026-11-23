# Gate 3: the real engine loop in the browser: GameStart -> scene -> actor hooks + IYIUI canvas.
import engine


def GameStart():
    print("GATE3 GameStart")
    engine.change_scene("scripts/scenes/smoke.py")
