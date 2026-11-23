// Declares the core localization subsystem for IYIEngine.
// Loads, caches, and resolves localized strings from JSON locale files (assets/locales/<code>.json).
// Provides placeholder formatting (${name}), fallback language resolution, missing-key tracking,
// and development hot-reloading.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace IYI {

class ResourceStore;
class AssetManager;

struct LocaleInfo
{
    std::string Code;
    std::string Name;
    std::string NativeName;
};

class LocalizationManager
{
public:
    static LocalizationManager& Get();

    LocalizationManager(const LocalizationManager&) = delete;
    LocalizationManager& operator=(const LocalizationManager&) = delete;

    void Initialize(AssetManager* assets, const std::string& fallback = "en");

    void SetLanguage(const std::string& code);
    const std::string& GetLanguage() const;

    void SetFallbackLanguage(const std::string& code);
    const std::string& GetFallbackLanguage() const;

    std::string Text(const std::string& key, const std::unordered_map<std::string, std::string>& args = {}) const;
    bool Has(const std::string& key) const;

    const std::vector<LocaleInfo>& Languages() const;
    const LocaleInfo* GetLanguageInfo(const std::string& code) const;

    const std::unordered_set<std::string>& MissingKeys() const;
    void Reload();
    // Grows whenever a text could change (language, fallback, reload, hot-reloaded file): UI text caches its resolved string by it.
    uint64_t Revision() const { return m_revision; }

#ifndef IYI_SHIPPING_BUILD
    void PollForChanges(float dt);
#endif

private:
    LocalizationManager() = default;
    ~LocalizationManager() = default;

    void DiscoverLanguages();
    bool LoadLanguage(const std::string& code, std::unordered_map<std::string, std::string>& outStrings);
    std::string FormatPlaceholders(const std::string& key, const std::string& text, const std::unordered_map<std::string, std::string>& args) const;

#ifndef IYI_SHIPPING_BUILD
    void CheckFileForChange(const std::string& code, std::unordered_map<std::string, std::string>& targetMap, uint64_t& recordedMTime);
#endif

    ResourceStore* m_resources = nullptr;
    uint64_t m_revision = 0;
    std::string m_currentLanguage = "en";
    std::string m_fallbackLanguage = "en";

    std::vector<LocaleInfo> m_languages;
    std::unordered_map<std::string, std::string> m_currentStrings;
    std::unordered_map<std::string, std::string> m_fallbackStrings;

    mutable std::unordered_set<std::string> m_missingKeys;
    mutable std::unordered_set<std::string> m_loggedMissingKeys;
    mutable std::unordered_set<std::string> m_warnedArgs;

#ifndef IYI_SHIPPING_BUILD
    float m_pollTimer = 0.0f;
    uint64_t m_activeFileMTime = 0;
    uint64_t m_fallbackFileMTime = 0;
#endif
};

} // namespace IYI
