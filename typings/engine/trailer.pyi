# IYITrailer (engine docs/IYITRAILER.md): development builds only; a shipping engine has no engine.trailer.
from typing import Any

def start(name: str = "", fps: int = 0, width: int = 0, height: int = 0, audio: bool | None = None,
          output_dir: str = "", track_ui: list[str] = ...) -> None: ...  # track_ui: element rects in the report
def stop() -> None: ...
def status() -> dict[str, Any]: ...
