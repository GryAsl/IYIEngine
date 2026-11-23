"""Crash-safe file writes: a sibling temp file, fsync, then os.replace over the real file.

A crash or full disk mid-write can never leave a truncated file behind: the old file stays whole
until the new one is complete.
"""

import json
import os
import time


def write_atomic(path: str, data: bytes) -> None:
    """Raises OSError on failure (the old file is left untouched, the temp file removed)."""
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    temp = f"{path}.{os.getpid()}.{time.time_ns()}.tmp"
    try:
        with open(temp, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(temp, path)
    except BaseException:
        try:
            os.remove(temp)
        except OSError:
            pass
        raise


def write_json_atomic(path: str, data, indent: int | None = 2) -> None:
    write_atomic(path, json.dumps(data, indent=indent).encode("utf-8"))


if __name__ == "__main__":  # self-check: python files.py
    import tempfile
    d = tempfile.mkdtemp()
    p = os.path.join(d, "sub", "a.json")
    write_json_atomic(p, {"a": 1})
    try:
        write_atomic(p, object())  # not bytes: fails mid-write
    except TypeError:
        pass
    assert json.load(open(p)) == {"a": 1} and os.listdir(os.path.dirname(p)) == ["a.json"]
    print("files ok")
