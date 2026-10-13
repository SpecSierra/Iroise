/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/HashMap.h>
#include <AK/LexicalPath.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Menu.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>
#include <UI/SFOS/EmbeddedHost.h>
#include <UI/SFOS/WaylandWindow.h>
#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <UI/SFOS/VulkanPresenter.h>
#endif

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

// A HeadlessWebView already knows how to run a page without a toolkit. This one also hands its frames to whoever
// shows them (our own window, or the Silica app that embeds us) and turns one-finger touches into taps and scrolls.
class PresentedWebView final : public WebView::HeadlessWebView {
public:
    enum class TouchPhase {
        Down,
        Motion,
        Up,
    };

    static NonnullOwnPtr<PresentedWebView> create(Core::AnonymousBuffer theme, Gfx::IntSize size, double device_pixel_ratio)
    {
        auto view = adopt_own(*new PresentedWebView(move(theme), size, device_pixel_ratio));
        view->initialize_tab(Web::HTML::VisibilityState::Visible);
        view->handle_resize();
        view->set_has_system_focus(true);
        return view;
    }

    // Shows a frame and says where it went, or returns an empty Optional if it cannot take one right now.
    Function<Optional<StringView>(Gfx::SharedImageBuffer const&, Gfx::IntSize painted_size)> on_present;

    void present_pending_frame()
    {
        if (m_has_unpresented_frame)
            present();
    }

    void resize(Gfx::IntSize size, double device_pixel_ratio)
    {
        m_device_pixel_ratio = device_pixel_ratio;
        reset_viewport_size(size.to_type<Web::DevicePixels>());
    }

