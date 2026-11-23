// The Steamworks bridge: Steam lifecycle, achievements, leaderboards and stats synchronization (the
// IYI::Api Steam functions, SteamBridge.h). Python reaches them as engine.steam (SteamBindings.cpp).
// Keeps public engine headers clean by encapsulating all Steamworks SDK headers in this translation unit.

#include "IYI/SteamBridge.h"
#include "IYI/Log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef IYI_STEAM
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "steam_api.h"
#endif

namespace IYI {

#ifdef IYI_STEAM

static bool s_available = false;
static bool s_statsReady = false;
static int s_nextHandle = 0;

class SteamStatsListener {
public:
    CCallbackManual<SteamStatsListener, UserStatsReceived_t> m_callback;

    void OnUserStatsReceived(UserStatsReceived_t* pCallback)
    {
        if (!pCallback) return;
        if (SteamUser() && pCallback->m_eResult == k_EResultOK && pCallback->m_steamIDUser == SteamUser()->GetSteamID()) {
            s_statsReady = true;
            LogWrite(LogLevel::Info, "steam", "User stats received and ready");
        } else if (pCallback->m_eResult != k_EResultOK) {
            LogWrite(LogLevel::Warn, "steam", "User stats receive returned result " + std::to_string(static_cast<int>(pCallback->m_eResult)));
        }
    }
};

static std::unique_ptr<SteamStatsListener> s_statsListener;
static std::unordered_map<std::string, SteamLeaderboard_t> s_leaderboardCache;

class SteamLeaderboardRequest {
public:
    enum class Status { Pending, Ok, Failed };
    enum class RequestType { Upload, Download };

    int m_handle = 0;
    Status m_status = Status::Pending;
    RequestType m_type = RequestType::Upload;

    std::string m_name;
    int32_t m_score = 0;
    std::vector<int32_t> m_uploadDetails;

    bool m_aroundUser = true;
    int m_limit = 10;

    using Entry = Api::SteamLeaderboardEntry;
    std::vector<Entry> m_entries;

    CCallResult<SteamLeaderboardRequest, LeaderboardFindResult_t> m_findCallResult;
    CCallResult<SteamLeaderboardRequest, LeaderboardScoreUploaded_t> m_uploadCallResult;
    CCallResult<SteamLeaderboardRequest, LeaderboardScoresDownloaded_t> m_downloadCallResult;

    void OnLeaderboardFind(LeaderboardFindResult_t* pResult, bool bIOFailure)
    {
        if (bIOFailure || !pResult || pResult->m_bLeaderboardFound == 0 || pResult->m_hSteamLeaderboard == 0 || !SteamUserStats()) {
            m_status = Status::Failed;
            return;
        }

        SteamLeaderboard_t hLeaderboard = pResult->m_hSteamLeaderboard;
        s_leaderboardCache[m_name] = hLeaderboard;

        if (m_type == RequestType::Upload) {
            const int32_t* pDetails = m_uploadDetails.empty() ? nullptr : m_uploadDetails.data();
            int detailCount = static_cast<int>(m_uploadDetails.size());
            SteamAPICall_t call = SteamUserStats()->UploadLeaderboardScore(
                hLeaderboard,
                k_ELeaderboardUploadScoreMethodKeepBest,
                m_score,
                pDetails,
                detailCount
            );
            if (call == k_uAPICallInvalid) {
                m_status = Status::Failed;
                return;
            }
            m_uploadCallResult.Set(call, this, &SteamLeaderboardRequest::OnLeaderboardUpload);
        } else if (m_type == RequestType::Download) {
            ELeaderboardDataRequest reqType = m_aroundUser ? k_ELeaderboardDataRequestGlobalAroundUser : k_ELeaderboardDataRequestGlobal;
            int rangeStart = 1;
            int rangeEnd = m_limit;
            if (m_aroundUser) {
                int half = (m_limit - 1) / 2;
                rangeStart = -half;
                rangeEnd = m_limit - 1 - half;
            }
            SteamAPICall_t call = SteamUserStats()->DownloadLeaderboardEntries(
                hLeaderboard,
                reqType,
                rangeStart,
                rangeEnd
            );
            if (call == k_uAPICallInvalid) {
                m_status = Status::Failed;
                return;
            }
            m_downloadCallResult.Set(call, this, &SteamLeaderboardRequest::OnLeaderboardDownload);
        }
    }

