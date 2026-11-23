// Exposes the audio subsystem to Python scripts under engine.audio: thin calls into Api::Audio* (AudioManager.h),
// which record into the frame packet's Audio pass and keep the game side's view of the audio state.

#include "IYI/AudioManager.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace IYI {

void RegisterAudioBindings(py::module_& m)
{
    py::module_ audio = m.def_submodule("audio", "IYI audio subsystem");

    audio.def("play", &Api::AudioPlay, py::arg("path"), py::arg("volume") = 1.0f, py::arg("pitch") = 1.0f, py::arg("pan") = 0.5f, py::arg("loop") = false, py::arg("bus") = "sfx",
       "Play a sound and return its handle");
    audio.def("play_at", &Api::AudioPlayAt, py::arg("path"), py::arg("x"), py::arg("y"), py::arg("volume") = 1.0f, py::arg("pitch") = 1.0f, py::arg("loop") = false, py::arg("bus") = "sfx",
       "Play a positional sound at (x, y) and return its handle");
    audio.def("stop", &Api::AudioStop, py::arg("h"), "Stop playing sound by handle");
    audio.def("pause", &Api::AudioPause, py::arg("h"), "Pause playing sound by handle");
    audio.def("resume", &Api::AudioResume, py::arg("h"), "Resume paused sound by handle");
    audio.def("is_playing", &Api::AudioIsPlaying, py::arg("h"), "Check if sound handle is currently playing");
    audio.def("set_volume", &Api::AudioSetVolume, py::arg("h"), py::arg("v"), "Set volume for sound handle");
    audio.def("set_pitch", &Api::AudioSetPitch, py::arg("h"), py::arg("p"), "Set pitch for sound handle");
    audio.def("set_pan", &Api::AudioSetPan, py::arg("h"), py::arg("p"), "Set pan for sound handle (0.0 left, 0.5 center, 1.0 right)");
    audio.def("set_position", &Api::AudioSetPosition, py::arg("h"), py::arg("x"), py::arg("y"), "Set world position for sound handle");
    audio.def("stop_all", &Api::AudioStopAll, py::arg("bus") = "", "Stop all sounds, optionally filtered by bus");
    audio.def("set_listener", &Api::AudioSetListener, py::arg("x"), py::arg("y"), "Set listener 2D position for spatial audio");
    audio.def("set_falloff", &Api::AudioSetFalloff, py::arg("near"), py::arg("far"), "Set near and far falloff distances for spatial audio");
    audio.def("music", &Api::AudioMusic, py::arg("path"), py::arg("fade") = 0.0f, py::arg("loop") = true, py::arg("volume") = 1.0f,
       "Play music stream with optional crossfade, looping, and volume");
    audio.def("stop_music", &Api::AudioStopMusic, py::arg("fade") = 0.0f, "Stop music with optional fade-out");
    audio.def("pause_music", &Api::AudioPauseMusic, "Pause currently playing music");
    audio.def("resume_music", &Api::AudioResumeMusic, "Resume paused music");
    audio.def("set_music_pitch", &Api::AudioSetMusicPitch, py::arg("pitch"), "Set speed and pitch of the music streams (1 = normal)");
    audio.def("current_music", &Api::AudioCurrentMusic, "Get path of currently playing music or empty string");
    audio.def("queue", &Api::AudioQueue, py::arg("path"), "Queue a track to play when current non-looping track ends");
    audio.def("clear_queue", &Api::AudioClearQueue, "Clear the music queue");
    audio.def("set_bus_volume", &Api::AudioSetBusVolume, py::arg("name"), py::arg("v"), "Set volume multiplier for an audio bus");
    audio.def("get_bus_volume", &Api::AudioGetBusVolume, py::arg("name"), "Get volume multiplier for an audio bus");
    audio.def("set_bus_gain", &Api::AudioSetBusGain, py::arg("name"), py::arg("gain"),
              "Session-only bus multiplier (0..1) on top of the saved volume; never written to audio.json");
}

} // namespace IYI
