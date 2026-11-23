// Browser has no processes: the development-only trailer encoder (TrailerProcess.cpp) gets ENOSYS and
// reports the failure through its own error path instead of the link failing.
#include <errno.h>
#include <spawn.h>

int posix_spawnp(pid_t* pid, const char* file, const posix_spawn_file_actions_t* actions,
                 const posix_spawnattr_t* attr, char* const argv[], char* const envp[])
{
    (void)pid; (void)file; (void)actions; (void)attr; (void)argv; (void)envp;
    return ENOSYS;
}
