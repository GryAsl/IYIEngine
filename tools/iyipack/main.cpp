// iyipack: Asset and resource packer for IYIEngine.
// Reads a JSON manifest, traverses project assets, normalizes paths, detects CRC32
// collisions, applies per-extension compression and XChaCha20-Poly1305 encryption,
// and packages files into an rres archive with Central Directory omitted.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

#include "raylib.h"

#define RRES_IMPLEMENTATION
#include "rres.h"

#include "monocypher.h"
#include "IYI/ResourcePath.h"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Glob Pattern Matching
// ---------------------------------------------------------------------------

static std::string GlobToRegex(const std::string& glob)
{
    std::string g;
    g.reserve(glob.size());
    for (char c : glob) {
        if (c == '\\') {
            g += '/';
        } else {
            g += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }

    bool hasSlash = (g.find('/') != std::string::npos);

    std::string regexStr = "^";
    if (!hasSlash) {
        regexStr += "(?:.*/)?";
    }

    for (size_t i = 0; i < g.size(); ++i) {
        if (g[i] == '*' && i + 1 < g.size() && g[i + 1] == '*') {
            i++;
            if (i + 1 < g.size() && g[i + 1] == '/') {
                regexStr += "(?:.*/)?";
                i++;
            } else {
                regexStr += ".*";
            }
        } else if (g[i] == '*') {
            regexStr += "[^/]*";
        } else if (g[i] == '?') {
            regexStr += "[^/]";
        } else if (g[i] == '.' || g[i] == '(' || g[i] == ')' || g[i] == '+' ||
                   g[i] == '^' || g[i] == '$' || g[i] == '[' || g[i] == ']' ||
                   g[i] == '{' || g[i] == '}' || g[i] == '|' || g[i] == '\\') {
            regexStr += '\\';
            regexStr += g[i];
        } else {
            regexStr += g[i];
        }
    }
    regexStr += "$";
    return regexStr;
}

struct GlobMatcher {
    std::string pattern;
    std::regex regex;

    explicit GlobMatcher(const std::string& pat)
        : pattern(pat)
        , regex(GlobToRegex(pat), std::regex_constants::ECMAScript | std::regex_constants::optimize)
    {
    }

    bool Matches(const std::string& path) const
    {
        return std::regex_match(path, regex);
    }
};

// ---------------------------------------------------------------------------
// CSPRNG Random Bytes Generator
// ---------------------------------------------------------------------------

static bool GenerateRandomBytes(uint8_t* buffer, size_t size)
{
    std::random_device rd;
    for (size_t i = 0; i < size; ++i) {
        buffer[i] = static_cast<uint8_t>(rd() & 0xFF);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Manifest Configuration
// ---------------------------------------------------------------------------

struct ManifestConfig {
    std::string inputRoot = ".";
    std::string outputFile = "game.rres";
    std::vector<std::string> includeGlobs;
    std::vector<std::string> excludeGlobs;

    std::string compressionDefault = "deflate";
    std::unordered_map<std::string, std::string> compressionExtensions;

    bool encryptionEnabled = false;
    std::string encryptionKeyEnv;
    std::string encryptionKeyFile;
};

// ---------------------------------------------------------------------------
// File and Chunk Structures
// ---------------------------------------------------------------------------

struct FileCandidate {
    std::string normRelPath;
    fs::path diskPath;
    std::string extension;
    uint32_t id = 0;
};

struct BuiltChunk {
    rresResourceChunkInfo info;
    std::vector<uint8_t> data;
};

// ---------------------------------------------------------------------------
// Key Management
// ---------------------------------------------------------------------------

static bool LoadEncryptionKey(const std::string& keyEnv, const std::string& keyFile, std::string& outKey)
{
    if (!keyFile.empty()) {
        std::ifstream f(keyFile, std::ios::binary);
        if (!f.is_open()) {
            std::cerr << "[iyipack] ERROR: Could not open key file: " << keyFile << "\n";
            return false;
        }
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        while (!content.empty() && (content.back() == '\r' || content.back() == '\n' || content.back() == ' ' || content.back() == '\t')) {
            content.pop_back();
        }
        if (content.empty()) {
            std::cerr << "[iyipack] ERROR: Key file is empty: " << keyFile << "\n";
            return false;
        }
        outKey = std::move(content);
        return true;
    }

    std::string envName = keyEnv.empty() ? "IYI_ENCRYPTION_KEY" : keyEnv;
    const char* envVal = std::getenv(envName.c_str());
    if (!envVal || std::strlen(envVal) == 0) {
        std::cerr << "[iyipack] ERROR: Encryption key environment variable '" << envName << "' is not set or empty.\n";
        return false;
    }
    outKey = envVal;
    while (!outKey.empty() && (outKey.back() == '\r' || outKey.back() == '\n')) {
        outKey.pop_back();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Usage Display
// ---------------------------------------------------------------------------

static void PrintUsage(const char* progName)
{
    std::cout << "Usage: " << progName << " [options] <manifest.json>\n\n"
              << "Options:\n"
              << "  -m, --manifest <file>  Path to packing manifest JSON\n"
              << "  -i, --input <dir>      Override input root directory\n"
              << "  -o, --output <file>    Override output .rres archive path\n"
              << "  --key-env <VAR>        Read encryption key from environment variable\n"
              << "  --key-file <FILE>      Read encryption key from file\n"
              << "  --encrypt              Force enable encryption\n"
              << "  --no-encrypt           Force disable encryption\n"
              << "  -h, --help             Show this help information\n\n"
              << "Security Note:\n"
              << "  Keys cannot be passed as plain CLI arguments. Use --key-env or --key-file.\n";
}

// ---------------------------------------------------------------------------
// Main Implementation
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    std::string manifestPath;
    std::string inputRootOverride;
    std::string outputFileOverride;
    std::string keyEnvOverride;
    std::string keyFileOverride;
    int forceEncrypt = -1; // -1 = manifest default, 0 = false, 1 = true

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else if (arg == "--key" || arg == "-k") {
            std::cerr << "[iyipack] ERROR: Supplying an encryption key directly as a CLI argument is forbidden for security.\n"
                      << "Please provide the key via --key-env <ENV_VAR> or --key-file <PATH>.\n";
            return 1;
        } else if (arg == "--manifest" || arg == "-m") {
            if (i + 1 >= argc) {
                std::cerr << "[iyipack] ERROR: Missing argument for " << arg << "\n";
                return 1;
            }
            manifestPath = argv[++i];
        } else if (arg == "--input" || arg == "-i") {
            if (i + 1 >= argc) {
                std::cerr << "[iyipack] ERROR: Missing argument for " << arg << "\n";
                return 1;
            }
            inputRootOverride = argv[++i];
        } else if (arg == "--output" || arg == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "[iyipack] ERROR: Missing argument for " << arg << "\n";
                return 1;
            }
            outputFileOverride = argv[++i];
        } else if (arg == "--key-env") {
            if (i + 1 >= argc) {
                std::cerr << "[iyipack] ERROR: Missing argument for " << arg << "\n";
                return 1;
            }
            keyEnvOverride = argv[++i];
        } else if (arg == "--key-file") {
            if (i + 1 >= argc) {
                std::cerr << "[iyipack] ERROR: Missing argument for " << arg << "\n";
                return 1;
            }
            keyFileOverride = argv[++i];
        } else if (arg == "--encrypt") {
            forceEncrypt = 1;
        } else if (arg == "--no-encrypt") {
            forceEncrypt = 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "[iyipack] ERROR: Unknown option: " << arg << "\n";
            PrintUsage(argv[0]);
            return 1;
        } else {
            if (manifestPath.empty()) {
                manifestPath = arg;
            } else {
                std::cerr << "[iyipack] ERROR: Unexpected extra argument: " << arg << "\n";
                return 1;
            }
        }
    }

    if (manifestPath.empty()) {
        std::cerr << "[iyipack] ERROR: No manifest specified.\n";
        PrintUsage(argv[0]);
        return 1;
    }

    fs::path manifestFsPath(manifestPath);
    if (!fs::exists(manifestFsPath)) {
        std::cerr << "[iyipack] ERROR: Manifest file not found: " << manifestPath << "\n";
        return 1;
    }

    fs::path manifestDir = manifestFsPath.parent_path();
    if (manifestDir.empty()) {
        manifestDir = ".";
    }

    std::ifstream manifestFile(manifestFsPath);
    if (!manifestFile.is_open()) {
        std::cerr << "[iyipack] ERROR: Failed to open manifest: " << manifestPath << "\n";
        return 1;
    }

    json manifestJson;
    try {
        manifestFile >> manifestJson;
    } catch (const std::exception& ex) {
        std::cerr << "[iyipack] ERROR: Failed to parse manifest JSON: " << ex.what() << "\n";
        return 1;
    }

    ManifestConfig cfg;

    if (manifestJson.contains("input_root")) {
        cfg.inputRoot = manifestJson["input_root"].get<std::string>();
    } else if (manifestJson.contains("input")) {
        cfg.inputRoot = manifestJson["input"].get<std::string>();
    }

    if (manifestJson.contains("output_file")) {
        cfg.outputFile = manifestJson["output_file"].get<std::string>();
    } else if (manifestJson.contains("output")) {
        cfg.outputFile = manifestJson["output"].get<std::string>();
    }

    if (manifestJson.contains("include") && manifestJson["include"].is_array()) {
        for (const auto& item : manifestJson["include"]) {
            if (item.is_string()) {
                cfg.includeGlobs.push_back(item.get<std::string>());
            }
        }
    }
    if (cfg.includeGlobs.empty()) {
        cfg.includeGlobs.push_back("**/*");
    }

    if (manifestJson.contains("exclude") && manifestJson["exclude"].is_array()) {
        for (const auto& item : manifestJson["exclude"]) {
            if (item.is_string()) {
                cfg.excludeGlobs.push_back(item.get<std::string>());
            }
        }
    }

    if (manifestJson.contains("compression")) {
        const auto& comp = manifestJson["compression"];
        if (comp.is_string()) {
            cfg.compressionDefault = comp.get<std::string>();
        } else if (comp.is_object()) {
            if (comp.contains("default") && comp["default"].is_string()) {
                cfg.compressionDefault = comp["default"].get<std::string>();
            }
            if (comp.contains("extensions") && comp["extensions"].is_object()) {
                for (auto it = comp["extensions"].begin(); it != comp["extensions"].end(); ++it) {
                    std::string extKey = it.key();
                    if (!extKey.empty() && extKey[0] != '.') {
                        extKey = "." + extKey;
                    }
                    for (char& c : extKey) {
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                    cfg.compressionExtensions[extKey] = it.value().get<std::string>();
                }
            }
        }
    }

    if (manifestJson.contains("encryption")) {
        const auto& enc = manifestJson["encryption"];
        if (enc.is_boolean()) {
            cfg.encryptionEnabled = enc.get<bool>();
        } else if (enc.is_object()) {
            if (enc.contains("enabled") && enc["enabled"].is_boolean()) {
                cfg.encryptionEnabled = enc["enabled"].get<bool>();
            }
            if (enc.contains("key_env") && enc["key_env"].is_string()) {
                cfg.encryptionKeyEnv = enc["key_env"].get<std::string>();
            }
            if (enc.contains("key_file") && enc["key_file"].is_string()) {
                cfg.encryptionKeyFile = enc["key_file"].get<std::string>();
            }
        }
    }

    // Apply CLI overrides
    if (!inputRootOverride.empty()) {
        cfg.inputRoot = inputRootOverride;
    }
    if (!outputFileOverride.empty()) {
        cfg.outputFile = outputFileOverride;
    }
    if (!keyEnvOverride.empty()) {
        cfg.encryptionKeyEnv = keyEnvOverride;
    }
    if (!keyFileOverride.empty()) {
        cfg.encryptionKeyFile = keyFileOverride;
    }
    if (forceEncrypt != -1) {
        cfg.encryptionEnabled = (forceEncrypt == 1);
    }

    // Resolve input directory relative to manifest directory if not absolute
    fs::path inputRootPath(cfg.inputRoot);
    if (inputRootPath.is_relative()) {
        if (fs::exists(manifestDir / inputRootPath)) {
            inputRootPath = manifestDir / inputRootPath;
        }
    }

    if (!fs::exists(inputRootPath) || !fs::is_directory(inputRootPath)) {
        std::cerr << "[iyipack] ERROR: Input root directory does not exist: " << inputRootPath.string() << "\n";
        return 1;
    }

    // Resolve output path
    fs::path outputPath(cfg.outputFile);
    if (outputPath.is_relative()) {
        outputPath = manifestDir / outputPath;
    }

    // Prepare encryption key if enabled
    std::string encryptionKey;
    if (cfg.encryptionEnabled) {
        std::string resolvedKeyFile = cfg.encryptionKeyFile;
        if (!resolvedKeyFile.empty() && fs::path(resolvedKeyFile).is_relative()) {
            if (fs::exists(manifestDir / resolvedKeyFile)) {
                resolvedKeyFile = (manifestDir / resolvedKeyFile).string();
            }
        }
        if (!LoadEncryptionKey(cfg.encryptionKeyEnv, resolvedKeyFile, encryptionKey)) {
            return 1;
        }
    }

    // One salt and one Argon2 derivation per archive, so the runtime derives the key once instead of
    // once per file (~27 ms each). Every chunk still gets its own random nonce. From here on
    // encryptionKey holds the derived 32-byte key; the passphrase is wiped.
    uint8_t archiveSalt[16] = { 0 };
    if (cfg.encryptionEnabled) {
        GenerateRandomBytes(archiveSalt, sizeof(archiveSalt));
        uint8_t derivedKey[32] = { 0 };
        crypto_argon2_config argonConfig = {
            .algorithm = CRYPTO_ARGON2_I,
            .nb_blocks = 16384, // 16 MB
            .nb_passes = 3,
            .nb_lanes = 1
        };
        crypto_argon2_inputs argonInputs = {
            .pass = reinterpret_cast<const uint8_t*>(encryptionKey.data()),
            .salt = archiveSalt,
            .pass_size = static_cast<uint32_t>(encryptionKey.size()),
            .salt_size = 16
        };
        crypto_argon2_extras argonExtras = { 0 };
        void* workArea = std::malloc(argonConfig.nb_blocks * 1024);
        if (!workArea) {
            std::cerr << "[iyipack] ERROR: Failed to allocate Argon2 work area (16 MB)\n";
            crypto_wipe(encryptionKey.data(), encryptionKey.size());
            return 1;
        }
        crypto_argon2(derivedKey, 32, workArea, argonConfig, argonInputs, argonExtras);
        std::free(workArea);
        crypto_wipe(encryptionKey.data(), encryptionKey.size());
        encryptionKey.assign(reinterpret_cast<const char*>(derivedKey), sizeof(derivedKey));
        crypto_wipe(derivedKey, sizeof(derivedKey));
    }

    // Prepare glob matchers
    std::vector<GlobMatcher> includeMatchers;
    for (const auto& pat : cfg.includeGlobs) {
        includeMatchers.emplace_back(pat);
    }
    std::vector<GlobMatcher> excludeMatchers;
    for (const auto& pat : cfg.excludeGlobs) {
        excludeMatchers.emplace_back(pat);
    }

    // Discover matching files
    std::vector<FileCandidate> candidates;
    for (const auto& entry : fs::recursive_directory_iterator(inputRootPath, fs::directory_options::follow_directory_symlink)) {
        if (entry.is_directory()) {
            continue;
        }

        fs::path relPathFs = fs::relative(entry.path(), inputRootPath);
        std::string normRelPath = IYI::ResourceKey(relPathFs.generic_string());
        if (normRelPath.empty()) {
            continue;
        }

        // Check exclusions
        bool excluded = false;
        for (const auto& matcher : excludeMatchers) {
            if (matcher.Matches(normRelPath)) {
                excluded = true;
                break;
            }
        }
        if (excluded) {
            continue;
        }

        // Check inclusions
        bool included = false;
        for (const auto& matcher : includeMatchers) {
            if (matcher.Matches(normRelPath)) {
                included = true;
                break;
            }
        }
        if (!included) {
            continue;
        }

        FileCandidate cand;
        cand.normRelPath = normRelPath;
        cand.diskPath = entry.path();

        size_t dotPos = normRelPath.rfind('.');
        size_t slashPos = normRelPath.rfind('/');
        if (dotPos != std::string::npos && (slashPos == std::string::npos || dotPos > slashPos)) {
            cand.extension = normRelPath.substr(dotPos);
        }

        cand.id = rresComputeCRC32(reinterpret_cast<const unsigned char*>(normRelPath.data()), static_cast<int>(normRelPath.length()));
        candidates.push_back(cand);
    }

    // Check for CRC32 collisions
    std::unordered_map<uint32_t, std::string> idMap;
    for (const auto& cand : candidates) {
        auto it = idMap.find(cand.id);
        if (it != idMap.end()) {
            std::cerr << "[iyipack] ERROR: Resource ID collision detected!\n"
                      << "  ID: 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << cand.id << std::dec << "\n"
                      << "  File 1: " << it->second << "\n"
                      << "  File 2: " << cand.normRelPath << "\n";
            if (!encryptionKey.empty()) {
                crypto_wipe(encryptionKey.data(), encryptionKey.size());
            }
            return 1;
        }
        idMap[cand.id] = cand.normRelPath;
    }

    // Sort deterministically by normalized relative path
    std::sort(candidates.begin(), candidates.end(), [](const FileCandidate& a, const FileCandidate& b) {
        return a.normRelPath < b.normRelPath;
    });

    if (candidates.size() > 65535) {
        std::cerr << "[iyipack] ERROR: Resource count exceeds rres format limit of 65535 chunks ("
                  << candidates.size() << " files found)\n";
        if (!encryptionKey.empty()) {
            crypto_wipe(encryptionKey.data(), encryptionKey.size());
        }
        return 1;
    }

    std::cout << "[iyipack] Packaging " << candidates.size() << " files into " << outputPath.string() << "\n";
    std::cout << "[iyipack] Compression default: " << cfg.compressionDefault << "\n";
    std::cout << "[iyipack] Encryption: " << (cfg.encryptionEnabled ? "ENABLED (XChaCha20-Poly1305)" : "DISABLED") << "\n";

    // Build chunks
    std::vector<BuiltChunk> builtChunks;
    builtChunks.reserve(candidates.size());

    for (const auto& cand : candidates) {
        std::ifstream fileStream(cand.diskPath, std::ios::binary);
        if (!fileStream.is_open()) {
            std::cerr << "[iyipack] ERROR: Failed to read file: " << cand.diskPath.string() << "\n";
            if (!encryptionKey.empty()) {
                crypto_wipe(encryptionKey.data(), encryptionKey.size());
            }
            return 1;
        }

        std::vector<uint8_t> fileBytes((std::istreambuf_iterator<char>(fileStream)), std::istreambuf_iterator<char>());
        uint32_t rawSize = static_cast<uint32_t>(fileBytes.size());

        // Extension properties (extension01 and extension02, big-endian)
        uint32_t ext01 = 0;
        uint32_t ext02 = 0;
        if (!cand.extension.empty()) {
            char extBuf[8] = { 0 };
            for (size_t i = 0; i < cand.extension.size() && i < 8; ++i) {
                extBuf[i] = cand.extension[i];
            }
            ext01 = (static_cast<uint32_t>(static_cast<unsigned char>(extBuf[0])) << 24) |
                    (static_cast<uint32_t>(static_cast<unsigned char>(extBuf[1])) << 16) |
                    (static_cast<uint32_t>(static_cast<unsigned char>(extBuf[2])) << 8)  |
                    (static_cast<uint32_t>(static_cast<unsigned char>(extBuf[3])));
            ext02 = (static_cast<uint32_t>(static_cast<unsigned char>(extBuf[4])) << 24) |
                    (static_cast<uint32_t>(static_cast<unsigned char>(extBuf[5])) << 16) |
                    (static_cast<uint32_t>(static_cast<unsigned char>(extBuf[6])) << 8)  |
                    (static_cast<uint32_t>(static_cast<unsigned char>(extBuf[7])));
        }

        // RRES_DATA_RAW layout: propCount (4) + props[4] (16) + raw bytes
        uint32_t baseSize = 20 + rawSize;
        std::vector<uint8_t> uncompChunk(baseSize);

        uint32_t propCount = 4;
        uint32_t props[4] = { rawSize, ext01, ext02, 0 };
        std::memcpy(uncompChunk.data() + 0, &propCount, sizeof(uint32_t));
        std::memcpy(uncompChunk.data() + 4, &props[0], sizeof(uint32_t));
        std::memcpy(uncompChunk.data() + 8, &props[1], sizeof(uint32_t));
        std::memcpy(uncompChunk.data() + 12, &props[2], sizeof(uint32_t));
        std::memcpy(uncompChunk.data() + 16, &props[3], sizeof(uint32_t));
        if (rawSize > 0) {
            std::memcpy(uncompChunk.data() + 20, fileBytes.data(), rawSize);
        }

        // Compression policy
        std::string policy = cfg.compressionDefault;
        auto policyIt = cfg.compressionExtensions.find(cand.extension);
        if (policyIt != cfg.compressionExtensions.end()) {
            policy = policyIt->second;
        }
        for (char& c : policy) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        uint8_t compType = RRES_COMP_NONE;
        std::vector<uint8_t> compChunk;

        if (policy == "deflate") {
            int compSize = 0;
            unsigned char* compressed = CompressData(uncompChunk.data(), static_cast<int>(uncompChunk.size()), &compSize);
            if (compressed != nullptr && compSize > 0) {
                compType = RRES_COMP_DEFLATE;
                compChunk.assign(compressed, compressed + compSize);
                MemFree(compressed);
            } else {
                std::cerr << "[iyipack] WARNING: Deflate compression failed for '" << cand.normRelPath << "', storing uncompressed.\n";
                compType = RRES_COMP_NONE;
                compChunk = uncompChunk;
            }
        } else {
            compType = RRES_COMP_NONE;
            compChunk = uncompChunk;
        }

        // Encryption step
        uint8_t cipherType = RRES_CIPHER_NONE;
        std::vector<uint8_t> packedChunk;

        if (cfg.encryptionEnabled) {
            cipherType = RRES_CIPHER_XCHACHA20_POLY1305;
            size_t plainSize = compChunk.size();

            uint8_t nonce[24] = { 0 };
            if (!GenerateRandomBytes(nonce, sizeof(nonce))) {
                std::cerr << "[iyipack] ERROR: Failed to generate random nonce\n";
                if (!encryptionKey.empty()) {
                    crypto_wipe(encryptionKey.data(), encryptionKey.size());
                }
                return 1;
            }

            std::vector<uint8_t> cipherText(plainSize);
            uint8_t mac[16] = { 0 };
            crypto_aead_lock(cipherText.data(), mac, reinterpret_cast<const uint8_t*>(encryptionKey.data()), nonce, nullptr, 0, compChunk.data(), plainSize);

            // Chunk layout: [cipherText: plainSize] [salt: 16] [nonce: 24] [mac: 16]
            packedChunk.resize(plainSize + 16 + 24 + 16);
            if (plainSize > 0) {
                std::memcpy(packedChunk.data(), cipherText.data(), plainSize);
            }
            std::memcpy(packedChunk.data() + plainSize, archiveSalt, 16);
            std::memcpy(packedChunk.data() + plainSize + 16, nonce, 24);
            std::memcpy(packedChunk.data() + plainSize + 16 + 24, mac, 16);

            crypto_wipe(nonce, sizeof(nonce));
            crypto_wipe(mac, sizeof(mac));
        } else {
            cipherType = RRES_CIPHER_NONE;
            packedChunk = std::move(compChunk);
        }

        // Fill rresResourceChunkInfo
        BuiltChunk chunk;
        std::memset(&chunk.info, 0, sizeof(rresResourceChunkInfo));
        chunk.info.type[0] = 'R';
        chunk.info.type[1] = 'A';
        chunk.info.type[2] = 'W';
        chunk.info.type[3] = 'D';
        chunk.info.id = cand.id;
        chunk.info.compType = compType;
        chunk.info.cipherType = cipherType;
        chunk.info.flags = 0;
        chunk.info.packedSize = static_cast<uint32_t>(packedChunk.size());
        chunk.info.baseSize = static_cast<uint32_t>(baseSize);
        chunk.info.nextOffset = 0;
        chunk.info.reserved = 0;
        chunk.info.crc32 = rresComputeCRC32(packedChunk.data(), static_cast<int>(packedChunk.size()));
        chunk.data = std::move(packedChunk);

        builtChunks.push_back(std::move(chunk));
    }

    if (!encryptionKey.empty()) {
        crypto_wipe(encryptionKey.data(), encryptionKey.size());
    }

    // Ensure parent directories exist
    if (outputPath.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(outputPath.parent_path(), ec);
    }

    // Write .rres archive
    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile.is_open()) {
        std::cerr << "[iyipack] ERROR: Could not open output archive for writing: " << outputPath.string() << "\n";
        return 1;
    }

    rresFileHeader header;
    std::memset(&header, 0, sizeof(rresFileHeader));
    header.id[0] = 'r';
    header.id[1] = 'r';
    header.id[2] = 'e';
    header.id[3] = 's';
    header.version = 100;
    header.chunkCount = static_cast<unsigned short>(builtChunks.size());
    header.cdOffset = 0; // Central Directory omitted
    header.reserved = 0;

    outFile.write(reinterpret_cast<const char*>(&header), sizeof(rresFileHeader));

    for (const auto& chunk : builtChunks) {
        outFile.write(reinterpret_cast<const char*>(&chunk.info), sizeof(rresResourceChunkInfo));
        if (!chunk.data.empty()) {
            outFile.write(reinterpret_cast<const char*>(chunk.data.data()), chunk.data.size());
        }
    }

    outFile.close();

    uintmax_t archiveSize = 0;
    std::error_code ec;
    archiveSize = fs::file_size(outputPath, ec);

    std::cout << "[iyipack] Done. Packed " << builtChunks.size() << " files into "
              << outputPath.string() << " (" << archiveSize << " bytes).\n";

    return 0;
}
