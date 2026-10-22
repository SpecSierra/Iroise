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
#include <string.h>
#include <unistd.h>

typedef int (*EngineMain)(char const* role, int argc, char** argv);
typedef int (*InterfaceMain)(int argc, char** argv);

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
