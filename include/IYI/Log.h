// Declares the centralized engine logging core.
// All engine messages pass through LogWrite to guarantee consistent formatting, column-aligned
// greppable output, and synchronized disk log recording with automatic archive rotation.
//
// Connected to:
// - Initialized in Application::Initialize after project path discovery.
// - Flushed and closed in Application::Shutdown and Application::~Application.
// - Receives routed console messages from Application, ProjectConfig, PythonRuntime, AssetManager, and UI subsystems.
//
// Future evolution (per ROADMAP.md):
// Public API remains minimal and safe to call from any thread (one mutex). Advanced formatting
// frameworks and asynchronous log queues are explicitly rejected.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace IYI {

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error
};

void LogInit(const std::string& projectDir);
void LogShutdown();
void LogWrite(LogLevel level, const std::string& category, const std::string& message);
void LogSetMinLevel(LogLevel level);
LogLevel LogParseLevel(const std::string& text, bool& ok);
int LogWarnCount();
int LogErrorCount();
// The last 40 Info/Warn/Error lines as written (agent channel health, SPEC §3.1).
std::vector<std::string> LogRecentLines();

// Structured events, one JSON object per line in logs/events.jsonl ({"t", "frame", "kind", ...fields}):
// the engine writes scene, ui_click, warning, error and quit; a game adds its own (engine.log_event).
void LogEvent(const std::string& kind, const std::vector<std::pair<std::string, std::string>>& fields = {});
void LogEventJson(const std::string& kind, const std::string& fieldsObject);   // fields as a JSON object
void LogSetFrame(int frame);                     // the frame number the next events carry
int LogEventCount();                             // events since start
std::vector<std::string> LogEventsSince(int mark);   // the lines after that count (the last 500 kept)

} // namespace IYI

using IYI::LogLevel;
using IYI::LogInit;
using IYI::LogShutdown;
using IYI::LogWrite;
using IYI::LogSetMinLevel;
using IYI::LogParseLevel;
using IYI::LogWarnCount;
using IYI::LogErrorCount;
using IYI::LogRecentLines;
using IYI::LogEvent;
using IYI::LogEventJson;
