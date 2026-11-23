// Implements the development resource backend and ResourceStore path normalization.
// Normalizes resource paths to project-relative forward-slash form (case preserved); packaged IDs use the lowercase ResourceKey.
// In development mode, DevResourceBackend reads directly from the specified project directory on the filesystem.
//
// Connected to:
// - Implements ResourceStore and DevResourceBackend declared in include/IYI/ResourceStore.h.
// - Used by AssetManager to load textures, fonts, shaders, sounds, and music streams.
// - Used by ProjectConfig to read project.json during engine initialization.

#include "IYI/ResourceStore.h"

#include <mutex>
#include "IYI/ResourcePath.h"
#include "IYI/Log.h"
#include "MobilePaths.h"
#ifdef IYI_PLATFORM_ANDROID
#include <android/asset_manager.h>
#endif
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void* hModule, wchar_t* lpFilename, unsigned long nSize);
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4390 4456 4244)
#endif
#define RRES_SUPPORT_LOG_INFO 0
#include "raylib.h"
#define RRES_IMPLEMENTATION
#include "rres.h"
#include "monocypher.h"
#define RRES_RAYLIB_IMPLEMENTATION
#define RRES_SUPPORT_ENCRYPTION_XCHACHA20
#include "rres-raylib.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#ifdef IYI_SHIPPING_BUILD
#include "IYI/ReleaseSecret.h"
#endif

namespace IYI {

namespace fs = std::filesystem;

static fs::path GetExecutablePath()
{
#ifdef _WIN32
    wchar_t buffer[260];
    unsigned long len = GetModuleFileNameW(nullptr, buffer, 260);
    if (len > 0 && len < 260) {
        return fs::path(buffer);
    }
#elif defined(__linux__)
    char buffer[1024];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        buffer[len] = '\0';
        return fs::path(buffer);
    }
#elif defined(__APPLE__)
    char buffer[1024];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        return fs::path(buffer);
    }
#endif
    return fs::current_path();
}

static fs::path GetExecutableDir()
{
    return GetExecutablePath().parent_path();
}

fs::path GetDataDir()
{
#ifdef IYI_PLATFORM_ANDROID
    return {}; // APK assets have no filesystem directory.
#elif defined(IYI_MOBILE)
    return GetExecutableDir();
#else
    fs::path exePath = GetExecutablePath();
    fs::path exeDir = exePath.parent_path();
    std::string stem = exePath.stem().string();
    std::error_code ec;
#ifdef __APPLE__
    // .app bundle: Contents/MacOS/<Name> -> Contents/Resources/<Name>_Data
    fs::path bundleData = exeDir.parent_path() / "Resources" / (stem + "_Data");
    if (fs::is_directory(bundleData, ec)) {
        return bundleData;
    }
#endif
    fs::path candidate = exeDir / (stem + "_Data");
    if (fs::exists(candidate, ec) && fs::is_directory(candidate, ec)) {
        return candidate;
    }
    // Fallback: search for any directory in exeDir ending in "_Data"
    for (const auto& entry : fs::directory_iterator(exeDir, ec)) {
        if (entry.is_directory(ec)) {
            std::string name = entry.path().filename().string();
            if (name.size() > 5 && name.substr(name.size() - 5) == "_Data") {
                return entry.path();
            }
        }
    }
    return candidate;
#endif
}

// ---------------------------------------------------------------------------
// DevResourceBackend
// ---------------------------------------------------------------------------

DevResourceBackend::DevResourceBackend(const std::string& projectDir, const std::vector<std::string>& libraryDirs)
    : m_projectDir(projectDir), m_libraryDirs(libraryDirs)
{
    for (char& c : m_projectDir) {
        if (c == '\\') c = '/';
    }
    // Trailing slashes go, but never the one that makes a drive root ("C:/" is not the relative "C:").
    while (m_projectDir.size() > 1 && m_projectDir.back() == '/' && m_projectDir[m_projectDir.size() - 2] != ':') {
        m_projectDir.pop_back();
    }
}

