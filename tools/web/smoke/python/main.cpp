// Gate 2: start the engine's own PythonRuntime (embedded CPython + the pybind11 "engine" module) in the
// browser and run proj/scripts/main.py GameStart(). No window, no Application: only engine API that needs neither.
#include "IYI/PythonRuntime.h"
#include <cstdio>

int main()
{
    IYI::PythonRuntime py;
    bool ok = py.Initialize("/proj", "scripts/main.py", false, nullptr, {}, {});
    printf("GATE2 initialize %d\n", ok);
    if (ok) printf("GATE2 gamestart %d\n", py.CallGameStart());
    py.Shutdown();
    printf("GATE2 done\n");
}