    void OnLeaderboardUpload(LeaderboardScoreUploaded_t* pResult, bool bIOFailure)
    {
        if (bIOFailure || !pResult || pResult->m_bSuccess == 0) {
            m_status = Status::Failed;
            return;
        }
        m_status = Status::Ok;
    }

    void OnLeaderboardDownload(LeaderboardScoresDownloaded_t* pResult, bool bIOFailure)
    {
        if (bIOFailure || !pResult || !SteamUserStats()) {
            m_status = Status::Failed;
            return;
        }

        m_entries.clear();
        int count = pResult->m_cEntryCount;
        for (int i = 0; i < count; ++i) {
            LeaderboardEntry_t entry;
            int32_t details[64] = { 0 };
            if (SteamUserStats()->GetDownloadedLeaderboardEntry(pResult->m_hSteamLeaderboardEntries, i, &entry, details, 64)) {
                Entry e;
                e.rank = entry.m_nGlobalRank;
                e.score = entry.m_nScore;
                if (SteamFriends()) {
                    const char* persona = SteamFriends()->GetFriendPersonaName(entry.m_steamIDUser);
                    e.name = persona ? persona : "";
                } else {
                    e.name = "";
                }
                int cDetails = std::clamp(entry.m_cDetails, 0, 64);
                e.details.assign(details, details + cDetails);
                m_entries.push_back(std::move(e));
            }
        }
        m_status = Status::Ok;
    }
};

static std::unordered_map<int, std::unique_ptr<SteamLeaderboardRequest>> s_requests;

// Handles are never reused while live: skip any id still present in the map.
static int AllocHandle()
{
    return NextFreeHandle(s_nextHandle, [](int h) { return s_requests.count(h) != 0; });
}

#endif // IYI_STEAM

SteamInitResult SteamStartup(int appId)
{
#ifdef IYI_STEAM
    if (appId == 0) {
        return SteamInitResult::Failed;
    }

#ifdef IYI_SHIPPING_BUILD
    if (SteamAPI_RestartAppIfNecessary(static_cast<uint32>(appId))) {
        LogWrite(LogLevel::Info, "steam", "SteamAPI_RestartAppIfNecessary requested restart; exiting");
        return SteamInitResult::RestartAppQuit;
    }
#else
    std::string appIdStr = std::to_string(appId);
#if defined(_WIN32)
    _putenv_s("SteamAppId", appIdStr.c_str());
    _putenv_s("SteamGameId", appIdStr.c_str());
#else
    setenv("SteamAppId", appIdStr.c_str(), 1);
    setenv("SteamGameId", appIdStr.c_str(), 1);
#endif
#endif

    SteamErrMsg errMsg = { 0 };
    ESteamAPIInitResult initResult = SteamAPI_InitEx(&errMsg);
    if (initResult != k_ESteamAPIInitResult_OK) {
        LogWrite(LogLevel::Warn, "steam", std::string("SteamAPI_InitEx failed: ") + errMsg);
        return SteamInitResult::Failed;
    }

    s_available = true;
    s_statsReady = false;
    LogWrite(LogLevel::Info, "steam", "Steam initialized successfully (AppID " + std::to_string(appId) + ")");

    if (!s_statsListener) {
        s_statsListener = std::make_unique<SteamStatsListener>();
    }
    s_statsListener->m_callback.Register(s_statsListener.get(), &SteamStatsListener::OnUserStatsReceived);

    return SteamInitResult::Ok;
#else
    (void)appId;
    LogWrite(LogLevel::Warn, "steam", "Steam support not compiled into this build");
    return SteamInitResult::Failed;
#endif
}

void SteamRunFrame()
{
#ifdef IYI_STEAM
    if (s_available) {
        SteamAPI_RunCallbacks();
    }
#endif
}

void SteamShutdown()
{
#ifdef IYI_STEAM
    if (s_available) {
        LogWrite(LogLevel::Info, "steam", "Shutting down SteamAPI...");
        if (s_statsListener) {
            s_statsListener->m_callback.Unregister();
            s_statsListener.reset();
        }
        s_requests.clear();
        s_leaderboardCache.clear();
        s_statsReady = false;
        s_available = false;
        SteamAPI_Shutdown();
        LogWrite(LogLevel::Info, "steam", "SteamAPI shutdown complete");
    }
#endif
}

namespace Api {

bool SteamAvailable()
{
#ifdef IYI_STEAM
    return s_available;
#else
    return false;
#endif
}

bool SteamStatsReady()
{
#ifdef IYI_STEAM
    return s_available && s_statsReady;
#else
    return false;
#endif
}

std::optional<bool> SteamHasAchievement(const std::string& name)
{
#ifdef IYI_STEAM
    if (!s_available || !s_statsReady || !SteamUserStats()) {
        return std::nullopt;
    }
    bool achieved = false;
    if (!SteamUserStats()->GetAchievement(name.c_str(), &achieved)) {
        return std::nullopt;
    }
    return achieved;
#else
    (void)name;
    return std::nullopt;
#endif
}

bool SteamSetAchievement(const std::string& name)
{
#ifdef IYI_STEAM
    if (!s_available || !s_statsReady || !SteamUserStats()) {
        return false;
    }
    return SteamUserStats()->SetAchievement(name.c_str());
#else
    (void)name;
    return false;
#endif
}

bool SteamStoreStats()
{
#ifdef IYI_STEAM
    if (!s_available || !s_statsReady || !SteamUserStats()) {
        return false;
    }
    return SteamUserStats()->StoreStats();
#else
    return false;
#endif
}

bool SteamCloudEnabled()
{
#ifdef IYI_STEAM
    ISteamRemoteStorage* cloud = s_available ? SteamRemoteStorage() : nullptr;
    return cloud && cloud->IsCloudEnabledForAccount() && cloud->IsCloudEnabledForApp();
#else
    return false;
#endif
}

std::vector<SteamCloudFile> SteamCloudFiles()
{
    std::vector<SteamCloudFile> files;
#ifdef IYI_STEAM
    if (!SteamCloudEnabled()) return files;
    ISteamRemoteStorage* cloud = SteamRemoteStorage();
    for (int i = 0, count = cloud->GetFileCount(); i < count; ++i) {
        int32 size = 0;
        const char* name = cloud->GetFileNameAndSize(i, &size);
        if (name) files.push_back({ name, size, cloud->GetFileTimestamp(name) });
    }
#endif
    return files;
}

std::optional<std::string> SteamCloudRead(const std::string& name)
{
#ifdef IYI_STEAM
    if (!SteamCloudEnabled() || !SteamRemoteStorage()->FileExists(name.c_str())) return std::nullopt;
    std::string data(static_cast<size_t>(SteamRemoteStorage()->GetFileSize(name.c_str())), '\0');
    const int32 read = SteamRemoteStorage()->FileRead(name.c_str(), data.data(), static_cast<int32>(data.size()));
    if (read != static_cast<int32>(data.size())) return std::nullopt;
    return data;
#else
    (void)name;
    return std::nullopt;
#endif
}

bool SteamCloudWrite(const std::string& name, const std::string& data)
{
#ifdef IYI_STEAM
    return SteamCloudEnabled() && SteamRemoteStorage()->FileWrite(name.c_str(), data.data(), static_cast<int32>(data.size()));
#else
    (void)name, (void)data;
    return false;
#endif
}

bool SteamCloudDelete(const std::string& name)
{
#ifdef IYI_STEAM
    return SteamCloudEnabled() && SteamRemoteStorage()->FileDelete(name.c_str());
#else
    (void)name;
    return false;
#endif
}

std::optional<SteamImage> SteamAvatar()
{
#ifdef IYI_STEAM
    if (!s_available || !SteamFriends() || !SteamUser() || !SteamUtils()) {
        return std::nullopt;
    }
    const int handle = SteamFriends()->GetMediumFriendAvatar(SteamUser()->GetSteamID());   // 0: none, -1: loading
    uint32 width = 0, height = 0;
    if (handle <= 0 || !SteamUtils()->GetImageSize(handle, &width, &height) || width == 0 || height == 0) {
        return std::nullopt;
    }
    SteamImage image{ static_cast<int>(width), static_cast<int>(height), std::string(size_t(width) * height * 4, char(0)) };
    if (!SteamUtils()->GetImageRGBA(handle, reinterpret_cast<uint8*>(image.rgba.data()), static_cast<int>(image.rgba.size()))) {
        return std::nullopt;
    }
    return image;
#else
    return std::nullopt;
#endif
}

// False when the overlay cannot show it (no Steam, overlay disabled or not hooked yet): the caller falls back.
bool SteamOpenStore(int64_t appId)
{
#ifdef IYI_STEAM
    if (!s_available || appId <= 0 || appId > UINT32_MAX || !SteamFriends() || !SteamUtils()
        || !SteamUtils()->IsOverlayEnabled()) {
        return false;
    }
    SteamFriends()->ActivateGameOverlayToStore(static_cast<AppId_t>(appId), k_EOverlayToStoreFlag_None);
    return true;
#else
    (void)appId;
    return false;
#endif
}

// score/details arrive as int64 (a Python int beyond int64 is rejected by pybind11 with TypeError) and
// are saturated to int32 here, never narrowed by a cast. The first saturation is logged once.
int SteamUploadLeaderboardScore(const std::string& name, int64_t rawScore, const std::vector<int64_t>& details, bool ascending)
{
#ifdef IYI_STEAM
    if (!s_available || !SteamUserStats()) {
        return 0;
    }

    static bool s_loggedClamp = false;
    const int32_t score = ClampToInt32(rawScore);
    bool clamped = (score != rawScore);
    std::vector<int32_t> clampedDetails;
    size_t count = std::min<size_t>(details.size(), 64);
    clampedDetails.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        int32_t d = ClampToInt32(details[i]);
        clamped = clamped || (d != details[i]);
        clampedDetails.push_back(d);
    }
    if (clamped && !s_loggedClamp) {
        s_loggedClamp = true;
        LogWrite(LogLevel::Warn, "steam", "leaderboard score/details exceeded int32 and were clamped (logged once)");
    }