// Every DevResourceBackend read resolves through here: the project first, then each library root in
// order, the first existing file wins. Empty when the path leaves the root (ResourceContainedPath), so
// all four entry points refuse it the same way the packaged backend would, or when it exists nowhere.
fs::path DevResourceBackend::Locate(const std::string& relPath) const
{
    std::error_code ec;
    fs::path full = ResourceContainedPath(m_projectDir, relPath);
    if (full.empty()) {
        return {};
    }
    if (fs::exists(full, ec) && !fs::is_directory(full, ec)) {
        return full;
    }
    for (const std::string& root : m_libraryDirs) {
        fs::path lib = ResourceContainedPath(root, relPath);
        if (!lib.empty() && fs::exists(lib, ec) && !fs::is_directory(lib, ec)) {
            return lib;
        }
    }
    return {};
}

bool DevResourceBackend::Exists(const std::string& relPath) const
{
    return !Locate(relPath).empty();
}

bool DevResourceBackend::ReadBytes(const std::string& relPath, std::vector<unsigned char>& outBytes) const
{
    outBytes.clear();
    fs::path full = Locate(relPath);
    if (full.empty()) {
        return false;
    }

    std::ifstream file(full, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }

    std::streamsize size = file.tellg();
    if (size < 0) {
        return false;
    }

    file.seekg(0, std::ios::beg);
    outBytes.resize(static_cast<size_t>(size));
    if (size > 0) {
        if (!file.read(reinterpret_cast<char*>(outBytes.data()), size)) {
            outBytes.clear();
            return false;
        }
    }
    return true;
}

