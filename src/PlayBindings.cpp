// Exposes the Google Play bridge (AndroidPlayServices.cpp, the IYI::Api Play functions) as
// engine.play. Every function is a safe off value on desktop, iOS and when unconfigured.

#include "IYI/PlayBridge.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace IYI {

void RegisterPlayBindings(py::module_& m)
{
    py::module_ play = m.def_submodule("play", "IYI Google Play bridge (Android; off elsewhere)");

    play.def("available", &Api::PlayAvailable, "Play Games configured (release.json) and signed in");
    play.def("unlock", &Api::PlayUnlock, py::arg("achievement_id"),
        "Unlock an achievement by its release.json name; fire and forget");
    play.def("cloud_write", [](const std::string& name, const py::bytes& data, int64_t modifiedMs) {
        Api::PlayCloudWrite(name, std::string(data), modifiedMs);
    }, py::arg("name"), py::arg("data"), py::arg("modified_ms"),
       "Upload bytes to the Saved Games snapshot name; async, never touches local files");
    play.def("cloud_fetch", &Api::PlayCloudFetch, py::arg("name"), "Download snapshot name; poll cloud_result");
    play.def("cloud_result", [](const std::string& name) {
        const PlayCloud result = Api::PlayCloudResult(name);
        return py::make_tuple(result.status,
                              result.data ? py::object(py::bytes(*result.data)) : py::object(py::none()),
                              result.modifiedMs);
    }, py::arg("name"), "Fetch state: (status, data, modified_ms); status pending/ready/missing/error/off");
    play.def("cloud_names", &Api::PlayCloudNames, "Saved Games snapshot names, or None until listed after sign-in");
    play.def("request_review", &Api::PlayRequestReview, "Launch the in-app review flow (Play decides whether it shows)");
    play.def("license_state", &Api::PlayLicenseState, "Licence state: off (no key / not Android), pending, ok, denied");
}

} // namespace IYI
