// Exposes the video player (Video.h) to Python as engine.video. open() never raises for a bad file:
// the video's state is "error" and error says why.

#include "IYI/Video.h"
#include <pybind11/pybind11.h>

namespace py = pybind11;

namespace IYI {

void RegisterVideoBindings(py::module_& m)
{
    py::module_ video = m.def_submodule("video", "Video playback: MPEG-1 video with optional MP2 audio (.mpg)");

    py::class_<Video, std::shared_ptr<Video>>(video, "Video")
        .def("play", &Video::Play, "Play from the start (ready or ended) or resume (paused)")
        .def("pause", &Video::Pause)
        .def("resume", &Video::Resume)
        .def("stop", &Video::Stop, "Stop and rewind to the start: state 'ready'")
        .def("close", &Video::Close, "Free the decoder, texture and stream now; safe to repeat")
        .def("set_volume", &Video::SetVolume, py::arg("volume"), "0..1, times the master and music buses")
        .def("draw_fit", [](Video& v, float x, float y, float w, float h) { v.DrawFit({ x, y, w, h }); },
             py::arg("x"), py::arg("y"), py::arg("width"), py::arg("height"),
             "Draw black over the area and the current frame aspect-fit in its middle")
        .def_property_readonly("state", &Video::StateName, "ready | playing | paused | ended | error | closed")
        .def_property_readonly("duration", &Video::Duration)
        .def_property_readonly("position", &Video::Position)
        .def_property_readonly("width", &Video::Width)
        .def_property_readonly("height", &Video::Height)
        .def_property_readonly("has_audio", &Video::HasAudio)
        .def_property_readonly("error", &Video::Error)
        .def("__repr__", [](const Video& v) {
            return std::string("<Video ") + v.StateName() + " " + std::to_string(v.Width()) + "x" + std::to_string(v.Height()) + ">";
        });

    video.def("open", &OpenVideo, py::arg("path"), "Open a project-relative .mpg; check .state for 'error'");
}

} // namespace IYI
