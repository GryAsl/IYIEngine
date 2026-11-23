// Declares the plain C++ interface to Steamworks: lifecycle and the IYI::Api Steam functions. The
// Steamworks SDK headers stay in src/Steam.cpp and pybind11 in src/SteamBindings.cpp.
//
// Connected to:
// - Implemented in src/Steam.cpp; SteamBindings.cpp exposes engine.steam.
// - Called by Application::Initialize, Application::GameStep (SteamRunFrame) and Application::Shutdown.

#pragma once

#include <cstdint>
#include <climits>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace IYI {

enum class SteamInitResult {
    Ok,
    Failed,
    RestartAppQuit
};

// Steam leaderboard scores and details are int32. Values outside the range saturate; they are never wrapped.
inline int32_t ClampToInt32(int64_t v)
{
    if (v > INT32_MAX) return INT32_MAX;
    if (v < INT32_MIN) return INT32_MIN;
    return static_cast<int32_t>(v);
}

// Next async-request handle: positive, wraps past INT_MAX, and never equals a handle still live.
inline int NextFreeHandle(int& counter, const std::function<bool(int)>& inUse)
{
    do {
        counter = (counter <= 0 || counter == INT_MAX) ? 1 : counter + 1;  // no signed overflow (UB)
    } while (inUse(counter));
    return counter;
}

SteamInitResult SteamStartup(int appId);
void SteamRunFrame();
void SteamShutdown();

// engine.steam for C++ (SPEC §21); all are safe stubs when Steam is unavailable.
namespace Api {
struct SteamLeaderboardEntry { int rank = 0; std::string name; int score = 0; std::vector<int> details; };
bool SteamAvailable();
bool SteamStatsReady();
std::optional<bool> SteamHasAchievement(const std::string& name);   // nullopt: unknown (stats not ready)
bool SteamSetAchievement(const std::string& name);
bool SteamStoreStats();
bool SteamOpenStore(int64_t appId);
int SteamUploadLeaderboardScore(const std::string& name, int64_t score, const std::vector<int64_t>& details = {}, bool ascending = false);
int SteamRequestLeaderboard(const std::string& name, bool aroundUser = true, int limit = 10, bool ascending = false);
std::string SteamRequestStatus(int handle);   // "pending", "ok", "failed" or "unknown"
std::vector<SteamLeaderboardEntry> SteamLeaderboardEntries(int handle);
void SteamRelease(int handle);

// Steam Cloud (ISteamRemoteStorage): small files the game syncs itself, under the quota set in the
// Steamworks panel. Synchronous; every call is false / empty when Steam or the player's cloud is off.
struct SteamCloudFile { std::string name; int size = 0; int64_t timestamp = 0; };   // Unix seconds
bool SteamCloudEnabled();   // Steam is up and cloud is on for both the account and this game
std::vector<SteamCloudFile> SteamCloudFiles();
std::optional<std::string> SteamCloudRead(const std::string& name);   // the file's bytes
bool SteamCloudWrite(const std::string& name, const std::string& data);
bool SteamCloudDelete(const std::string& name);

// The player's own Steam avatar (ISteamFriends medium, 64 x 64), RGBA8 rows top first. nullopt without
// Steam, without an avatar, or while Steam is still downloading it (asking starts that: ask again later).
struct SteamImage { int width = 0; int height = 0; std::string rgba; };
std::optional<SteamImage> SteamAvatar();
} // namespace Api

} // namespace IYI
