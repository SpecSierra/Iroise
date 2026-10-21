/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Only in the instrumented engine that trains profile-guided optimisation (scripts/pgo-profile.sh). gcc writes a
// process's profile when it exits, but WebContent, the Compositor and the MediaServer end with _exit
// (Core::Process::terminate_immediately), which skips that. This _exit, which the executable exports so that it takes
// the place of libc's for every library, writes the profile first. One __gcov_dump covers every instrumented library
// in the process: gcc keeps them all in one list.

#include <sys/syscall.h>
#include <unistd.h>

extern "C" void __gcov_dump();

extern "C" [[noreturn]] void _exit(int status)
{
    __gcov_dump();
    syscall(SYS_exit_group, status);
    __builtin_unreachable();
}

extern "C" [[noreturn]] void _Exit(int status)
{
    _exit(status);
}
