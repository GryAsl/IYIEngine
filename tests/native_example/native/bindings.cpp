// The only Python-aware file of the fixture: a thin pybind11 binding over the plain C++ core. Every
// call takes and returns plain data (ints, doubles, lists, tuples); no engine type and no pybind11
// object crosses into native_example.*.
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "native_example.h"

namespace py = pybind11;

PYBIND11_MODULE(native_example, m)
{
    m.doc() = "Fixture module for project-scoped native modules (docs/spec/project.md §2b).";
    m.def("add", &native_example::Add, py::arg("a"), py::arg("b"));
    m.def("accumulate", &native_example::Accumulate, py::arg("step_seconds"), py::arg("deltas"));
}