    auto it = s_leaderboardCache.find(name);
    if (it != s_leaderboardCache.end()) {
        SteamLeaderboard_t hLeaderboard = it->second;
        const int32_t* pDetails = clampedDetails.empty() ? nullptr : clampedDetails.data();
        int detailCount = static_cast<int>(clampedDetails.size());
        SteamAPICall_t call = SteamUserStats()->UploadLeaderboardScore(
            hLeaderboard,
            k_ELeaderboardUploadScoreMethodKeepBest,
            score,
            pDetails,
            detailCount
        );
        if (call == k_uAPICallInvalid) {
            return 0;
        }

        int handle = AllocHandle();
        auto req = std::make_unique<SteamLeaderboardRequest>();
        req->m_handle = handle;
        req->m_status = SteamLeaderboardRequest::Status::Pending;
        req->m_type = SteamLeaderboardRequest::RequestType::Upload;
        req->m_name = name;
        req->m_score = score;
        req->m_uploadDetails = std::move(clampedDetails);
        req->m_uploadCallResult.Set(call, req.get(), &SteamLeaderboardRequest::OnLeaderboardUpload);
        s_requests[handle] = std::move(req);
        return handle;
    } else {
        ELeaderboardSortMethod sortMethod = ascending ? k_ELeaderboardSortMethodAscending : k_ELeaderboardSortMethodDescending;
        SteamAPICall_t call = SteamUserStats()->FindOrCreateLeaderboard(
            name.c_str(),
            sortMethod,
            k_ELeaderboardDisplayTypeNumeric
        );
        if (call == k_uAPICallInvalid) {
            return 0;
        }

        int handle = AllocHandle();
        auto req = std::make_unique<SteamLeaderboardRequest>();
        req->m_handle = handle;
        req->m_status = SteamLeaderboardRequest::Status::Pending;
        req->m_type = SteamLeaderboardRequest::RequestType::Upload;
        req->m_name = name;
        req->m_score = score;
        req->m_uploadDetails = std::move(clampedDetails);
        req->m_findCallResult.Set(call, req.get(), &SteamLeaderboardRequest::OnLeaderboardFind);
        s_requests[handle] = std::move(req);
        return handle;
    }
#else
    (void)name; (void)rawScore; (void)details; (void)ascending;
    return 0;
#endif
}

int SteamRequestLeaderboard(const std::string& name, bool around_user, int limit, bool ascending)
{
#ifdef IYI_STEAM
    if (!s_available || !SteamUserStats()) {
        return 0;
    }

    if (limit <= 0) {
        limit = 1;
    }

    auto it = s_leaderboardCache.find(name);
    if (it != s_leaderboardCache.end()) {
        SteamLeaderboard_t hLeaderboard = it->second;
        ELeaderboardDataRequest reqType = around_user ? k_ELeaderboardDataRequestGlobalAroundUser : k_ELeaderboardDataRequestGlobal;
        int rangeStart = 1;
        int rangeEnd = limit;
        if (around_user) {
            int half = (limit - 1) / 2;
            rangeStart = -half;
            rangeEnd = limit - 1 - half;
        }

        SteamAPICall_t call = SteamUserStats()->DownloadLeaderboardEntries(
            hLeaderboard,
            reqType,
            rangeStart,
            rangeEnd
        );
        if (call == k_uAPICallInvalid) {
            return 0;
        }

        int handle = AllocHandle();
        auto req = std::make_unique<SteamLeaderboardRequest>();
        req->m_handle = handle;
        req->m_status = SteamLeaderboardRequest::Status::Pending;
        req->m_type = SteamLeaderboardRequest::RequestType::Download;
        req->m_name = name;
        req->m_aroundUser = around_user;
        req->m_limit = limit;
        req->m_downloadCallResult.Set(call, req.get(), &SteamLeaderboardRequest::OnLeaderboardDownload);
        s_requests[handle] = std::move(req);
        return handle;
    } else {
        ELeaderboardSortMethod sortMethod = ascending ? k_ELeaderboardSortMethodAscending : k_ELeaderboardSortMethodDescending;
        SteamAPICall_t call = SteamUserStats()->FindOrCreateLeaderboard(
            name.c_str(),
            sortMethod,
            k_ELeaderboardDisplayTypeNumeric
        );
        if (call == k_uAPICallInvalid) {
            return 0;
        }

        int handle = AllocHandle();
        auto req = std::make_unique<SteamLeaderboardRequest>();
        req->m_handle = handle;
        req->m_status = SteamLeaderboardRequest::Status::Pending;
        req->m_type = SteamLeaderboardRequest::RequestType::Download;
        req->m_name = name;
        req->m_aroundUser = around_user;
        req->m_limit = limit;
        req->m_findCallResult.Set(call, req.get(), &SteamLeaderboardRequest::OnLeaderboardFind);
        s_requests[handle] = std::move(req);
        return handle;
    }
#else
    (void)name; (void)around_user; (void)limit; (void)ascending;
    return 0;
#endif
}

std::string SteamRequestStatus(int handle)
{
#ifdef IYI_STEAM
    auto it = s_requests.find(handle);
    if (it == s_requests.end()) {
        return "unknown";
    }
    switch (it->second->m_status) {
        case SteamLeaderboardRequest::Status::Pending: return "pending";
        case SteamLeaderboardRequest::Status::Ok:      return "ok";
        case SteamLeaderboardRequest::Status::Failed:  return "failed";
    }
    return "unknown";
#else
    (void)handle;
    return "unknown";
#endif
}

std::vector<SteamLeaderboardEntry> SteamLeaderboardEntries(int handle)
{
#ifdef IYI_STEAM
    auto it = s_requests.find(handle);
    if (it == s_requests.end() || it->second->m_status != SteamLeaderboardRequest::Status::Ok) {
        return {};
    }
    return it->second->m_entries;
#else
    (void)handle;
    return {};
#endif
}

void SteamRelease(int handle)
{
#ifdef IYI_STEAM
    s_requests.erase(handle);
#else
    (void)handle;
#endif
}

} // namespace Api

} // namespace IYI