bool DevResourceBackend::ReadText(const std::string& relPath, std::string& outText) const
{
    outText.clear();
    fs::path full = Locate(relPath);
    if (full.empty()) {
        return false;
    }

    std::ifstream file(full, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    std::string str((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    outText = std::move(str);
    return true;
}

std::string DevResourceBackend::GetPhysicalPath(const std::string& relPath) const
{
    fs::path full = Locate(relPath);
    return full.empty() ? "" : full.generic_string();
}

// ---------------------------------------------------------------------------
// PackagedResourceBackend
// ---------------------------------------------------------------------------

// Each read owns its source, just as each desktop read owns its FILE*.
class ArchiveSource {
public:
    explicit ArchiveSource(const std::string& path) {
#ifdef IYI_PLATFORM_ANDROID
        if (auto* assets = MobileAssets()) m_asset = AAssetManager_open(assets, path.c_str(), AASSET_MODE_RANDOM);
#else
        m_file = fopen(path.c_str(), "rb");
#endif
    }
    ~ArchiveSource() {
#ifdef IYI_PLATFORM_ANDROID
        if (m_asset) AAsset_close(m_asset);
#else
        if (m_file) fclose(m_file);
#endif
    }
    explicit operator bool() const {
#ifdef IYI_PLATFORM_ANDROID
        return m_asset != nullptr;
#else
        return m_file != nullptr;
#endif
    }
    long Tell() {
#ifdef IYI_PLATFORM_ANDROID
        return static_cast<long>(AAsset_seek64(m_asset, 0, SEEK_CUR));
#else
        return ftell(m_file);
#endif
    }
    bool Seek(long offset, int origin) {
#ifdef IYI_PLATFORM_ANDROID
        return AAsset_seek64(m_asset, offset, origin) >= 0;
#else
        return fseek(m_file, offset, origin) == 0;
#endif
    }
    size_t Read(void* data, size_t size) {
#ifdef IYI_PLATFORM_ANDROID
        size_t total = 0;
        const auto remaining = AAsset_getLength64(m_asset) - AAsset_seek64(m_asset, 0, SEEK_CUR);
        if (remaining < 0) return 0;
        size = std::min(size, static_cast<size_t>(remaining));
        while (total < size) {
            size_t count = std::min(size - total, static_cast<size_t>(std::numeric_limits<int>::max()));
            int n = AAsset_read(m_asset, static_cast<unsigned char*>(data) + total, count);
            if (n <= 0) break;
            total += static_cast<size_t>(n);
        }
        return total;
#else
        return fread(data, 1, size, m_file);
#endif
    }
private:
#ifdef IYI_PLATFORM_ANDROID
    AAsset* m_asset = nullptr;
#else
    FILE* m_file = nullptr;
#endif
};

PackagedResourceBackend::PackagedResourceBackend(const std::string& archivePath)
{
#ifdef IYI_PLATFORM_ANDROID
    m_archivePath = archivePath.empty() ? "game.rres" : archivePath;
#else
    if (!archivePath.empty()) {
        m_archivePath = archivePath;
    } else {
        fs::path dataDir = GetDataDir();
        fs::path candidate = dataDir / "game.rres";
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            m_archivePath = candidate.lexically_normal().string();
        } else {
            fs::path exeDir = GetExecutableDir();
            if (fs::exists(exeDir / "game.rres", ec)) {
                m_archivePath = (exeDir / "game.rres").lexically_normal().string();
            } else {
                m_archivePath = candidate.lexically_normal().string();
            }
        }
    }
#endif
    for (char& c : m_archivePath) {
        if (c == '\\') c = '/';
    }
    ScanArchive();
}

PackagedResourceBackend::~PackagedResourceBackend()
{
    crypto_wipe(m_key, sizeof(m_key));
}

const unsigned char* PackagedResourceBackend::KeyForSalt(const unsigned char* salt) const
{
    std::lock_guard<std::mutex> lock(m_keyMutex);   // game and audio side both read the archive
    if (m_hasKey && std::memcmp(salt, m_keySalt, sizeof(m_keySalt)) == 0) {
        return m_key;
    }
#ifdef IYI_SHIPPING_BUILD
    ReleaseSecret secret;
    if (secret.IsEmpty()) {
        return nullptr;
    }
    // Same parameters as iyipack and rres-raylib: Argon2i, 16 MB, 3 passes, one lane.
    crypto_argon2_config config = { CRYPTO_ARGON2_I, 16384, 3, 1 };
    crypto_argon2_inputs inputs = { reinterpret_cast<const uint8_t*>(secret.Get()), salt,
                                    static_cast<uint32_t>(secret.Size()), 16 };
    std::vector<unsigned char> workArea(static_cast<size_t>(config.nb_blocks) * 1024);
    crypto_argon2(m_key, sizeof(m_key), workArea.data(), config, inputs, crypto_argon2_no_extras);
    secret.Wipe();
    std::memcpy(m_keySalt, salt, sizeof(m_keySalt));
    m_hasKey = true;
    return m_key;
#else
    return nullptr;
#endif
}

// iyipack chunk trailer after the cipher text: salt[16] nonce[24] mac[16].
static constexpr unsigned int kCipherTrailer = 16 + 24 + 16;

// Turns a packed iyipack chunk into the file's bytes: decrypt (XChaCha20-Poly1305, in place),
// inflate (DEFLATE), then drop the RRES_DATA_RAW header (propCount, props[4]; props[0] = file size).
// Replaces rres-raylib's UnpackResourceChunk, which runs Argon2 again for every chunk (~27 ms each,
// ~7 s for a 250-file startup); the key comes from KeyForSalt instead.
static bool UnpackChunk(rresResourceChunk& chunk, const unsigned char* key, std::vector<unsigned char>& outBytes)
{
    unsigned char* data = static_cast<unsigned char*>(chunk.data.raw);
    unsigned int size = chunk.info.packedSize;
    if (chunk.info.cipherType != RRES_CIPHER_NONE) {
        if (chunk.info.cipherType != RRES_CIPHER_XCHACHA20_POLY1305 || !key || size < kCipherTrailer) {
            return false;
        }
        size -= kCipherTrailer;
        const unsigned char* nonce = data + size + 16;
        const unsigned char* mac = data + size + 16 + 24;
        if (crypto_aead_unlock(data, mac, key, nonce, nullptr, 0, data, size) != 0) {
            return false;
        }
    }

    unsigned char* inflated = nullptr;
    if (chunk.info.compType == RRES_COMP_DEFLATE) {
        int inflatedSize = 0;
        inflated = DecompressData(data, static_cast<int>(size), &inflatedSize);
        if (!inflated) {
            return false;
        }
        data = inflated;
        size = static_cast<unsigned int>(inflatedSize);
    } else if (chunk.info.compType != RRES_COMP_NONE) {
        return false;
    }

    uint32_t fileSize = 0;
    if (size >= 20) {
        std::memcpy(&fileSize, data + 4, sizeof(fileSize));
    }
    bool ok = size >= 20 && fileSize <= size - 20;
    if (ok) {
        outBytes.assign(data + 20, data + 20 + fileSize);
    }
    MemFree(inflated);
    return ok;
}

void PackagedResourceBackend::ScanArchive()
{
    m_chunkOffsets.clear();
    m_valid = false;

#ifndef IYI_PLATFORM_ANDROID
    std::error_code ec;
    if (!fs::exists(m_archivePath, ec)) {
#ifdef IYI_SHIPPING_BUILD
        LogWrite(LogLevel::Error, "resource", "FATAL: Archive file not found: " + m_archivePath);
#else
        LogWrite(LogLevel::Warn, "resource", "Archive file not found: " + m_archivePath);
#endif
        return;
    }
#endif

    ArchiveSource f(m_archivePath);
    if (!f) {
#ifdef IYI_SHIPPING_BUILD
        LogWrite(LogLevel::Error, "resource", "FATAL: Could not open archive: " + m_archivePath);
#else
        LogWrite(LogLevel::Warn, "resource", "Could not open archive: " + m_archivePath);
#endif
        return;
    }

    rresFileHeader header;
    if (f.Read(&header, sizeof(header)) != sizeof(header)) {
#ifdef IYI_SHIPPING_BUILD
        LogWrite(LogLevel::Error, "resource", "FATAL: Failed to read rres header from " + m_archivePath);
#endif
        return;
    }

    if (header.id[0] != 'r' || header.id[1] != 'r' || header.id[2] != 'e' || header.id[3] != 's' || header.version != 100) {
#ifdef IYI_SHIPPING_BUILD
        LogWrite(LogLevel::Error, "resource", "FATAL: Invalid rres archive signature or version in " + m_archivePath);
#endif
        return;
    }

    for (int i = 0; i < header.chunkCount; ++i) {
        rresResourceChunkInfo info;
        const long offset = f.Tell();
        if (f.Read(&info, sizeof(info)) != sizeof(info)) {
            break;
        }
        m_chunkOffsets.emplace(info.id, offset);   // the first chunk of an id wins, as rresLoadResourceChunk does
        f.Seek(static_cast<long>(info.packedSize), SEEK_CUR);
    }

    m_valid = true;
}

bool PackagedResourceBackend::Exists(const std::string& relPath) const
{
    if (!m_valid) {
        return false;
    }
    std::string norm = ResourceKey(relPath);
    if (norm.empty()) {
        return false;
    }
    uint32_t id = rresComputeCRC32(reinterpret_cast<const unsigned char*>(norm.data()), static_cast<int>(norm.length()));
    return m_chunkOffsets.find(id) != m_chunkOffsets.end();
}

bool PackagedResourceBackend::ReadBytes(const std::string& relPath, std::vector<unsigned char>& outBytes) const
{
    outBytes.clear();
    std::string norm = ResourceKey(relPath);
    if (norm.empty()) {
        return false;
    }

    uint32_t id = rresComputeCRC32(reinterpret_cast<const unsigned char*>(norm.data()), static_cast<int>(norm.length()));
    const auto found = m_valid ? m_chunkOffsets.find(id) : m_chunkOffsets.end();
    if (found == m_chunkOffsets.end()) {
#ifdef IYI_SHIPPING_BUILD
        LogWrite(LogLevel::Error, "resource", "FATAL: Missing resource in archive: '" + relPath + "' (normalized: '" + norm + "')");
#endif
        return false;
    }

    // rresLoadResourceChunk without its rescan: seek to the chunk ScanArchive found.
    rresResourceChunk chunk{};
    if (ArchiveSource f{m_archivePath}) {
        rresResourceChunkInfo info{};
        if (f.Seek(found->second, SEEK_SET) && f.Read(&info, sizeof(info)) == sizeof(info) && info.id == id) {
            std::vector<unsigned char> packed(info.packedSize);
            if (f.Read(packed.data(), packed.size()) == packed.size()) {
                chunk.data = rresLoadResourceChunkData(info, packed.data());
                chunk.info = info;
            }
        }
    }
    if (chunk.info.id != id) {
#ifdef IYI_SHIPPING_BUILD
        LogWrite(LogLevel::Error, "resource", "FATAL: Failed to load chunk for resource: '" + relPath + "'");
#endif
        return false;
    }

    if (chunk.info.cipherType != RRES_CIPHER_NONE || chunk.info.compType != RRES_COMP_NONE) {
        const unsigned char* key = nullptr;
        if (chunk.info.cipherType != RRES_CIPHER_NONE && chunk.info.packedSize >= kCipherTrailer) {
            key = KeyForSalt(static_cast<const unsigned char*>(chunk.data.raw) + chunk.info.packedSize - kCipherTrailer);
        }
        bool ok = UnpackChunk(chunk, key, outBytes);
        if (!ok) {
#ifdef IYI_SHIPPING_BUILD
            LogWrite(LogLevel::Error, "resource", "FATAL: Failed to unpack resource (decryption/decompression): '" + relPath + "'");
#else
            LogWrite(LogLevel::Error, "resource", "Failed to unpack resource: '" + relPath + "'");
#endif
        }
        rresUnloadResourceChunk(chunk);
        return ok;
    }

    if (rresGetDataType(chunk.info.type) == RRES_DATA_RAW) {   // uncompressed raw data: copy it out once
        const unsigned char* p = static_cast<const unsigned char*>(chunk.data.raw);
        if (p && chunk.data.propCount > 0) outBytes.assign(p, p + chunk.data.props[0]);
        rresUnloadResourceChunk(chunk);
        return true;
    }

    unsigned int dataSize = 0;
    void* data = LoadDataFromResource(chunk, &dataSize);
    if (!data && dataSize > 0) {
#ifdef IYI_SHIPPING_BUILD
        LogWrite(LogLevel::Error, "resource", "FATAL: Failed to retrieve raw data for: '" + relPath + "'");
#endif
        rresUnloadResourceChunk(chunk);
        return false;
    }

    if (dataSize > 0 && data != nullptr) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(data);
        outBytes.assign(p, p + dataSize);
        MemFree(data);
    }

    rresUnloadResourceChunk(chunk);
    return true;
}

bool PackagedResourceBackend::ReadText(const std::string& relPath, std::string& outText) const
{
    outText.clear();
    std::vector<unsigned char> bytes;
    if (!ReadBytes(relPath, bytes)) {
        return false;
    }
    outText.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

std::string PackagedResourceBackend::GetPhysicalPath(const std::string& relPath) const
{
    (void)relPath;
    return "";
}

// ---------------------------------------------------------------------------
// ResourceStore
// ---------------------------------------------------------------------------

ResourceStore::ResourceStore()
{
#ifdef IYI_SHIPPING_BUILD
    m_backend = std::make_unique<PackagedResourceBackend>();
#endif
}

ResourceStore::ResourceStore(std::unique_ptr<IResourceBackend> backend)
    : m_backend(std::move(backend))
{
}

void ResourceStore::SetBackend(std::unique_ptr<IResourceBackend> backend)
{
#ifdef IYI_SHIPPING_BUILD
    if (dynamic_cast<PackagedResourceBackend*>(backend.get()) != nullptr) {
        m_backend = std::move(backend);
    } else {
        LogWrite(LogLevel::Error, "resource", "FATAL: Attempted to set non-packaged resource backend in shipping build");
    }
#else
    m_backend = std::move(backend);
#endif
}

bool ResourceStore::Exists(const std::string& relPath) const
{
    if (!m_backend) return false;
    return m_backend->Exists(ResourceFsPath(relPath));
}

bool ResourceStore::ReadBytes(const std::string& relPath, std::vector<unsigned char>& outBytes) const
{
    if (!m_backend) {
#ifdef IYI_SHIPPING_BUILD
        LogWrite(LogLevel::Error, "resource", "FATAL: No resource backend mounted in shipping build for path: " + relPath);
#endif
        return false;
    }
    return m_backend->ReadBytes(ResourceFsPath(relPath), outBytes);
}

bool ResourceStore::ReadText(const std::string& relPath, std::string& outText) const
{
    if (!m_backend) {
#ifdef IYI_SHIPPING_BUILD
        LogWrite(LogLevel::Error, "resource", "FATAL: No resource backend mounted in shipping build for path: " + relPath);
#endif
        return false;
    }
    return m_backend->ReadText(ResourceFsPath(relPath), outText);
}

std::string ResourceStore::GetPhysicalPath(const std::string& relPath) const
{
    if (!m_backend) return "";
    return m_backend->GetPhysicalPath(ResourceFsPath(relPath));
}

} // namespace IYI
