// Single definition of resource path handling, shared by the engine and tools/iyipack so packaged
// resource IDs can never drift apart.
//
// Two distinct concepts:
// - ResourceFsPath: slash cleanup + lexical normalization, CASE PRESERVED. This is what a filesystem
//   backend opens; lowercasing it breaks case-sensitive filesystems (Linux/macOS).
// - ResourceKey: ResourceFsPath lowercased (ASCII). The deterministic form hashed into packaged IDs.
//   Only archive lookups may use it; never open a physical file with it.

#pragma once

#include <cctype>
#include <filesystem>
#include <string>

namespace IYI {

inline std::string ResourceFsPath(const std::string& path)
{
    if (path.empty()) {
        return "";
    }
    std::string norm = path;
    for (char& c : norm) {
        if (c == '\\') c = '/';
    }
    std::string result = std::filesystem::path(norm).lexically_normal().generic_string();

    size_t start = 0;
    while (start < result.size()) {
        if (result[start] == '/') {
            start++;
        } else if (start + 1 < result.size() && result[start] == '.' && result[start + 1] == '/') {
            start += 2;
        } else {
            break;
        }
    }
    result.erase(0, start);
    if (result == ".") {
        result.clear();
    }
    return result;
}

// Containment chokepoint for every filesystem resource path: `relPath` joined to `root`, or an empty
// path when the result would leave `root` (a "../" climbing out, an absolute or drive path). A
// packaged build can only ever reach files inside the game, so a path that escaped in development
// would work there and nowhere else. Lexical, like the rest of this header: legitimate inner
// normalization such as "assets/../assets/a.png" still resolves. Both separators are accepted.
inline std::filesystem::path ResourceContainedPath(const std::string& root, const std::string& relPath)
{
    std::string rel = relPath;
    for (char& c : rel) {
        if (c == '\\') c = '/';
    }
    const std::filesystem::path relP(rel);
    if (relP.has_root_name() || relP.has_root_directory()) {
        return {};
    }
    const std::filesystem::path base = std::filesystem::path(root).lexically_normal();
    std::filesystem::path full = (base / relP).lexically_normal();
    const std::filesystem::path inside = full.lexically_relative(base);
    if (inside.empty() || *inside.begin() == "..") {
        return {};
    }
    return full;
}

inline std::string ResourceKey(const std::string& path)
{
    std::string key = ResourceFsPath(path);
    for (char& c : key) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return key;
}

} // namespace IYI
