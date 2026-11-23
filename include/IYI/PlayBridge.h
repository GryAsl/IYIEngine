// engine.play for C++ (SPEC §24.5): Google Play Games achievements, Saved Games snapshots, in-app
// review and the LVL licence state. Java (PlayServices.java) does the async work and keeps the
// results; C++ only reads them through the activity's JNI methods. Every call is a no-op / "off"
// on desktop, iOS, and when release.json leaves the Play field empty (the Android shell gates on it).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace IYI {

// One Saved Games snapshot fetch: status "pending", "ready", "missing", "error" or "off".
struct PlayCloud {
    std::string status = "off";
    std::optional<std::string> data;
    int64_t modifiedMs = 0;   // the snapshot's last-modified time (0 unless ready)
};

namespace Api {

bool PlayAvailable();   // Play Games configured (release.json) and signed in
void PlayUnlock(const std::string& achievementId);   // release.json name, not the Play id; fire and forget
void PlayCloudWrite(const std::string& name, const std::string& data, int64_t modifiedMs);
void PlayCloudFetch(const std::string& name);   // async; poll PlayCloudResult
PlayCloud PlayCloudResult(const std::string& name);
std::optional<std::vector<std::string>> PlayCloudNames();   // snapshot names; nullopt until listed
void PlayRequestReview();
std::string PlayLicenseState();   // "off", "pending", "ok" or "denied"

} // namespace Api

} // namespace IYI
