/*
 * Iroise: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Runs one of the engine's processes ("Ladybird", "WebContent", ...) with that process's own arguments, argv[0]
// included. See EngineMain.cpp.
extern "C" [[gnu::visibility("default")]] int iroise_engine_main(char const* role, int argc, char** argv);
