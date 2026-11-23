"""Warn-once logging for fallbacks that must not fail silently or spam the log every frame."""

try:  # headless tests import this without the engine
    import engine
except ImportError:
    engine = None

_seen: set[str] = set()


def warn_once(key: str, message: str) -> None:
    if key in _seen:
        return
    _seen.add(key)
    if engine is None:
        return
    try:
        engine.log_warn(f"[{key}] {message}", "game")
    except Exception:  # logging itself must never break gameplay
        pass
