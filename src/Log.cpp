// Implements the centralized engine logging core.
// Formats log messages with microsecond timestamps, fixed-width level strings, and categories.
// Dispatches to stdout (Debug/Info) and stderr (Warn/Error), synchronized with a persistent latest.log file.
// Automatically archives prior runs and rotates archives to maintain a strict file cap.
// Thread-safe: the game thread and the render thread both log, so one mutex covers the file, the
// console and the recent lines (a line is never torn or interleaved with another thread's).

#include "IYI/Log.h"
#include "MobilePaths.h"
#ifdef IYI_PLATFORM_ANDROID
#include <android/log.h>
#endif
#include <iostream>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <vector>
#include <deque>
#include <cctype>
#include <cstdio>
#include <atomic>
#include <mutex>

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void* hModule, wchar_t* lpFilename, unsigned long nSize);
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace IYI {

static std::atomic<LogLevel> s_minLevel{ LogLevel::Info };
static std::mutex s_mutex;                 // guards the file, the console writes and s_recent
static std::ofstream s_logFile;
static bool s_fileOpen = false;
static std::atomic<int> s_warnCount{ 0 };
static std::atomic<int> s_errorCount{ 0 };
static std::deque<std::string> s_recent;   // the last Info+ lines, for the agent channel's health
constexpr size_t kRecentLines = 40;
static std::ofstream s_eventFile;          // logs/events.jsonl, guarded by s_mutex too
static std::deque<std::string> s_events;   // the last kEvents event lines, for agent replies
static int s_eventCount = 0;
static std::atomic<int> s_frame{ 0 };
constexpr size_t kEvents = 500;

// Length of the valid UTF-8 sequence at s[i]; 0 for a stray or cut byte (overlong forms, UTF-16
// surrogates and code points past U+10FFFF included).
static size_t Utf8Length(const std::string& s, size_t i)
{
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const size_t n = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >> 4) == 0xE ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
    if (n == 0 || i + n > s.size()) return 0;
    for (size_t k = 1; k < n; ++k)
        if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return 0;
    const unsigned char c1 = n > 1 ? static_cast<unsigned char>(s[i + 1]) : 0;
    if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 > 0x9F) || (c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 > 0x8F)) return 0;
    return n;
}

// A JSON string literal. Invalid UTF-8 becomes U+FFFD, so every line of events.jsonl parses.
static std::string JsonString(const std::string& s)
{
    std::string out = "\"";
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const size_t n = Utf8Length(s, i);
        if (n == 0) { out += "\xEF\xBF\xBD"; continue; }
        if (n > 1) { out.append(s, i, n); i += n - 1; continue; }
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (static_cast<unsigned char>(c) < 0x20) out += ' ';
        else out += c;
    }
    return out + "\"";
}

static std::string GetTimestampString();

// Caller holds s_mutex. fieldsObject is "{...}" (valid JSON), spliced after the fixed keys.
static void WriteEventLocked(const std::string& kind, const std::string& fieldsObject)
{
    std::string line = "{\"t\":" + JsonString(GetTimestampString()) + ",\"frame\":" + std::to_string(s_frame.load()) +
                       ",\"kind\":" + JsonString(kind);
    const size_t open = fieldsObject.find('{');
    const std::string rest = open == std::string::npos ? "}" : fieldsObject.substr(open + 1);
    line += (rest.find_first_not_of(" \t\r\n}") == std::string::npos) ? "}" : "," + rest;
    s_events.push_back(line);
    if (s_events.size() > kEvents) s_events.pop_front();
    ++s_eventCount;
    if (s_eventFile.is_open()) s_eventFile << line << "\n" << std::flush;
}

void LogEventJson(const std::string& kind, const std::string& fieldsObject)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    WriteEventLocked(kind, fieldsObject);
}

void LogEvent(const std::string& kind, const std::vector<std::pair<std::string, std::string>>& fields)
{
    std::string object = "{";
    for (const auto& [key, value] : fields) object += (object.size() > 1 ? "," : "") + JsonString(key) + ":" + JsonString(value);
    LogEventJson(kind, object + "}");
}

void LogSetFrame(int frame) { s_frame = frame; }

int LogEventCount()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_eventCount;
}

std::vector<std::string> LogEventsSince(int mark)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    const int available = std::min(s_eventCount - mark, static_cast<int>(s_events.size()));
    if (available <= 0) return {};
    return std::vector<std::string>(s_events.end() - available, s_events.end());
}

static bool LocalTime(const std::time_t* timer, std::tm* outTm)
{
#ifdef _WIN32
    return localtime_s(outTm, timer) == 0;
#else
    return localtime_r(timer, outTm) != nullptr;
#endif
}

