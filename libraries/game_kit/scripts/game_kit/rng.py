"""Deterministic seeded random streams: mulberry32 over uint32 values kept in a dict the caller owns.

A game keeps every stream as one int in its save (`streams[name]`), so a seed reproduces a run and a
loaded save resumes the exact next roll. Give each system its own stream so one system's rolls never
shift another's.
"""

MASK = 0xFFFFFFFF
GOLDEN = 0x9E3779B1


def derive(seed: int, salt: int = 0, n: int = 0) -> int:
    """A stream's start value from a run seed, a per-stream salt and an optional counter."""
    return (seed ^ salt ^ (n * GOLDEN)) & MASK


def is_valid(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= MASK


def random(streams: dict, name: str) -> float:
    """Next float in [0, 1) from streams[name], advancing it."""
    s = (streams[name] + 0x6D2B79F5) & MASK
    streams[name] = s
    t = ((s ^ (s >> 15)) * (s | 1)) & MASK
    t ^= (t + (((t ^ (t >> 7)) * (t | 61)) & MASK)) & MASK
    return ((t ^ (t >> 14)) & MASK) / 4294967296.0


def uniform(streams: dict, name: str, a: float, b: float) -> float:
    return a + (b - a) * random(streams, name)


def randint(streams: dict, name: str, a: int, b: int) -> int:
    """Inclusive on both ends, like random.randint."""
    return a + int(random(streams, name) * (b - a + 1))


def choice(streams: dict, name: str, seq):
    return seq[int(random(streams, name) * len(seq))]


def weighted(streams: dict, name: str, items, weight):
    """One item picked with probability weight(item) / total; None if nothing has weight."""
    total = sum(max(0.0, weight(i)) for i in items)
    if total <= 0:
        return None
    r = random(streams, name) * total
    for item in items:
        r -= max(0.0, weight(item))
        if r < 0:
            return item
    return next(i for i in reversed(items) if weight(i) > 0)  # float rounding left r just >= 0


if __name__ == "__main__":  # self-check: python rng.py
    a, b = {"x": derive(42, 1)}, {"x": derive(42, 1)}
    seq = [random(a, "x") for _ in range(100)]
    assert seq == [random(b, "x") for _ in range(100)] and all(0 <= v < 1 for v in seq)
    assert {randint(a, "x", 1, 3) for _ in range(200)} == {1, 2, 3}
    assert weighted(a, "x", ["a", "b"], lambda i: 0 if i == "a" else 1) == "b"
    print("rng ok")
