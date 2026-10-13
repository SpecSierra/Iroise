/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/LexicalPath.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Environment.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>
#include <UI/SFOS/WaylandWindow.h>

namespace Ladybird {

class Application final : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

private:
    Application() = default;

    // Each run is a throwaway probe: never hand off to an already running instance.
    virtual bool should_coordinate_browser_process() const override { return false; }
    virtual bool should_use_temporary_profile_by_default() const override { return true; }

    // We only say how far the finger moved and when it lifted; the compositor makes the fling.
    virtual bool platform_reports_scroll_momentum() const override { return false; }
};

// A HeadlessWebView already knows how to run a page without a toolkit. This one also shows its frames in a window
// and turns one-finger touches into taps and scrolls.
class WindowedWebView final : public WebView::HeadlessWebView {
public:
    static NonnullOwnPtr<WindowedWebView> create(Core::AnonymousBuffer theme, WaylandWindow& window, double device_pixel_ratio)
    {
        auto view = adopt_own(*new WindowedWebView(move(theme), window, device_pixel_ratio));
        view->initialize_tab(Web::HTML::VisibilityState::Visible);
        view->handle_resize();
        view->set_has_system_focus(true);
        return view;
    }

private:
    // A finger has to travel this far, in device pixels, before a touch becomes a scroll rather than a tap.
    static constexpr int touch_slop = 24;

    WindowedWebView(Core::AnonymousBuffer theme, WaylandWindow& window, double device_pixel_ratio)
        : WebView::HeadlessWebView(move(theme), window.size().to_type<Web::DevicePixels>())
        , m_window(window)
    {
        m_device_pixel_ratio = device_pixel_ratio;
        m_frame_timer.start();

        on_ready_to_paint = [this] { present(); };
        m_window.on_frame_ready = [this] {
            if (m_has_unpresented_frame)
                present();
        };
        m_window.on_resize = [this](auto size) {
            reset_viewport_size(size.template to_type<Web::DevicePixels>());
        };
        m_window.on_touch = [this](auto phase, int id, auto position) {
            handle_touch(phase, id, position);
        };
    }

    void present()
    {
        if (!m_has_usable_bitmap || !m_front_bitmap.shared_image_buffer)
            return;
        auto bitmap = m_front_bitmap.shared_image_buffer->bitmap_if_present();
        if (!bitmap)
            return;

        m_has_unpresented_frame = !m_window.present(*bitmap, m_front_bitmap.last_painted_size.to_type<int>());
        if (m_has_unpresented_frame)
            return;

        ++m_frames_presented;
        if (auto elapsed = m_frame_timer.elapsed_milliseconds(); elapsed >= 1000) {
            if (s_log_frame_rate)
                warnln("ladybird-sfos: {} fps", m_frames_presented * 1000 / elapsed);
            m_frames_presented = 0;
            m_frame_timer.start();
        }
    }

    void send_mouse_event(Web::MouseEvent::Type type, Gfx::IntPoint position, Gfx::IntPoint wheel_delta = {}, Web::ScrollGesturePhase phase = Web::ScrollGesturePhase::None)
    {
        auto is_button_event = type == Web::MouseEvent::Type::MouseDown || type == Web::MouseEvent::Type::MouseUp;
        Web::MouseEvent event {};
        event.type = type;
        event.position = position.to_type<Web::DevicePixels>();
        event.screen_position = position.to_type<Web::DevicePixels>();
        if (is_button_event) {
            event.button = Web::UIEvents::MouseButton::Primary;
            event.click_count = 1;
        }
        if (type == Web::MouseEvent::Type::MouseDown)
            event.buttons = Web::UIEvents::MouseButton::Primary;
        // NB: Wheel deltas are in CSS pixels; touches arrive in device pixels.
        event.wheel_delta_x = wheel_delta.x() / m_device_pixel_ratio;
        event.wheel_delta_y = wheel_delta.y() / m_device_pixel_ratio;
        event.wheel_delta_precision = Web::WheelDeltaPrecision::Precise;
        event.scroll_gesture_phase = phase;
        enqueue_input_event(move(event));
    }

    void handle_touch(WaylandWindow::TouchPhase phase, int id, Gfx::IntPoint position)
    {
        using enum WaylandWindow::TouchPhase;

        if (phase == Down) {
            if (m_active_touch.has_value())
                return;
            m_active_touch = id;
            m_touch_start = m_touch_last = position;
            m_is_scrolling = false;
            return;
        }
        if (m_active_touch != id)
            return;

        if (phase == Motion) {
            auto travelled = position - m_touch_start;
            if (!m_is_scrolling && max(abs(travelled.x()), abs(travelled.y())) > touch_slop)
                m_is_scrolling = true;
            if (m_is_scrolling) {
                // The content follows the finger, so it scrolls the opposite way.
                send_mouse_event(Web::MouseEvent::Type::MouseWheel, position, m_touch_last - position, Web::ScrollGesturePhase::Ongoing);
                m_touch_last = position;
            }
            return;
        }

        m_active_touch.clear();
        if (m_is_scrolling) {
            send_mouse_event(Web::MouseEvent::Type::MouseWheel, position, {}, Web::ScrollGesturePhase::Ended);
            return;
        }
        send_mouse_event(Web::MouseEvent::Type::MouseMove, position);
        send_mouse_event(Web::MouseEvent::Type::MouseDown, position);
        send_mouse_event(Web::MouseEvent::Type::MouseUp, position);
    }

    static inline bool s_log_frame_rate = Core::Environment::has("LADYBIRD_SFOS_LOG_FPS"sv);

    WaylandWindow& m_window;
    bool m_has_unpresented_frame { false };

    Optional<int> m_active_touch;
    Gfx::IntPoint m_touch_start;
    Gfx::IntPoint m_touch_last;
    bool m_is_scrolling { false };

    Core::ElapsedTimer m_frame_timer;
    i64 m_frames_presented { 0 };
};

static double device_pixel_ratio_from_environment()
{
    // The J2 panel is 1032 device pixels wide; 2.5 gives a 413 CSS pixel viewport.
    if (auto value = Core::Environment::get("LADYBIRD_SFOS_DPR"sv); value.has_value()) {
        if (auto ratio = value->to_number<double>(); ratio.has_value() && *ratio >= 1)
            return *ratio;
    }
    return 2.5;
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    auto app = TRY(Ladybird::Application::create(arguments));
    auto const& browser_options = Ladybird::Application::browser_options();

    if (browser_options.headless_mode.has_value())
        return app->execute();

    auto window = TRY(Ladybird::WaylandWindow::create());
    window->on_disconnect = [] { Core::EventLoop::current().quit(1); };

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    auto view = Ladybird::WindowedWebView::create(move(theme), *window, Ladybird::device_pixel_ratio_from_environment());
    // NB: On a fresh profile the application puts its welcome page ahead of the URLs we were given. Every run here
    //     has a fresh profile, so take the last URL: ours if there is one, the welcome page otherwise.
    if (!browser_options.urls.is_empty())
        view->load(browser_options.urls.last());

    return app->execute();
}