static std::string GetExecutablePath()
{
#ifdef _WIN32
    wchar_t buffer[1024];
    unsigned long len = GetModuleFileNameW(nullptr, buffer, 1024);
    if (len > 0 && len < 1024) {
        std::string s = std::filesystem::path(buffer).string();
        for (char& c : s) {
            if (c == '\\') c = '/';
        }
        return s;
    }
#elif defined(__linux__)
    char buffer[1024];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        buffer[len] = '\0';
        return std::string(buffer);
    }
#elif defined(__APPLE__)
    char buffer[1024];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        return std::string(buffer);
    }
#endif
    return "unknown";
}

static const char* LevelToString(LogLevel level)
{
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO ";
}

static std::string GetTimestampString()
{
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t timer = std::chrono::system_clock::to_time_t(now);
    std::tm bt{};
    LocalTime(&timer, &bt);
    char buf[32];
    int msCount = static_cast<int>(ms.count());
    if (msCount < 0) msCount = -msCount;
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", bt.tm_hour, bt.tm_min, bt.tm_sec, msCount);
    return std::string(buf);
}

static std::string GetLocalDateTimeString()
{
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm bt{};
    LocalTime(&tt, &bt);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  bt.tm_year + 1900,
                  bt.tm_mon + 1,
                  bt.tm_mday,
                  bt.tm_hour,
                  bt.tm_min,
                  bt.tm_sec);
    return std::string(buf);
}

static std::string FormatArchiveName(const std::filesystem::file_time_type& ftime)
{
    using namespace std::chrono;
    auto sysTime = time_point_cast<system_clock::duration>(ftime - std::filesystem::file_time_type::clock::now() + system_clock::now());
    std::time_t tt = system_clock::to_time_t(sysTime);
    std::tm bt{};
    LocalTime(&tt, &bt);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "run-%04d%02d%02d-%02d%02d%02d.log",
                  bt.tm_year + 1900,
                  bt.tm_mon + 1,
                  bt.tm_mday,
                  bt.tm_hour,
                  bt.tm_min,
                  bt.tm_sec);
    return std::string(buf);
}

void LogSetMinLevel(LogLevel level)
{
    s_minLevel = level;
}

int LogWarnCount()
{
    return s_warnCount;
}

int LogErrorCount()
{
    return s_errorCount;
}

std::vector<std::string> LogRecentLines()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return { s_recent.begin(), s_recent.end() };
}

LogLevel LogParseLevel(const std::string& text, bool& ok)
{
    std::string lower = text;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lower == "debug") {
        ok = true;
        return LogLevel::Debug;
    } else if (lower == "info") {
        ok = true;
        return LogLevel::Info;
    } else if (lower == "warn" || lower == "warning") {
        ok = true;
        return LogLevel::Warn;
    } else if (lower == "error") {
        ok = true;
        return LogLevel::Error;
    }
    ok = false;
    return LogLevel::Info;
}

