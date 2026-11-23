# Gate 2: real CPython + the real pybind11 engine module, inside the browser.
import sys, zlib, json
import engine


def GameStart():
    print(sys.version)
    v = engine.Vector2(3.0, 4.0)
    r = engine.Rectangle(1.0, 2.0, 30.0, 40.0)
    try:
        engine.Vector2("bad")  # pybind11 C++ exception -> Python TypeError
        cast_error = "none"
    except TypeError as e:
        cast_error = type(e).__name__
    print("GATE2", json.dumps({
        "platform": sys.platform, "v": [v.x, v.y], "rect_w": r.width, "KEY_A": engine.KEY_A,
        "zlib": len(zlib.compress(b"x" * 1000)), "cast_error": cast_error,
    }))
    print("IYI_WEB_PYBIND_OK")
