/*
 * Iroise: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibCore/Forward.h>

namespace Ladybird {

// Ends a page's process before it takes the phone's memory. A page that leaks (a WebGL test was seen holding 6.6 GB
// of the GPU's memory) otherwise runs the phone out of it: the kernel kills the wrong process, everything thrashes,
// and the phone's watchdog reboots it. The page shows that its process went away instead.
NonnullRefPtr<Core::Timer> start_memory_guard();

// Asks the kernel to end this process first when memory runs out, the more so the less its loss costs: a page before
// the Compositor, both before the phone's own apps. A process may raise this for itself but not lower it.
void raise_out_of_memory_priority(StringView role);

}