    void handle_touch(TouchPhase phase, int id, Gfx::IntPoint position)
    {
        using enum TouchPhase;

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

private:
    // A finger has to travel this far, in device pixels, before a touch becomes a scroll rather than a tap.
    static constexpr int touch_slop = 24;

    PresentedWebView(Core::AnonymousBuffer theme, Gfx::IntSize size, double device_pixel_ratio)
        : WebView::HeadlessWebView(move(theme), size.to_type<Web::DevicePixels>())
    {
        m_device_pixel_ratio = device_pixel_ratio;
        m_frame_timer.start();
        on_ready_to_paint = [this] { present(); };
    }

    void present()
    {
        if (!m_has_usable_bitmap || !m_front_bitmap.shared_image_buffer || !on_present)
            return;

        auto shown_by = on_present(*m_front_bitmap.shared_image_buffer, m_front_bitmap.last_painted_size.to_type<int>());
        m_has_unpresented_frame = !shown_by.has_value();
        if (m_has_unpresented_frame)
            return;

        ++m_frames_presented;
        if (auto elapsed = m_frame_timer.elapsed_milliseconds(); elapsed >= 1000) {
            if (s_log_frame_rate)
                warnln("ladybird-sfos: {} fps from {}", m_frames_presented * 1000 / elapsed, *shown_by);
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

    static inline bool s_log_frame_rate = Core::Environment::has("LADYBIRD_SFOS_LOG_FPS"sv);

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

static ErrorOr<Core::AnonymousBuffer> load_theme()
{
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    return Gfx::load_system_theme(theme_path.string());
}

// Our own full-screen window: Vulkan when the frames are shared GPU images, wl_shm copies otherwise.
static ErrorOr<int> run_windowed(Application& app)
{
    auto window = TRY(WaylandWindow::create());
    window->on_disconnect = [] { Core::EventLoop::current().quit(1); };

    auto view = PresentedWebView::create(TRY(load_theme()), window->size(), device_pixel_ratio_from_environment());

#ifdef USE_VULKAN_DMABUF_IMAGES
    OwnPtr<VulkanPresenter> vulkan_presenter;
    if (!Core::Environment::has("LADYBIRD_SFOS_CPU_PRESENT"sv)) {
        if (auto presenter = VulkanPresenter::create(window->display(), window->surface(), window->size()); presenter.is_error())
            warnln("ladybird-sfos: no Vulkan presentation ({}), copying frames on the CPU", presenter.error());
        else
            vulkan_presenter = presenter.release_value();
    }
#endif

    view->on_present = [&](Gfx::SharedImageBuffer const& buffer, Gfx::IntSize painted_size) -> Optional<StringView> {
#ifdef USE_VULKAN_DMABUF_IMAGES
        if (auto const* dmabuf = buffer.linux_dmabuf_handle(); dmabuf && vulkan_presenter) {
            if (!vulkan_presenter->present(*dmabuf, painted_size).is_error())
                return "Vulkan"sv;
            warnln("ladybird-sfos: Vulkan presentation failed, copying frames on the CPU from now on");
            vulkan_presenter = nullptr;
        }
#endif
        // NB: A shared GPU image also has a bitmap, which is a mapping of the same memory.
        auto bitmap = buffer.bitmap_if_present();
        if (!bitmap)
            return {};
        auto size = Gfx::IntSize { min(bitmap->width(), painted_size.width()), min(bitmap->height(), painted_size.height()) };
        if (!window->present(bitmap->scanline_u8(0), bitmap->pitch(), size))
            return {};
        return "CPU copy"sv;
    };
    window->on_frame_ready = [&] { view->present_pending_frame(); };
    window->on_resize = [&](Gfx::IntSize size) {
#ifdef USE_VULKAN_DMABUF_IMAGES
        if (vulkan_presenter && vulkan_presenter->resize(size).is_error())
            vulkan_presenter = nullptr;
#endif
        view->resize(size, device_pixel_ratio_from_environment());
    };
    window->on_touch = [&](WaylandWindow::TouchPhase phase, int id, Gfx::IntPoint position) {
        using enum WaylandWindow::TouchPhase;
        auto view_phase = phase == Down ? PresentedWebView::TouchPhase::Down : phase == Motion ? PresentedWebView::TouchPhase::Motion
                                                                                                : PresentedWebView::TouchPhase::Up;
        view->handle_touch(view_phase, id, position);
    };

    // NB: On a fresh profile the application puts its welcome page ahead of the URLs we were given. Every run here
    //     has a fresh profile, so take the last URL: ours if there is one, the welcome page otherwise.
    if (auto const& urls = Application::browser_options().urls; !urls.is_empty())
        view->load(urls.last());

    return app.execute();
}

// No window of our own: a Silica app shows our frames and sends us input over a socket. It has one view per tab.
#ifdef USE_VULKAN_DMABUF_IMAGES
class EmbeddedViews {
public:
    explicit EmbeddedViews(EmbeddedHost& host, Core::AnonymousBuffer theme)
        : m_host(host)
        , m_theme(move(theme))
    {
        m_host.on_message = [this](lb_embed_header const& header, StringView text) { handle(header, text); };
    }

private:
    // The Action a view enables and disables as its history changes; we only relay that.
    struct HistoryObserver final : WebView::Action::Observer {
        explicit HistoryObserver(Function<void()> callback)
            : on_change(move(callback))
        {
        }
        virtual void on_enabled_state_changed(WebView::Action&) override { on_change(); }
        Function<void()> on_change;
    };

    void create_view(u32 id, Gfx::IntSize size, double device_pixel_ratio)
    {
        if (m_views.contains(id) || size.is_empty())
            return;
        auto view = PresentedWebView::create(m_theme, size, device_pixel_ratio);
        auto* raw_view = view.ptr();

        view->on_present = [this, id](Gfx::SharedImageBuffer const& buffer, Gfx::IntSize painted_size) -> Optional<StringView> {
            auto const* dmabuf = buffer.linux_dmabuf_handle();
            if (!dmabuf)
                return {};
            m_host.send_frame(id, *dmabuf, painted_size);
            return "embedding app"sv;
        };
        view->on_url_change = [this, id](URL::URL const& url) {
            auto text = url.serialize();
            m_host.send_text(id, LB_EMBED_URL, text.bytes_as_string_view());
        };
        view->on_title_change = [this, id](Utf16String const& title) {
            auto text = title.to_utf8();
            m_host.send_text(id, LB_EMBED_TITLE, text.bytes_as_string_view());
        };
        view->on_load_start = [this, id] { m_host.send_flags(id, LB_EMBED_LOADING, true); };
        view->on_load_finish = [this, id](URL::URL const&) { m_host.send_flags(id, LB_EMBED_LOADING, false); };
        view->on_input_method_state_change = [this, id, raw_view] {
            m_host.send_flags(id, LB_EMBED_TEXT_INPUT, raw_view->input_method_state().is_enabled);
        };
        view->on_web_content_crashed = [this, id](auto) { m_host.send_flags(id, LB_EMBED_CRASHED, true); };

        for (auto* action : { &view->navigate_back_action(), &view->navigate_forward_action() }) {
            action->add_observer(make<HistoryObserver>(Function<void()> { [this, id, raw_view] {
                m_host.send_flags(id, LB_EMBED_HISTORY, raw_view->navigate_back_action().enabled(), raw_view->navigate_forward_action().enabled());
            } }));
        }

        m_views.set(id, move(view));
    }

    void send_key(PresentedWebView& view, Web::UIEvents::KeyCode key, u32 code_point)
    {
        for (auto type : { Web::KeyEvent::Type::KeyDown, Web::KeyEvent::Type::KeyUp }) {
            Web::KeyEvent event {};
            event.type = type;
            event.key = key;
            event.code_point = code_point;
            event.should_insert_text = type == Web::KeyEvent::Type::KeyDown && code_point != 0;
            view.enqueue_input_event(move(event));
        }
    }

    void handle(lb_embed_header const& header, StringView text)
    {
        if (header.type == LB_EMBED_CREATE_VIEW) {
            create_view(header.view, { header.a, header.b }, header.scale >= 1 ? header.scale : device_pixel_ratio_from_environment());
            return;
        }
        auto found = m_views.get(header.view);
        if (!found.has_value())
            return;
        auto& view = **found;

        switch (header.type) {
        case LB_EMBED_DESTROY_VIEW:
            m_views.remove(header.view);
            break;
        case LB_EMBED_LOAD:
            if (auto url = WebView::sanitize_url(text); url.has_value())
                view.load(*url);
            break;
        case LB_EMBED_TOUCH: {
            auto phase = header.a == LB_EMBED_TOUCH_DOWN ? PresentedWebView::TouchPhase::Down : header.a == LB_EMBED_TOUCH_MOTION ? PresentedWebView::TouchPhase::Motion
                                                                                                                                 : PresentedWebView::TouchPhase::Up;
            view.handle_touch(phase, header.b, { header.c, header.d });
            break;
        }
        case LB_EMBED_RESIZE:
            if (header.a > 0 && header.b > 0)
                view.resize({ header.a, header.b }, header.scale >= 1 ? header.scale : device_pixel_ratio_from_environment());
            break;
        case LB_EMBED_GO_BACK:
            view.traverse_the_history_by_delta(-1);
            break;
        case LB_EMBED_GO_FORWARD:
            view.traverse_the_history_by_delta(1);
            break;
        case LB_EMBED_RELOAD:
            view.reload();
            break;
        case LB_EMBED_STOP:
            view.stop_loading();
            break;
        case LB_EMBED_SET_VISIBLE:
            view.set_system_visibility_state(header.a ? Web::HTML::VisibilityState::Visible : Web::HTML::VisibilityState::Hidden);
            view.set_has_system_focus(header.a != 0);
            break;
        case LB_EMBED_COMMIT_TEXT:
            view.commit_text_from_input_method(Utf16String::from_utf8(text), -header.a, header.a);
            break;
        case LB_EMBED_COMPOSE_TEXT:
            if (text.is_empty())
                view.unmark_text_from_input_method();
            else
                view.set_marked_text_from_input_method(Utf16String::from_utf8(text));
            break;
        case LB_EMBED_KEY:
            switch (header.a) {
            case LB_EMBED_KEY_BACKSPACE:
                send_key(view, Web::UIEvents::KeyCode::Key_Backspace, 0);
                break;
            case LB_EMBED_KEY_RETURN:
                send_key(view, Web::UIEvents::KeyCode::Key_Return, '\n');
                break;
            case LB_EMBED_KEY_TAB:
                send_key(view, Web::UIEvents::KeyCode::Key_Tab, '\t');
                break;
            case LB_EMBED_KEY_LEFT:
                send_key(view, Web::UIEvents::KeyCode::Key_Left, 0);
                break;
            case LB_EMBED_KEY_RIGHT:
                send_key(view, Web::UIEvents::KeyCode::Key_Right, 0);
                break;
            case LB_EMBED_KEY_UP:
                send_key(view, Web::UIEvents::KeyCode::Key_Up, 0);
                break;
            case LB_EMBED_KEY_DOWN:
                send_key(view, Web::UIEvents::KeyCode::Key_Down, 0);
                break;
            case LB_EMBED_KEY_DELETE:
                send_key(view, Web::UIEvents::KeyCode::Key_Delete, 0);
                break;
            case LB_EMBED_KEY_ESCAPE:
                send_key(view, Web::UIEvents::KeyCode::Key_Escape, 0);
                break;
            default:
                break;
            }
            break;
        default:
            break;
        }
    }

    EmbeddedHost& m_host;
    Core::AnonymousBuffer m_theme;
    HashMap<u32, NonnullOwnPtr<PresentedWebView>> m_views;
};
#endif

static ErrorOr<int> run_embedded(Application& app, ByteString const& socket_path)
{
#ifdef USE_VULKAN_DMABUF_IMAGES
    auto host = TRY(EmbeddedHost::connect(socket_path));
    host->on_disconnect = [] { Core::EventLoop::current().quit(0); };
    EmbeddedViews views { *host, TRY(load_theme()) };
    return app.execute();
#else
    (void)app;
    (void)socket_path;
    return Error::from_string_literal("Embedding needs a build with shared GPU images");
#endif
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    auto app = TRY(Ladybird::Application::create(arguments));

    if (Ladybird::Application::browser_options().headless_mode.has_value())
        return app->execute();

    if (auto socket_path = Core::Environment::get(StringView { LB_EMBED_SOCKET_ENVIRONMENT_VARIABLE, strlen(LB_EMBED_SOCKET_ENVIRONMENT_VARIABLE) }); socket_path.has_value())
        return Ladybird::run_embedded(*app, ByteString { *socket_path });

    return Ladybird::run_windowed(*app);
}
