# profiler_overlay engine library C++ (engine src/LibraryBindings.cpp): the F3 frame profiler table.

def update(mode: int, gpu: bool) -> tuple[int, bool]: ...  # the (mode, gpu) after F3 / F4
def draw(mode: int, x: int, y: int, gpu: bool) -> None: ...
