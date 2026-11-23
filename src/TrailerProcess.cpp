// Runs FFmpeg for IYITrailer (Trailer.h): one child process per call, launched from an argument list
// (never through a shell, so paths with spaces or non-ASCII characters pass through intact), its
// output appended to a log file, optionally fed through a pipe into its stdin. Kept apart from
// Trailer.cpp because windows.h and raylib.h clash.

#include "IYI/Trailer.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace IYI {

// CommandLineToArgvW's rules: quote when needed; backslashes double only before a quote.
std::string TrailerQuoteArgs(const std::vector<std::string>& args)
{
    std::string line;
    for (const std::string& arg : args) {
        if (!line.empty()) line += ' ';
        if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) {
            line += arg;
            continue;
        }
        line += '"';
        size_t slashes = 0;
        for (char c : arg) {
            if (c == '\\') { ++slashes; continue; }
            line.append(c == '"' ? slashes * 2 + 1 : slashes, '\\');
            slashes = 0;
            line += c;
        }
        line.append(slashes * 2, '\\');
        line += '"';
    }
    return line;
}

#ifdef _WIN32

static std::wstring Wide(const std::string& s)
{
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size());
    return w;
}

bool TrailerSpawn(const std::vector<std::string>& args, const std::string& logPath, bool pipeInput, TrailerProcess& out)
{
    SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
    HANDLE log = CreateFileW(Wide(logPath).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) return false;
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (pipeInput) {
        if (!CreatePipe(&readEnd, &writeEnd, &inherit, 8u << 20)) { CloseHandle(log); return false; }
        SetHandleInformation(writeEnd, HANDLE_FLAG_INHERIT, 0);   // the child must not hold its own input open
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = pipeInput ? readEnd : nullptr;
    si.hStdOutput = log;
    si.hStdError = log;
    PROCESS_INFORMATION pi{};
    std::wstring line = Wide(TrailerQuoteArgs(args));
    const bool ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &si, &pi) != 0;
    CloseHandle(log);
    if (readEnd) CloseHandle(readEnd);
    if (!ok) {
        if (writeEnd) CloseHandle(writeEnd);
        return false;
    }
    CloseHandle(pi.hThread);
    out.Handle = pi.hProcess;
    out.Input = writeEnd;
    return true;
}

bool TrailerWrite(TrailerProcess& process, const void* data, size_t size)
{
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        DWORD written = 0;
        const DWORD chunk = size > (1u << 30) ? (1u << 30) : (DWORD)size;
        if (!process.Input || !WriteFile(process.Input, p, chunk, &written, nullptr)) return false;   // broken pipe
        p += written;
        size -= written;
    }
    return true;
}

int TrailerWait(TrailerProcess& process)
{
    if (process.Input) CloseHandle(process.Input);
    process.Input = nullptr;
    if (!process.Handle) return -1;
    WaitForSingleObject(process.Handle, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.Handle, &code);
    CloseHandle(process.Handle);
    process.Handle = nullptr;
    return (int)code;
}

#else

bool TrailerSpawn(const std::vector<std::string>& args, const std::string& logPath, bool pipeInput, TrailerProcess& out)
{
    const int log = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log < 0) return false;
    int fds[2] = { -1, -1 };
    if (pipeInput && pipe(fds) != 0) { close(log); return false; }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (pipeInput) {
        posix_spawn_file_actions_adddup2(&actions, fds[0], 0);
        posix_spawn_file_actions_addclose(&actions, fds[1]);
    }
    posix_spawn_file_actions_adddup2(&actions, log, 1);
    posix_spawn_file_actions_adddup2(&actions, log, 2);
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    const bool ok = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ) == 0;
    posix_spawn_file_actions_destroy(&actions);
    close(log);
    if (pipeInput) close(fds[0]);
    if (!ok) {
        if (pipeInput) close(fds[1]);
        return false;
    }
    signal(SIGPIPE, SIG_IGN);   // a dead encoder must fail the write, not kill the engine
    out.Handle = reinterpret_cast<void*>(static_cast<intptr_t>(pid));
    out.Input = pipeInput ? reinterpret_cast<void*>(static_cast<intptr_t>(fds[1] + 1)) : nullptr;   // +1: fd 0 is valid
    return true;
}

bool TrailerWrite(TrailerProcess& process, const void* data, size_t size)
{
    if (!process.Input) return false;
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(process.Input)) - 1;
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

int TrailerWait(TrailerProcess& process)
{
    if (process.Input) close(static_cast<int>(reinterpret_cast<intptr_t>(process.Input)) - 1);
    process.Input = nullptr;
    if (!process.Handle) return -1;
    int status = 0;
    waitpid(static_cast<pid_t>(reinterpret_cast<intptr_t>(process.Handle)), &status, 0);
    process.Handle = nullptr;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#endif

} // namespace IYI
