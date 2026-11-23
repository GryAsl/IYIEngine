// Exposes IYITrailer (Trailer.h) to Python as engine.trailer. Development builds only: CMake leaves
// this file out of a shipping build, so the module does not exist there. start/stop only queue the
// work for the render side (they never wait, which would deadlock the two-thread mode); status()
// shows where it is.

#include "IYI/Trailer.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "IYI/FramePacket.h"

namespace py = pybind11;

namespace IYI {

void RegisterTrailerBindings(py::module_& m)
{
    py::module_ trailer = m.def_submodule("trailer", "IYITrailer: development-only gameplay recording");

    trailer.def("start", [](const std::string& name, int fps, int width, int height, py::object audio,
                            const std::string& outputDir, const std::vector<std::string>& trackUi) {
        const std::string state = TrailerStatusNow().State;
        if (state == "starting" || state == "recording" || state == "stopping") {
            throw std::runtime_error("engine.trailer.start: a recording is already " + state);
        }
        TrailerSettings s = TrailerDefaults();   // --trailer-config values, else the built-in ones
        if (!name.empty()) s.Name = name;
        if (fps > 0) s.Fps = fps;
        if (width > 0) s.Width = width;
        if (height > 0) s.Height = height;
        if (!audio.is_none()) s.Audio = audio.cast<bool>();
        if (!outputDir.empty()) s.OutputDir = outputDir;
        TrailerTrackUi(trackUi);
        TrailerMarkState("starting");
        RecordSetup([s]() { TrailerStart(s); });
    }, py::arg("name") = "", py::arg("fps") = 0, py::arg("width") = 0, py::arg("height") = 0,
       py::arg("audio") = py::none(), py::arg("output_dir") = "", py::arg("track_ui") = std::vector<std::string>{},
       "Start recording the game frame and audio into an .mp4 (0 or empty = the configured default)");

    trailer.def("stop", []() {
        const std::string state = TrailerStatusNow().State;
        if (state != "recording" && state != "starting") return;
        TrailerMarkState("stopping");
        RecordSetup([]() { TrailerStop(); });
    }, "Stop and finalize the recording; status() becomes 'done' or 'error'");

    trailer.def("status", []() {
        const TrailerStatus s = TrailerStatusNow();
        py::dict d;
        d["state"] = s.State;
        d["elapsed"] = s.Elapsed;
        d["mp4"] = s.Mp4;
        d["report"] = s.Report;
        d["error"] = s.Error;
        return d;
    }, "Recording state: dict with state, elapsed, mp4, report, error");
}

} // namespace IYI
