// Implements the core localization subsystem for IYIEngine.
// Manages locale discovery, flat string key loading, placeholder formatting,
// fallback resolution, and development hot-reloading.

#include "IYI/LocalizationManager.h"
#include "IYI/ResourceStore.h"
#include "IYI/AssetManager.h"
#include "IYI/Log.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>

#ifndef IYI_SHIPPING_BUILD
#include <filesystem>
#endif

namespace IYI {

static std::string ToLower(std::string_view str)
{
    std::string out;
    out.reserve(str.size());
    for (char c : str) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

LocalizationManager& LocalizationManager::Get()
{
    static LocalizationManager instance;
    return instance;
}

void LocalizationManager::Initialize(AssetManager* assets, const std::string& fallback)
{
    ++m_revision;
    m_resources = assets ? &assets->GetResourceStore() : nullptr;
    m_fallbackLanguage = ToLower(fallback);
    if (m_fallbackLanguage.empty()) {
        m_fallbackLanguage = "en";
    }
    m_currentLanguage = m_fallbackLanguage;

    DiscoverLanguages();

    LoadLanguage(m_fallbackLanguage, m_fallbackStrings);
    m_currentStrings = m_fallbackStrings;

#ifndef IYI_SHIPPING_BUILD
    m_pollTimer = 0.0f;
#endif
}

void LocalizationManager::SetLanguage(const std::string& code)
{
    ++m_revision;
    std::string lower = ToLower(code);
    if (lower == m_currentLanguage) {
        return;
    }

    m_currentLanguage = lower;

    if (m_currentLanguage == "__keys__") {
        m_currentStrings.clear();
        return;
    }

    if (m_currentLanguage == m_fallbackLanguage) {
        m_currentStrings = m_fallbackStrings;
#ifndef IYI_SHIPPING_BUILD
        m_activeFileMTime = m_fallbackFileMTime;
#endif
        return;
    }

    if (!LoadLanguage(m_currentLanguage, m_currentStrings)) {
        m_currentStrings.clear();   // not the previous language's strings: Text falls back
    }
}

const std::string& LocalizationManager::GetLanguage() const
{
    return m_currentLanguage;
}

void LocalizationManager::SetFallbackLanguage(const std::string& code)
{
    ++m_revision;
    std::string lower = ToLower(code);
    if (lower == m_fallbackLanguage || lower.empty()) {
        return;
    }

    m_fallbackLanguage = lower;
    if (!LoadLanguage(m_fallbackLanguage, m_fallbackStrings)) {
        m_fallbackStrings.clear();
    }

    if (m_currentLanguage == m_fallbackLanguage) {
        m_currentStrings = m_fallbackStrings;
#ifndef IYI_SHIPPING_BUILD
        m_activeFileMTime = m_fallbackFileMTime;
#endif
    }
}

const std::string& LocalizationManager::GetFallbackLanguage() const
{
    return m_fallbackLanguage;
}

std::string LocalizationManager::Text(const std::string& key, const std::unordered_map<std::string, std::string>& args) const
{
    if (m_currentLanguage == "__keys__") {
        return "[" + key + "]";
    }

    auto it = m_currentStrings.find(key);
    if (it != m_currentStrings.end() && !it->second.empty()) {
        return FormatPlaceholders(key, it->second, args);
    }

    if (m_currentLanguage != m_fallbackLanguage) {
        auto fbIt = m_fallbackStrings.find(key);
        if (fbIt != m_fallbackStrings.end() && !fbIt->second.empty()) {
            return FormatPlaceholders(key, fbIt->second, args);
        }
    }

    m_missingKeys.insert(key);
    std::string logKey = m_currentLanguage + ":" + key;
    if (m_loggedMissingKeys.insert(logKey).second) {
        LogWrite(LogLevel::Warn, "loc", "Missing localization key '" + key + "' for language '" + m_currentLanguage + "'");
    }

    return "[" + key + "]";
}

bool LocalizationManager::Has(const std::string& key) const
{
    if (m_currentLanguage == "__keys__") {
        return true;
    }

    auto it = m_currentStrings.find(key);
    if (it != m_currentStrings.end() && !it->second.empty()) {
        return true;
    }

    if (m_currentLanguage != m_fallbackLanguage) {
        auto fbIt = m_fallbackStrings.find(key);
        if (fbIt != m_fallbackStrings.end() && !fbIt->second.empty()) {
            return true;
        }
    }

    return false;
}

const std::vector<LocaleInfo>& LocalizationManager::Languages() const
{
    return m_languages;
}

const LocaleInfo* LocalizationManager::GetLanguageInfo(const std::string& code) const
{
    std::string lower = ToLower(code);
    for (const auto& info : m_languages) {
        if (info.Code == lower) {
            return &info;
        }
    }
    return nullptr;
}

const std::unordered_set<std::string>& LocalizationManager::MissingKeys() const
{
    return m_missingKeys;
}

void LocalizationManager::Reload()
{
    ++m_revision;
    m_missingKeys.clear();
    m_loggedMissingKeys.clear();
    m_warnedArgs.clear();

    DiscoverLanguages();

    LoadLanguage(m_fallbackLanguage, m_fallbackStrings);
    if (m_currentLanguage == "__keys__") {
        m_currentStrings.clear();
    } else if (m_currentLanguage == m_fallbackLanguage) {
        m_currentStrings = m_fallbackStrings;
    } else {
        LoadLanguage(m_currentLanguage, m_currentStrings);
    }

#ifndef IYI_SHIPPING_BUILD
    m_pollTimer = 0.0f;
#endif
}

void LocalizationManager::DiscoverLanguages()
{
    m_languages.clear();
    if (!m_resources) {
        return;
    }

    const std::string catalogPath = "assets/locales/_catalog.json";
    if (m_resources->Exists(catalogPath)) {
        std::string catalogText;
        if (m_resources->ReadText(catalogPath, catalogText)) {
            try {
                auto j = nlohmann::json::parse(catalogText);
                if (j.is_object() && j.contains("languages") && j["languages"].is_array()) {
                    for (const auto& item : j["languages"]) {
                        if (!item.is_object()) continue;
                        LocaleInfo info;
                        if (item.contains("code") && item["code"].is_string()) {
                            info.Code = ToLower(item["code"].get<std::string>());
                        }
                        if (info.Code.empty()) continue;

                        if (item.contains("name") && item["name"].is_string()) {
                            info.Name = item["name"].get<std::string>();
                        } else {
                            info.Name = info.Code;
                        }

                        if (item.contains("native_name") && item["native_name"].is_string()) {
                            info.NativeName = item["native_name"].get<std::string>();
                        } else {
                            info.NativeName = info.Name;
                        }

                        m_languages.push_back(std::move(info));
                    }
                }
            } catch (const std::exception& e) {
                LogWrite(LogLevel::Error, "loc", "Failed to parse _catalog.json: " + std::string(e.what()));
            }
        }
    }

#if !defined(IYI_SHIPPING_BUILD) && !defined(IYI_MOBILE)
    if (m_languages.empty()) {
        std::string projPhys = m_resources->GetPhysicalPath("project.json");
        if (!projPhys.empty()) {
            std::filesystem::path projPath(projPhys);
            std::filesystem::path localesDir = projPath.parent_path() / "assets" / "locales";
            std::error_code ec;
            if (std::filesystem::exists(localesDir, ec) && std::filesystem::is_directory(localesDir, ec)) {
                for (const auto& entry : std::filesystem::directory_iterator(localesDir, ec)) {
                    if (ec) break;
                    if (!entry.is_regular_file(ec)) continue;
                    auto p = entry.path();
                    if (p.extension() != ".json") continue;

                    std::string stem;
#if defined(_WIN32)
                    auto u8Stem = p.stem().u8string();
                    stem = std::string(u8Stem.begin(), u8Stem.end());
#else
                    stem = p.stem().string();
#endif
                    if (stem.empty() || stem[0] == '_') continue;

                    std::string code = ToLower(stem);
                    LocaleInfo info;
                    info.Code = code;
                    info.Name = code;
                    info.NativeName = code;

                    std::string localeText;
                    if (m_resources->ReadText("assets/locales/" + code + ".json", localeText)) {
                        try {
                            auto j = nlohmann::json::parse(localeText);
                            if (j.is_object() && j.contains("_meta") && j["_meta"].is_object()) {
                                auto& meta = j["_meta"];
                                if (meta.contains("name") && meta["name"].is_string()) {
                                    info.Name = meta["name"].get<std::string>();
                                }
                                if (meta.contains("native_name") && meta["native_name"].is_string()) {
                                    info.NativeName = meta["native_name"].get<std::string>();
                                }
                            }
                        } catch (...) {
                        }
                    }

                    m_languages.push_back(std::move(info));
                }

                std::sort(m_languages.begin(), m_languages.end(), [](const LocaleInfo& a, const LocaleInfo& b) {
                    return a.Code < b.Code;
                });
            }
        }
    }
#endif

    bool fallbackFound = false;
    for (const auto& l : m_languages) {
        if (l.Code == m_fallbackLanguage) {
            fallbackFound = true;
            break;
        }
    }
    if (!fallbackFound && !m_fallbackLanguage.empty()) {
        LocaleInfo fbInfo;
        fbInfo.Code = m_fallbackLanguage;
        fbInfo.Name = m_fallbackLanguage;
        fbInfo.NativeName = m_fallbackLanguage;
        std::string fbText;
        if (m_resources->ReadText("assets/locales/" + m_fallbackLanguage + ".json", fbText)) {
            try {
                auto j = nlohmann::json::parse(fbText);
                if (j.is_object() && j.contains("_meta") && j["_meta"].is_object()) {
                    auto& meta = j["_meta"];
                    if (meta.contains("name") && meta["name"].is_string()) {
                        fbInfo.Name = meta["name"].get<std::string>();
                    }
                    if (meta.contains("native_name") && meta["native_name"].is_string()) {
                        fbInfo.NativeName = meta["native_name"].get<std::string>();
                    }
                }
            } catch (...) {
            }
        }
        m_languages.push_back(std::move(fbInfo));
    }
}

bool LocalizationManager::LoadLanguage(const std::string& code, std::unordered_map<std::string, std::string>& outStrings)
{
    if (!m_resources || code.empty() || code == "__keys__") {
        return false;
    }

    std::string relPath = "assets/locales/" + code + ".json";
    if (!m_resources->Exists(relPath)) {
        LogWrite(LogLevel::Warn, "loc", "Locale file not found: " + relPath);
        return false;
    }

    std::string text;
    if (!m_resources->ReadText(relPath, text)) {
        LogWrite(LogLevel::Error, "loc", "Failed to read locale file: " + relPath);
        return false;
    }

#ifndef IYI_SHIPPING_BUILD
    // Recorded before parsing, so hot reload waits for the next edit of a broken file.
    std::string phys = m_resources->GetPhysicalPath(relPath);
    if (!phys.empty()) {
        std::error_code ec;
        auto mtime = std::filesystem::last_write_time(phys, ec);
        if (!ec) {
            uint64_t mtimeVal = static_cast<uint64_t>(mtime.time_since_epoch().count());
            if (code == m_currentLanguage) m_activeFileMTime = mtimeVal;
            if (code == m_fallbackLanguage) m_fallbackFileMTime = mtimeVal;
        }
    }
#endif

    try {
        auto j = nlohmann::json::parse(text);
        if (!j.is_object()) {
            LogWrite(LogLevel::Error, "loc", "Invalid locale file '" + relPath + "': root must be a JSON object");
            return false;
        }

        std::unordered_map<std::string, std::string> parsedStrings;
        std::string metaName;
        std::string metaNativeName;
        bool hasMeta = false;

        for (auto& [k, v] : j.items()) {
            if (k == "_meta") {
                if (v.is_object()) {
                    hasMeta = true;
                    if (v.contains("name") && v["name"].is_string()) {
                        metaName = v["name"].get<std::string>();
                    }
                    if (v.contains("native_name") && v["native_name"].is_string()) {
                        metaNativeName = v["native_name"].get<std::string>();
                    }
                }
            } else if (v.is_string()) {
                parsedStrings[k] = v.get<std::string>();
            }
        }

        outStrings = std::move(parsedStrings);

        if (hasMeta) {
            for (auto& info : m_languages) {
                if (info.Code == code) {
                    if (!metaName.empty()) info.Name = metaName;
                    if (!metaNativeName.empty()) info.NativeName = metaNativeName;
                    break;
                }
            }
        }

        return true;
    } catch (const std::exception& e) {
        LogWrite(LogLevel::Error, "loc", "Failed to parse locale file '" + relPath + "': " + e.what());
        return false;
    }
}

std::string LocalizationManager::FormatPlaceholders(const std::string& key, const std::string& text, const std::unordered_map<std::string, std::string>& args) const
{
    (void)key;
    if (text.find("${") == std::string::npos) {
        return text;
    }

    std::string result;
    result.reserve(text.size() + 16);

    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '$' && i + 1 < text.size() && text[i + 1] == '{') {
            size_t end = text.find('}', i + 2);
            if (end != std::string::npos) {
                std::string argName = text.substr(i + 2, end - (i + 2));
                auto it = args.find(argName);
                if (it != args.end()) {
                    result += it->second;
                } else {
                    result += text.substr(i, end - i + 1);
#ifndef IYI_SHIPPING_BUILD
                    std::string warnKey = key + ":" + argName;
                    if (m_warnedArgs.insert(warnKey).second) {
                        LogWrite(LogLevel::Warn, "loc", "Missing placeholder argument '${" + argName + "}' for key '" + key + "'");
                    }
#endif
                }
                i = end + 1;
                continue;
            }
        }
        result += text[i];
        ++i;
    }

