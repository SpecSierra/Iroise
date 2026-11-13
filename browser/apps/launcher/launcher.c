/*
 * Iroise: the app's one executable (the Jolla Store allows an app no other). Started as
 * `harbour-iroise --engine-role <name> ...` it is one of the engine's processes; otherwise it is the interface.
 * Either way it loads the library that does the work, from ../share/harbour-iroise/lib beside it (the engine from
 * its own engine/ directory there, with the libraries only it uses). It is plain C
 * so that an engine process, and there is one per site, loads nothing of the interface (Qt) on the way.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <dlfcn.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*EngineMain)(char const* role, int argc, char** argv);
typedef int (*InterfaceMain)(int argc, char** argv);

// The engine keeps a few variables every thread has of its own (its allocator's heaps, the JavaScript heap and VM)
// where a thread reaches them fastest, in the room glibc sets aside for that as a program starts. A library loaded
// later, as the engine is here, only gets what is left of that room, and there is far too little by default
// ("cannot allocate memory in static TLS block"). glibc sets more aside when told to before the program starts, so
// an engine process starts itself once more, asking for it. The engine's libraries need 16 KiB of it today.
#define STATIC_TLS_TUNABLE "glibc.rtld.optional_static_tls="
#define STATIC_TLS_BYTES "65536"

static void start_again_with_room_for_the_engine(char const* executable, char** argv)
{
    char const* tunables = getenv("GLIBC_TUNABLES");
    if (tunables && strstr(tunables, STATIC_TLS_TUNABLE))
        return;
    char value[1024];
    int length = snprintf(value, sizeof(value), "%s%s" STATIC_TLS_TUNABLE STATIC_TLS_BYTES, tunables ? tunables : "",
        tunables && *tunables ? ":" : "");
    if (length < 0 || (size_t)length >= sizeof(value) || setenv("GLIBC_TUNABLES", value, 1) != 0)
        return;
    execv(executable, argv);
    // NB: Not started again: loading the engine says what is wrong.
    perror("harbour-iroise: execv");
}

int main(int argc, char** argv)
{
    char executable[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (length < 0) {
        perror("harbour-iroise: /proc/self/exe");
        return 127;
    }
    executable[length] = 0;

    int is_engine = argc >= 3 && strcmp(argv[1], "--engine-role") == 0;
    if (is_engine)
        start_again_with_room_for_the_engine(executable, argv);
    char library[PATH_MAX];
    snprintf(library, sizeof(library), "%s/../share/harbour-iroise/lib/%s", dirname(executable),
        is_engine ? "engine/libiroiseengine.so" : "libiroisebrowser.so.1");

    void* handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "harbour-iroise: %s\n", dlerror());
        return 127;
    }
    if (is_engine) {
        EngineMain engine_main = (EngineMain)dlsym(handle, "iroise_engine_main");
        // The role's own argv starts at the role's name.
        return engine_main ? engine_main(argv[2], argc - 2, argv + 2) : 127;
    }
    InterfaceMain interface_main = (InterfaceMain)dlsym(handle, "iroise_ui_main");
    return interface_main ? interface_main(argc, argv) : 127;
}
