/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>

namespace Ladybird {

class Application final : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

private:
    Application() = default;

    // Each run is a throwaway probe: never hand off to an already running instance.
    virtual bool should_coordinate_browser_process() const override { return false; }
    virtual bool should_use_temporary_profile_by_default() const override { return true; }
};

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    auto app = TRY(Ladybird::Application::create(arguments));

    if (!Ladybird::Application::browser_options().headless_mode.has_value()) {
        warnln("ladybird-sfos: only --headless is implemented so far");
        return 1;
    }

    return app->execute();
}