    return result;
}

#ifndef IYI_SHIPPING_BUILD
void LocalizationManager::PollForChanges(float dt)
{
#ifdef IYI_MOBILE
    (void)dt;
    return;
#endif
    if (!m_resources) {
        return;
    }

    m_pollTimer += dt;
    if (m_pollTimer < 0.25f) {
        return;
    }
    m_pollTimer = 0.0f;

    if (m_currentLanguage != "__keys__") {
        CheckFileForChange(m_currentLanguage, m_currentStrings, m_activeFileMTime);
    }
    if (m_fallbackLanguage != m_currentLanguage && m_fallbackLanguage != "__keys__") {
        CheckFileForChange(m_fallbackLanguage, m_fallbackStrings, m_fallbackFileMTime);
    }
}

void LocalizationManager::CheckFileForChange(const std::string& code, std::unordered_map<std::string, std::string>& targetMap, uint64_t& recordedMTime)
{
    std::string relPath = "assets/locales/" + code + ".json";
    std::string phys = m_resources->GetPhysicalPath(relPath);
    if (phys.empty()) {
        return;
    }

    std::error_code ec;
    auto currentMTime = std::filesystem::last_write_time(phys, ec);
    if (ec) {
        return;
    }

    uint64_t mtimeVal = static_cast<uint64_t>(currentMTime.time_since_epoch().count());
    if (recordedMTime == 0) {
        recordedMTime = mtimeVal;
        return;
    }
    if (mtimeVal == recordedMTime) {
        return;
    }

    recordedMTime = mtimeVal;
    if (!LoadLanguage(code, targetMap)) {
        return;
    }
    ++m_revision;
    if (m_currentLanguage == m_fallbackLanguage) {
        if (&targetMap == &m_currentStrings) {
            m_fallbackStrings = m_currentStrings;
        } else {
            m_currentStrings = m_fallbackStrings;
        }
    }
    LogWrite(LogLevel::Info, "loc", "Reloaded locale '" + code + "' (" + relPath + ")");
}
#endif

} // namespace IYI
