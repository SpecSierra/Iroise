/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The whole engine as one library, for an app that may ship a single executable (the Jolla Store's rule). That
// executable loads this library and calls pacific_engine_main() with the role it was started for: "Ladybird" (the
// engine the interface talks to, see main.cpp) or one of the helper processes. Each role's ladybird_main() is
// compiled in under its own name (see CMakeLists.txt).

#include <AK/Format.h>
#include <AK/LexicalPath.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibCore/System.h>
#include <LibMain/Main.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <UI/SFOS/EngineMain.h>

// The helpers come from CMakeLists.txt (PACIFIC_HELPERS), which compiles their main.cpp in.
#include <EngineHelperRoles.h>

#define ENUMERATE_ENGINE_ROLES(X) \
    X(Ladybird)                   \
    ENUMERATE_HELPER_ROLES(X)

#define DECLARE_ROLE(role) ErrorOr<int> ladybird_main_##role(Main::Arguments);
ENUMERATE_ENGINE_ROLES(DECLARE_ROLE)
#undef DECLARE_ROLE

namespace Main {

static int s_return_code_for_errors = 1;

int return_code_for_errors() { return s_return_code_for_errors; }
void set_return_code_for_errors(int code) { s_return_code_for_errors = code; }

}

// The helpers are started as `<executable> --engine-role <name> ...` (LADYBIRD_HELPER_LAUNCHER), and find the
// engine's resources from LADYBIRD_APPLICATION_DIRECTORY: share/harbour-pacific/libexec for this library in
// share/harbour-pacific/lib/engine (Build/release/libexec on the build host), as the engine's executables would be laid
// out. Both pass on to every helper.
static void describe_installation()
{
    Dl_info info {};
    if (dladdr(reinterpret_cast<void*>(&describe_installation), &info) && info.dli_fname) {
        // NB: On the build host the library is in Build/release/lib, beside libexec, not in lib/engine.
        auto library_directory = LexicalPath(info.dli_fname).parent();
        auto prefix = library_directory.basename() == "engine"sv ? library_directory.parent().parent() : library_directory.parent();
        setenv("LADYBIRD_APPLICATION_DIRECTORY", prefix.append("libexec"sv).string().characters(), 0);
    }
    if (auto executable = Core::System::current_executable_path(); !executable.is_error())
        setenv("LADYBIRD_HELPER_LAUNCHER", executable.value().characters(), 0);
}

int pacific_engine_main(char const* role, int argc, char** argv)
{
    tzset();
    describe_installation();
    // A document with thousands of images, or many tabs, holds a shared-memory descriptor for each.
    if (auto result = Core::System::set_resource_limits(RLIMIT_NOFILE, 65536); result.is_error())
        warnln("Unable to increase open file limit: {}", result.error());

    Vector<StringView> strings;
    strings.ensure_capacity(argc);
    for (int i = 0; i < argc; ++i)
        strings.unchecked_append({ argv[i], strlen(argv[i]) });
    Main::Arguments arguments { .argc = argc, .argv = argv, .strings = strings.span() };

    auto role_name = StringView { role, strlen(role) };
    ErrorOr<int> result = Error::from_string_literal("Unknown engine role");
#define RUN_ROLE(name)              \
    if (role_name == #name##sv)     \
        result = ladybird_main_##name(arguments);
    ENUMERATE_ENGINE_ROLES(RUN_ROLE)
#undef RUN_ROLE

    if (result.is_error()) {
        warnln("\033[31;1mRuntime error\033[0m ({}): {}", role_name, result.error());
        return Main::return_code_for_errors();
    }
    return result.value();
}
