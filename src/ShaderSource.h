#pragma once

#include <functional>
#include <string>

namespace IYI {

inline void RewriteGlesShader(std::string& code, bool web)
{
    if (code.rfind("#version 330", 0) != 0) return;
    std::string header = "#version 300 es";
    if (web || code.find("precision highp float;") == std::string::npos)
        header += "\nprecision highp float;";
    if (web || code.find("precision highp int;") == std::string::npos)
        header += "\nprecision highp int;";
    code.replace(0, code.find('\n'), header);
}

// Shared shader code (raylib has no #include): each line starting `#include "name"` becomes the text
// of the file `name` beside `path`, read through `read` (the ResourceStore, so packaged games work).
// One level: an included file's own #include is left as is. Returns the path it could not read, or "".
inline std::string ExpandShaderIncludes(std::string& code, const std::string& path,
                                        const std::function<bool(const std::string&, std::string&)>& read)
{
    const std::string dir = path.substr(0, path.find_last_of("/\\") + 1);
    for (size_t at = code.find("#include \""); at != std::string::npos; at = code.find("#include \"", at + 1)) {
        const size_t close = code.find('"', at + 10);
        if ((at > 0 && code[at - 1] != '\n') || close == std::string::npos) continue;
        const std::string name = dir + code.substr(at + 10, close - at - 10);
        std::string text;
        if (!read(name, text)) return name;
        code.replace(at, close + 1 - at, text);
        at += text.size();
    }
    return "";
}

}