void LogInit(const std::string& projectDir)
{
    if (s_fileOpen) {
        LogShutdown();
    }

    namespace fs = std::filesystem;
    std::error_code ec;

    fs::path projectPath(projectDir);
#ifdef IYI_MOBILE
    projectPath = MobileDataRoot();
    if (projectPath.empty()) {
        LogWrite(LogLevel::Error, "startup", "Mobile data root is unavailable; file logging disabled");
        return;
    }
#endif
    fs::path logsDir = projectPath / "logs";

    if (!fs::exists(logsDir, ec)) {
        fs::create_directories(logsDir, ec);
        if (ec) {
            s_fileOpen = false;
            LogWrite(LogLevel::Warn, "startup", "Could not create log directory: " + logsDir.string() + " (" + ec.message() + ")");
            return;
        }
    }

    fs::path latestLog = logsDir / "latest.log";
    if (fs::exists(latestLog, ec)) {
        auto ftime = fs::last_write_time(latestLog, ec);
        if (!ec) {
            auto size = fs::file_size(latestLog, ec);
            if (!ec && size == 0) {
                fs::remove(latestLog, ec);
            } else {
                std::string archiveName = FormatArchiveName(ftime);
                fs::path archivePath = logsDir / archiveName;
                if (fs::exists(archivePath, ec)) {
                    int seq = 1;
                    std::string base = archiveName.substr(0, archiveName.length() - 4);
                    do {
                        archivePath = logsDir / (base + "-" + std::to_string(seq++) + ".log");
                    } while (fs::exists(archivePath, ec));
                }
                fs::rename(latestLog, archivePath, ec);
                if (ec) {
                    LogWrite(LogLevel::Warn, "startup", "Failed to archive latest.log: " + ec.message());
                }
            }
        }
    }

    // Count only latest.log and run-*.log files; other files in logs/ are left alone.
    struct ArchiveEntry {
        fs::path path;
        fs::file_time_type writeTime;
    };
    std::vector<ArchiveEntry> archives;
    for (const auto& entry : fs::directory_iterator(logsDir, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec)) {
            std::string fname = entry.path().filename().string();
            bool isRunLog = (fname.size() >= 8 && fname.compare(0, 4, "run-") == 0 && fname.compare(fname.size() - 4, 4, ".log") == 0);
            if (fname == "latest.log" || isRunLog) {
                archives.push_back({ entry.path(), entry.last_write_time(ec) });
            }
        }
    }

    std::sort(archives.begin(), archives.end(), [](const ArchiveEntry& a, const ArchiveEntry& b) {
        if (a.writeTime != b.writeTime) {
            return a.writeTime < b.writeTime;
        }
        return a.path.filename().string() < b.path.filename().string();
    });

    while (archives.size() > 4) {
        fs::remove(archives.front().path, ec);
        archives.erase(archives.begin());
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_logFile.open(latestLog, std::ios::out | std::ios::trunc);
        s_fileOpen = s_logFile.is_open();
        if (s_eventFile.is_open()) s_eventFile.close();
        s_eventFile.open(logsDir / "events.jsonl", std::ios::out | std::ios::trunc);
    }
    if (!s_fileOpen) {
        LogWrite(LogLevel::Warn, "startup", "Could not open log file: " + latestLog.string());
        return;
    }

    // First line of every run
    std::string exePath = GetExecutablePath();
    fs::path absProj = fs::canonical(projectPath, ec);
    if (ec) {
        absProj = fs::absolute(projectPath, ec);
    }
    std::string resolvedProj = absProj.string();
    for (char& c : resolvedProj) {
        if (c == '\\') c = '/';
    }

    LogWrite(LogLevel::Info, "startup", "engine started: exe=\"" + exePath + "\", project=\"" + resolvedProj + "\", time=" + GetLocalDateTimeString());
}

void LogShutdown()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_fileOpen && s_logFile.is_open()) {
        s_logFile << std::flush;
        s_logFile.close();
    }
    s_fileOpen = false;
    if (s_eventFile.is_open()) s_eventFile.close();
}

void LogWrite(LogLevel level, const std::string& category, const std::string& message)
{
    if (level == LogLevel::Warn) {
        ++s_warnCount;
    } else if (level == LogLevel::Error) {
        ++s_errorCount;
    }

    std::lock_guard<std::mutex> lock(s_mutex);
    if (level == LogLevel::Warn || level == LogLevel::Error) {
        WriteEventLocked(level == LogLevel::Warn ? "warning" : "error",
                         "{\"category\":" + JsonString(category) + ",\"message\":" + JsonString(message) + "}");
    }
    auto writeSingleLine = [&](const std::string& msg) {
        std::string line = "[" + GetTimestampString() + "] [" + LevelToString(level) + "] [" + category + "] " + msg;
#ifdef IYI_PLATFORM_ANDROID
        int priority = level == LogLevel::Debug ? ANDROID_LOG_DEBUG : level == LogLevel::Info ? ANDROID_LOG_INFO :
                       level == LogLevel::Warn ? ANDROID_LOG_WARN : ANDROID_LOG_ERROR;
        __android_log_write(priority, "IYIEngine", line.c_str());
#endif

        if (level != LogLevel::Debug) {
            s_recent.push_back(line);
            if (s_recent.size() > kRecentLines) s_recent.pop_front();
        }
        if (s_fileOpen && s_logFile.is_open()) {
            s_logFile << line << "\n" << std::flush;
        }

        if (level >= s_minLevel) {
            if (level == LogLevel::Debug || level == LogLevel::Info) {
                std::cout << line << "\n" << std::flush;
            } else {
                std::cerr << line << "\n" << std::flush;
            }
        }
    };

    if (message.find('\n') == std::string::npos) {
        writeSingleLine(message);
    } else {
        std::string line;
        for (char c : message) {
            if (c == '\n') {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                writeSingleLine(line);
                line.clear();
            } else {
                line += c;
            }
        }
        if (!line.empty()) {
            if (line.back() == '\r') {
                line.pop_back();
            }
            writeSingleLine(line);
        }
    }
}

} // namespace IYI
