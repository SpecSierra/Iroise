/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The engine without the Silica app (the build host, tests): `Ladybird [--engine-role <name>] arguments...`. On the
// phone the app's own executable does this, see browser/apps/launcher.

#include <UI/SFOS/EngineMain.h>
#include <string.h>

int main(int argc, char** argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--engine-role"))
        return pacific_engine_main(argv[2], argc - 2, argv + 2);
    return pacific_engine_main("Ladybird", argc, argv);
}
