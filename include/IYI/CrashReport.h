#pragma once

// Crash and hang reports (docs/spec/cli.md §3.2).
//
// Crash (every build, Windows): an unhandled exception or std::terminate writes crash.dmp (a minidump
// for a debugger) and crash.txt (what, where, the open profiler sections) into the run's logs/ folder,
// then a message box offers to open that folder. Nothing is sent anywhere. Python's own stacks at a
// fatal error go to logs/crash_python.txt (PythonRuntime::EnableFaultStacks).
//
// Hang watchdog (development builds only): a thread that only observes. Every 0.5 s it writes
// agentio/heartbeat.json (frame, seconds since the last frame, each thread's open profiler section,
// memory, stalled); a main loop stopped for 2 s is logged once, and again when it recovers.

#include <string>

namespace IYI {

// gameName titles the message box; dialog = false (agent runs, --hidden) writes the files only.
void CrashReportInstall(const std::string& logsDir, const std::string& gameName, bool dialog);

void WatchdogStart(const std::string& agentioDir);
void WatchdogFrame();   // the render loop, once per frame after present
void WatchdogStop();    // first in shutdown; safe when never started

} // namespace IYI
