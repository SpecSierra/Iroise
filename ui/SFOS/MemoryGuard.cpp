/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/StdLibExtras.h>
#include <LibCore/Environment.h>
#include <LibCore/Timer.h>
#include <LibWebView/Application.h>
#include <LibWebView/Process.h>
#include <LibWebView/ProcessManager.h>
#include <UI/SFOS/MemoryGuard.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

namespace Ladybird {

static u64 page_size()
{
    auto size = sysconf(_SC_PAGESIZE);
    return size > 0 ? static_cast<u64>(size) : 4096;
}

// What one process may hold: PACIFIC_PROCESS_MEMORY_LIMIT_MB, else 30% of the phone's memory, within 1 to 3 GiB
// (3 GiB on the Jolla Phone 2's 12 GB, about 1.1 GiB on the Xperia 10 II's 4 GB). The heaviest pages take well
// under 1 GiB.
static u64 memory_limit()
{
    if (auto megabytes = Core::Environment::get("PACIFIC_PROCESS_MEMORY_LIMIT_MB"sv); megabytes.has_value()) {
        if (auto value = megabytes->to_number<u64>(); value.has_value() && *value > 0)
            return *value * MiB;
    }
    auto pages = sysconf(_SC_PHYS_PAGES);
    auto total = pages > 0 ? static_cast<u64>(pages) * page_size() : 4096 * MiB;
    return clamp(total * 3 / 10, 1024 * MiB, 3072 * MiB);
}

// What a process holds in memory, its share of the GPU's included: the GPU driver maps its buffers into the process.
static Optional<u64> resident_memory(pid_t pid)
{
    auto path = ByteString::formatted("/proc/{}/statm", pid);
    auto* file = fopen(path.characters(), "r");
    if (!file)
        return {};
    unsigned long size = 0;
    unsigned long resident = 0;
    auto fields = fscanf(file, "%lu %lu", &size, &resident);
    fclose(file);
    if (fields != 2)
        return {};
    return static_cast<u64>(resident) * page_size();
}

NonnullRefPtr<Core::Timer> start_memory_guard()
{
    auto limit = memory_limit();
    auto timer = Core::Timer::create_repeating(AK::Duration::from_seconds(2), [limit] {
        WebView::Application::process_manager().for_each_process([&](WebView::Process& process) {
            switch (process.type()) {
            case WebView::ProcessType::WebContent:
            case WebView::ProcessType::WebWorker:
            case WebView::ProcessType::ImageDecoder:
            case WebView::ProcessType::Compositor:
                break;
            default:
                return;
            }
            auto resident = resident_memory(process.pid());
            if (!resident.has_value() || *resident <= limit)
                return;
            warnln("Pacific: {} (pid {}) holds {} MiB, more than the {} MiB a process may: ending it",
                WebView::process_name_from_type(process.type()), process.pid(), *resident / MiB, limit / MiB);
            kill(process.pid(), SIGKILL);
        });
    });
    timer->start();
    return timer;
}

void raise_out_of_memory_priority(StringView role)
{
    // From 0 (the default) to 1000 (first to go). Android's cached apps, under the phone's App Support, are at 900
    // and above: a page goes before them.
    int score = 0;
    if (role == "WebContent"sv || role == "WebWorker"sv)
        score = 950;
    else if (role == "ImageDecoder"sv || role == "WasmCompiler"sv)
        score = 900;
    else if (role == "MediaServer"sv || role == "VideoDecoderServer"sv || role == "AudioServer"sv)
        score = 800;
    else if (role == "Compositor"sv || role == "RequestServer"sv)
        score = 700;
    if (score == 0)
        return;
    auto* file = fopen("/proc/self/oom_score_adj", "r+");
    if (!file)
        return;
    int current = 0;
    if (fscanf(file, "%d", &current) == 1 && current < score) {
        rewind(file);
        fprintf(file, "%d\n", score);
    }
    fclose(file);
}

}
