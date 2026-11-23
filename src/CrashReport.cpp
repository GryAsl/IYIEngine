// Crash and hang reports (include/IYI/CrashReport.h). This file includes windows.h, so it must never
// include raylib (the two clash).
#include "IYI/CrashReport.h"
#include "IYI/Log.h"
#include "IYI/Profiler.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace IYI {

namespace {

std::string Sections()
{
    return "game thread section: " + ProfileOpenSection(ProfileThread::Game, false) +
           "\nrender thread section: " + ProfileOpenSection(ProfileThread::Render, false) + "\n";
}

#ifdef _WIN32
fs::path s_logsDir;
std::wstring s_gameName;
bool s_dialog = false;
std::atomic<bool> s_crashed{ false };

std::wstring Wide(const std::string& text)
{
    std::wstring out(MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), static_cast<int>(out.size()));
    if (!out.empty()) out.pop_back();   // the terminator the count included
    return out;
}

// Runs once, on the crashing thread; the heap may be damaged, so it does as little as it can.
void WriteReport(EXCEPTION_POINTERS* info, const char* what)
{
    if (s_crashed.exchange(true)) return;
    std::error_code ec;
    fs::create_directories(s_logsDir, ec);
    HANDLE dump = CreateFileW((s_logsDir / "crash.dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{ GetCurrentThreadId(), info, FALSE };
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                          info ? &exception : nullptr, nullptr, nullptr);
        CloseHandle(dump);
    }
    char where[600] = "unknown";
    if (info) {
        void* address = info->ExceptionRecord->ExceptionAddress;
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(address), &module)) {
            GetModuleFileNameA(module, name, MAX_PATH);
        }
        std::snprintf(where, sizeof(where), "exception 0x%08lX at %s+0x%llX",
                      info->ExceptionRecord->ExceptionCode, name,
                      static_cast<unsigned long long>(static_cast<char*>(address) - reinterpret_cast<char*>(module)));
    }
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    if (std::FILE* text = _wfopen((s_logsDir / "crash.txt").c_str(), L"w")) {
        std::fprintf(text, "%s\ntime: %s\nwhat: %s\n%s\ncrash.dmp: minidump for a debugger\n"
                           "crash_python.txt: Python stacks at a fatal error (appended per crash)\n"
                           "latest.log: the engine log of this run\n",
                     what, stamp, where, Sections().c_str());
        std::fclose(text);
    }
    if (!s_dialog) return;
    const std::wstring dir = s_logsDir.wstring();
    const std::wstring message = s_gameName + L" has crashed.\n\nA crash report was saved in:\n" + dir +
                                 L"\n\nOpen that folder?";
    if (MessageBoxW(nullptr, message.c_str(), s_gameName.c_str(), MB_YESNO | MB_ICONERROR | MB_TOPMOST) == IDYES) {
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info)
{
    WriteReport(info, "unhandled exception");
    return EXCEPTION_EXECUTE_HANDLER;   // end the process without Windows' own crash dialog
}
#endif

// Hang watchdog state: written by the render loop, read by the watchdog thread.
std::atomic<long long> s_frames{ 0 };
std::atomic<double> s_lastFrame{ 0.0 };
std::thread s_watchdog;
std::mutex s_stopMutex;
std::condition_variable s_stopCv;
bool s_stop = false;

double Seconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string Fixed1(double value)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", value);
    return text;
}

// unused in the browser build (no threads there: WatchdogStart is a no-op)
[[maybe_unused]] void Watch(fs::path dir)
{
    constexpr double kStallSeconds = 2.0;
    bool stalled = false;
    double stallStart = 0.0;
    std::unique_lock<std::mutex> lock(s_stopMutex);
    while (!s_stopCv.wait_for(lock, std::chrono::milliseconds(500), [] { return s_stop; })) {
        const double now = Seconds();
        const double since = s_frames.load() == 0 ? 0.0 : now - s_lastFrame.load();   // startup is not a stall
        const std::string game = ProfileOpenSection(ProfileThread::Game);
        const std::string render = ProfileOpenSection(ProfileThread::Render);
        if (!stalled && since >= kStallSeconds) {
            stalled = true;
            stallStart = now - since;
            LogWrite(LogLevel::Warn, "watchdog", "main loop stalled " + Fixed1(since) + " s (game: " + game +
                     ", render: " + render + "); Python stacks in agentio/hang_stack.txt");
        } else if (stalled && since < kStallSeconds) {
            stalled = false;
            LogWrite(LogLevel::Warn, "watchdog", "main loop recovered after a " + Fixed1(now - since - stallStart) +
                     " s stall");
        }
        nlohmann::json beat = { { "frame", s_frames.load() }, { "seconds_since_frame", since },
                                { "game_section", game }, { "render_section", render }, { "stalled", stalled },
                                { "private_mb", nullptr }, { "working_set_mb", nullptr } };
#ifdef _WIN32
        beat["pid"] = GetCurrentProcessId();
        PROCESS_MEMORY_COUNTERS_EX mem{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem))) {
            beat["private_mb"] = mem.PrivateUsage / (1024.0 * 1024.0);
            beat["working_set_mb"] = mem.WorkingSetSize / (1024.0 * 1024.0);
        }
#endif
        const fs::path temp = dir / "heartbeat.json.tmp";
        {
            std::ofstream out(temp, std::ios::binary);
            out << beat.dump(1);
        }
        std::error_code ec;
        fs::rename(temp, dir / "heartbeat.json", ec);
    }
}

} // namespace

void CrashReportInstall(const std::string& logsDir, const std::string& gameName, bool dialog)
{
#ifdef _WIN32
    s_logsDir = fs::u8path(logsDir);
    s_gameName = Wide(gameName);
    s_dialog = dialog;
    SetUnhandledExceptionFilter(OnUnhandledException);
    std::set_terminate([] {
        WriteReport(nullptr, "std::terminate (an uncaught C++ exception)");
        TerminateProcess(GetCurrentProcess(), 3);
    });
#else
    // shortcut: no crash report off Windows yet; add a signal handler when a Linux/macOS build ships.
    (void)logsDir, (void)gameName, (void)dialog;
#endif
}

void WatchdogStart(const std::string& agentioDir)
{
#if defined(__EMSCRIPTEN__) || defined(IYI_MOBILE)
    // shortcut: the browser build has no threads (Application::m_singleThread), so there is no
    // watchdog thread there; a stall is just the tab pausing. Add one if web ever gets pthreads.
    (void)agentioDir;
#else
    if (s_watchdog.joinable()) return;
    std::error_code ec;
    fs::create_directories(fs::u8path(agentioDir), ec);
    s_stop = false;
    s_watchdog = std::thread(Watch, fs::u8path(agentioDir));
#endif
}

void WatchdogFrame()
{
    s_lastFrame.store(Seconds());
    s_frames.fetch_add(1);
}

void WatchdogStop()
{
    if (!s_watchdog.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(s_stopMutex);
        s_stop = true;
    }
    s_stopCv.notify_all();
    s_watchdog.join();
}

} // namespace IYI
