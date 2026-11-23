// Declares the abstract resource storage layer for reading project-relative assets and data files.
// Provides a unified read-only abstraction (Exists, ReadBytes, ReadText) with normalized forward-slash paths (packaged IDs use a lowercase canonical key).
// Keeps public engine headers free of implementation details, archive libraries, and third-party dependencies.
//
// Connected to:
// - Owned by AssetManager (and accessed during ProjectConfig loading in Application::Initialize).
// - Subclassed by DevResourceBackend (which reads directly from the filesystem project directory in development,
//   then from each project.json "library" folder for a path the project lacks: the project always wins).
// - Subclassed by PackagedResourceBackend, which reads game.rres in shipping builds (no loose-file fallback).

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace IYI {

// Shipping data folder: <exe dir>/<Name>_Data, or Contents/Resources/<Name>_Data inside a macOS .app.
std::filesystem::path GetDataDir();

class IResourceBackend
{
public:
    virtual ~IResourceBackend() = default;

    virtual bool Exists(const std::string& relPath) const = 0;
    virtual bool ReadBytes(const std::string& relPath, std::vector<unsigned char>& outBytes) const = 0;
    virtual bool ReadText(const std::string& relPath, std::string& outText) const = 0;
    virtual std::string GetPhysicalPath(const std::string& relPath) const { (void)relPath; return ""; }
};

class DevResourceBackend : public IResourceBackend
{
public:
    // libraryDirs: further roots searched in order when the project lacks a path (project.json
    // "libraries"); the project always wins.
    explicit DevResourceBackend(const std::string& projectDir, const std::vector<std::string>& libraryDirs = {});

    bool Exists(const std::string& relPath) const override;
    bool ReadBytes(const std::string& relPath, std::vector<unsigned char>& outBytes) const override;
    bool ReadText(const std::string& relPath, std::string& outText) const override;
    std::string GetPhysicalPath(const std::string& relPath) const override;

    const std::string& GetProjectDir() const { return m_projectDir; }

private:
    std::filesystem::path Locate(const std::string& relPath) const;

    std::string m_projectDir;
    std::vector<std::string> m_libraryDirs;
};

class PackagedResourceBackend : public IResourceBackend
{
public:
    explicit PackagedResourceBackend(const std::string& archivePath = "");
    ~PackagedResourceBackend() override;

    bool Exists(const std::string& relPath) const override;
    bool ReadBytes(const std::string& relPath, std::vector<unsigned char>& outBytes) const override;
    bool ReadText(const std::string& relPath, std::string& outText) const override;
    std::string GetPhysicalPath(const std::string& relPath) const override;

    const std::string& GetArchivePath() const { return m_archivePath; }
    bool IsValid() const { return m_valid; }

private:
    void ScanArchive();
    // The decryption key for an archive salt (iyipack uses one per archive). Argon2 costs ~27 ms, so
    // the key is derived once and kept; null when it cannot be derived (no release secret).
    const unsigned char* KeyForSalt(const unsigned char* salt) const;

    std::string m_archivePath;
    std::unordered_map<uint32_t, long> m_chunkOffsets;   // resource id -> its chunk info header in the archive
    bool m_valid = false;
    mutable unsigned char m_keySalt[16] = {};
    mutable unsigned char m_key[32] = {};
    mutable bool m_hasKey = false;
    mutable std::mutex m_keyMutex;
};

class ResourceStore
{
public:
    ResourceStore();
    explicit ResourceStore(std::unique_ptr<IResourceBackend> backend);

    ResourceStore(const ResourceStore&) = delete;
    ResourceStore& operator=(const ResourceStore&) = delete;
    ResourceStore(ResourceStore&&) noexcept = default;
    ResourceStore& operator=(ResourceStore&&) noexcept = default;

    void SetBackend(std::unique_ptr<IResourceBackend> backend);
    IResourceBackend* GetBackend() const { return m_backend.get(); }
    bool HasBackend() const { return m_backend != nullptr; }

    bool Exists(const std::string& relPath) const;
    bool ReadBytes(const std::string& relPath, std::vector<unsigned char>& outBytes) const;
    bool ReadText(const std::string& relPath, std::string& outText) const;
    std::string GetPhysicalPath(const std::string& relPath) const;

private:
    std::unique_ptr<IResourceBackend> m_backend;
};

} // namespace IYI
