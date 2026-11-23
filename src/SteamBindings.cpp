// Exposes the Steamworks bridge (Steam.cpp, the IYI::Api Steam functions) to Python as engine.steam.

#include "IYI/SteamBridge.h"
#include "IYI/Application.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace IYI {

void RegisterSteamBindings(py::module_& m)
{
    py::module_ steam = m.def_submodule("steam", "IYI Steamworks bridge");

    steam.def("available", &Api::SteamAvailable, "Check if Steam API is initialized and available");
    steam.def("stats_ready", &Api::SteamStatsReady, "Check if user stats and achievements have synchronized");
    steam.def("has_achievement", &Api::SteamHasAchievement, py::arg("name"), "Check if achievement is unlocked; returns bool or None");
    steam.def("set_achievement", &Api::SteamSetAchievement, py::arg("name"), "Unlock an achievement; returns bool");
    steam.def("store_stats", &Api::SteamStoreStats, "Store stats and achievements on the Steam backend; returns bool");
    steam.def("open_store", &Api::SteamOpenStore, py::arg("app_id"),
        "Open an app's store page in the Steam overlay; False when the overlay cannot (caller falls back)");
    steam.def("upload_leaderboard_score", &Api::SteamUploadLeaderboardScore,
        py::arg("name"),
        py::arg("score"),
        py::arg("details") = std::vector<int64_t>{},
        py::arg("ascending") = false,
        "Upload a leaderboard score asynchronously (score/details saturate to int32); returns integer handle (0 on failure)"
    );
    steam.def("request_leaderboard", &Api::SteamRequestLeaderboard,
        py::arg("name"),
        py::arg("around_user") = true,
        py::arg("limit") = 10,
        py::arg("ascending") = false,
        "Request leaderboard entries asynchronously; returns integer handle (0 on failure)"
    );
    steam.def("request_status", &Api::SteamRequestStatus, py::arg("handle"), "Get async request status: 'pending', 'ok', 'failed', or 'unknown'");
    steam.def("leaderboard_entries", [](int handle) {
        py::list list;
        for (const auto& entry : Api::SteamLeaderboardEntries(handle)) {
            py::dict d;
            d["rank"] = entry.rank;
            d["name"] = entry.name;
            d["score"] = entry.score;
            d["details"] = entry.details;
            list.append(d);
        }
        return list;
    }, py::arg("handle"), "Get leaderboard entries as list of dicts for handle");
    steam.def("release", &Api::SteamRelease, py::arg("handle"), "Release and free async request handle");

    // Steam Cloud: the game syncs its own small files (SPEC §21).
    steam.def("cloud_enabled", &Api::SteamCloudEnabled, "True when Steam is up and cloud is on for the account and this game");
    steam.def("cloud_files", [] {
        py::list list;
        for (const auto& file : Api::SteamCloudFiles()) {
            py::dict d;
            d["name"] = file.name;
            d["size"] = file.size;
            d["timestamp"] = file.timestamp;
            list.append(d);
        }
        return list;
    }, "Files in this game's cloud as dicts: name, size, timestamp (Unix seconds)");
    steam.def("cloud_read", [](const std::string& name) -> py::object {
        const std::optional<std::string> data = Api::SteamCloudRead(name);
        return data ? py::object(py::bytes(*data)) : py::object(py::none());
    }, py::arg("name"), "A cloud file's bytes, or None");
    steam.def("cloud_write", [](const std::string& name, const py::bytes& data) {
        return Api::SteamCloudWrite(name, std::string(data));
    }, py::arg("name"), py::arg("data"), "Write bytes to a cloud file; returns bool");
    steam.def("cloud_delete", &Api::SteamCloudDelete, py::arg("name"), "Delete a cloud file; returns bool");
    steam.def("avatar_texture", [](int size) -> Texture2D* {
        auto avatar = Api::SteamAvatar();
        if (!avatar) {
            return nullptr;
        }
        return CurrentApp().GetAssets().PutTexture("steam/avatar#" + std::to_string(size), avatar->rgba,
                                                   avatar->width, avatar->height, size);
    }, py::arg("size") = 0, py::return_value_policy::reference,
       "The player's Steam avatar as a texture (resized to size x size when size > 0); None without Steam or while it loads");
}

} // namespace IYI
