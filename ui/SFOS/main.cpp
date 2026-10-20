/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/Format.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/File.h>
#include <LibCore/Environment.h>
#include <LibCore/GeolocationProvider.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibUnicode/Locale.h>
#include <LibWebCommon/CSS/PreferredColorScheme.h>
#include <LibWebCommon/Geolocation/GeolocationPositionErrorCode.h>
#include <LibWebCommon/HTML/AudioPlayState.h>
#include <LibWebCommon/HTML/ColorPickerUpdateState.h>
#include <LibWebCommon/HTML/FileFilter.h>
#include <LibWebCommon/HTML/SelectItem.h>
#include <LibWebCommon/HTML/SelectedFile.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebCommon/Page/ScreenWakeLockState.h>
#include <LibWebCommon/WebView/ConsoleOutput.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/FileDownloader.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Menu.h>
#include <LibWebView/Settings.h>
#include <LibWebView/URL.h>
#include <LibWebView/UserAgent.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentPage.h>
#include <UI/SFOS/EmbeddedHost.h>
#include <UI/SFOS/TouchEvents.h>

namespace Ladybird {

// The phone's language as pages want it: LANG=fr_FR.utf8 gives fr-FR, then fr. Empty for C/POSIX or a language the
// engine has no data for, which leaves the engine's own default (en-US, en).
static Vector<String> phone_languages()
{
    auto locale = Core::Environment::get("LANG"sv).value_or({});
    if (auto end = locale.find_any_of(".@"sv); end.has_value())
        locale = locale.substring_view(0, *end);
    if (locale.is_empty() || locale == "C"sv || locale == "POSIX"sv)
        return {};

    Vector<String> languages;
    auto tag = MUST(MUST(String::from_utf8(locale)).replace("_"sv, "-"sv, ReplaceMode::All));
    if (Unicode::is_locale_available(tag))
        languages.append(tag);
    if (auto separator = locale.find('_'); separator.has_value()) {
        auto language = MUST(String::from_utf8(locale.substring_view(0, *separator)));
        if (Unicode::is_locale_available(language))
            languages.append(move(language));
    }
    return languages;
}

class Application final : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

private:
    Application() = default;

    // Never hand off to an already running instance: an embedded engine belongs to the app that started it.
    virtual bool should_coordinate_browser_process() const override { return false; }
    // The engine is somebody's browser: its settings, its filter lists and its site data have to be there next time.
    virtual bool should_use_temporary_profile_by_default() const override { return false; }

    // We only say how far the finger moved and when it lifted; the compositor makes the fling.
    virtual bool platform_reports_scroll_momentum() const override { return false; }

    // A new profile starts on the phone's language rather than English.
    virtual Vector<String> platform_preferred_languages() const override { return phone_languages(); }
};

// A HeadlessWebView already knows how to run a page without a toolkit. This one also hands its frames to the Silica
// app that shows them and turns one-finger touches into taps and scrolls.
class PresentedWebView final : public WebView::HeadlessWebView {
public:
    enum class TouchPhase {
        Down,
        Motion,
        Up,
        Cancel,
    };

    // A view for the app's tab, or, given a traversable, for one a page opened.
    static NonnullOwnPtr<PresentedWebView> create(Core::AnonymousBuffer theme, Gfx::IntSize size, double device_pixel_ratio, WebView::IsPrivate is_private, Optional<WebView::CanonicalTraversable&> traversable = {})
    {
        auto view = adopt_own(*new PresentedWebView(move(theme), size, device_pixel_ratio, is_private));
        view->initialize_tab(Web::HTML::VisibilityState::Visible, traversable);
        view->handle_resize();
        view->set_has_system_focus(true);
        return view;
    }

    Function<void(Gfx::LinuxDmaBufHandle const&, Gfx::IntSize painted_size)> on_present;

    void resize(Gfx::IntSize size, double device_pixel_ratio)
    {
        m_device_pixel_ratio = device_pixel_ratio;
        reset_viewport_size(size.to_type<Web::DevicePixels>());
        traversable().for_each_hosting_page([&](WebView::WebContentPage& page) { send_screen_rect(page); });
    }

    // One finger, which the app passes on as it comes down, moves and lifts. It becomes one of three things:
    //  - on a page that listens for touches, touch events (see TouchEvents.h), and then the scroll or the tap the
    //    page did not cancel;
    //  - on a page in fullscreen, a mouse with its button held: it has nothing to scroll, and its player's controls
    //    are pressed and dragged;
    //  - otherwise a scroll when it travels, or the mouse events of a tap where it lifts.
    void handle_touch(TouchPhase phase, int id, Gfx::IntPoint position)
    {
        using enum TouchPhase;

        if (phase == Down) {
            if (m_active_touch.has_value())
                return;
            // A tap still waiting for the page's word is sent as it is (the page is slow to answer).
            send_pending_tap();
            m_active_touch = id;
            m_touch_start = m_touch_last = m_touch_position = position;
            m_is_scrolling = false;
            m_touch_mode = page_listens_for_touches ? TouchMode::TouchEvents : is_page_fullscreen ? TouchMode::Mouse : TouchMode::Classic;
            if (m_touch_mode == TouchMode::TouchEvents) {
                m_page_reported_start = false;
                m_page_owns_touch = false;
                m_awaiting_scroll = false;
                m_cancelable_moves_sent = 0;
                m_cancelable_moves_reported = 0;
                send_carrier(Web::MouseEvent::Type::MouseDown, position);
                m_carrier_held = true;
            } else if (m_touch_mode == TouchMode::Mouse) {
                send_mouse_event(Web::MouseEvent::Type::MouseMove, position);
                send_mouse_event(Web::MouseEvent::Type::MouseDown, position);
                m_mouse_held = true;
            }
            return;
        }
        if (m_active_touch != id)
            return;

        if (m_touch_mode == TouchMode::Mouse) {
            if (phase == Motion) {
                m_touch_last = position;
                send_mouse_event(Web::MouseEvent::Type::MouseMove, position);
                return;
            }
            m_active_touch.clear();
            m_mouse_held = false;
            // A cancelled touch (a second finger came) lets go of the button where the finger last was.
            send_mouse_event(Web::MouseEvent::Type::MouseUp, phase == Cancel ? m_touch_last : position);
            return;
        }

        if (m_touch_mode == TouchMode::TouchEvents) {
            if (phase == Motion) {
                m_touch_position = position;
                if (m_is_scrolling) {
                    // The content follows the finger, so it scrolls the opposite way.
                    send_mouse_event(Web::MouseEvent::Type::MouseWheel, position, m_touch_last - position, Web::ScrollGesturePhase::Ongoing);
                    m_touch_last = position;
                    return;
                }
                send_carrier(Web::MouseEvent::Type::MouseMove, position);
                ++m_cancelable_moves_sent;
                auto travelled = position - m_touch_start;
                if (!m_is_scrolling && !m_awaiting_scroll && !m_page_owns_touch && max(abs(travelled.x()), abs(travelled.y())) > touch_slop) {
                    // Whether it scrolls is the page's to say, by what it does with this touchmove.
                    m_awaiting_scroll = true;
                    m_cancelable_moves_needed = m_cancelable_moves_sent;
                    m_touch_answer_timer->restart();
                }
                scroll_if_page_allows();
                return;
            }
            m_active_touch.clear();
            if (m_carrier_held) {
                m_carrier_held = false;
                send_carrier(Web::MouseEvent::Type::MouseUp, position, phase == Cancel);
            }
            if (m_is_scrolling) {
                send_mouse_event(Web::MouseEvent::Type::MouseWheel, position, {}, Web::ScrollGesturePhase::Ended);
                m_is_scrolling = false;
                return;
            }
            // A finger that travelled was a gesture, of the page's or a scroll that never began; neither is a tap.
            if (phase == Cancel || m_page_owns_touch || m_awaiting_scroll) {
                m_awaiting_scroll = false;
                return;
            }
            m_pending_tap = position;
            m_touch_answer_timer->restart();
            return;
        }

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
        if (phase == Cancel)
            return;
        send_tap(position);
    }

    // What the page made of the touch events (TouchEvents.h): "start <cancelled>", "move <cancelled> <count>",
    // "end <cancelled>", or "listening" when it first listens for touches.
    // NB: Any script in the page can send these. They only decide what the page itself gets from the finger.
    void handle_touch_report(StringView report)
    {
        auto parts = report.split_view(' ');
        if (parts.is_empty())
            return;
        if (parts[0] == "listening"sv) {
            page_listens_for_touches = true;
            return;
        }
        if (m_touch_mode != TouchMode::TouchEvents || parts.size() < 2)
            return;
        bool cancelled = parts[1] == "1"sv;
        if (parts[0] == "start"sv) {
            m_page_reported_start = true;
            m_page_owns_touch |= cancelled;
        } else if (parts[0] == "move"sv && parts.size() >= 3) {
            m_page_owns_touch |= cancelled;
            m_cancelable_moves_reported = max(m_cancelable_moves_reported, parts[2].to_number<unsigned>().value_or(0));
        } else if (parts[0] == "end"sv) {
            if (m_pending_tap.has_value() && !cancelled && !m_page_owns_touch)
                send_tap(*m_pending_tap);
            m_pending_tap.clear();
        }
        if (m_page_owns_touch)
            m_awaiting_scroll = false;
        scroll_if_page_allows();
    }

    void page_committed()
    {
        // A new document listens for touches only once it says so.
        page_listens_for_touches = false;
        m_pending_tap.clear();
    }

    // Every page that comes to host the tab, also after a cross-site navigation moved it to another process.
    Function<void(WebView::WebContentPage&)> on_prepare_page;
    Gfx::IntSize size() const { return viewport_size().to_type<int>(); }
    bool desktop_mode { false };
    bool plays_audio { false };
    bool holds_wake_lock { false };
    bool is_page_fullscreen { false };
    // Whether the page has said it listens for touch events (see TouchEvents.h).
    bool page_listens_for_touches { false };

    void handle_pinch(Gfx::IntPoint position, double scale)
    {
        if (scale <= 0)
            return;
        Web::PinchEvent event {};
        event.position = position.to_type<Web::DevicePixels>();
        event.scale_delta = scale - 1.0;
        enqueue_input_event(move(event));
    }

private:
    // A finger has to travel this far, in device pixels, before a touch becomes a scroll rather than a tap.
    static constexpr int touch_slop = 24;

    PresentedWebView(Core::AnonymousBuffer theme, Gfx::IntSize size, double device_pixel_ratio, WebView::IsPrivate is_private)
        : WebView::HeadlessWebView(move(theme), size.to_type<Web::DevicePixels>(), is_private)
    {
        m_device_pixel_ratio = device_pixel_ratio;
        // NB: Before the first page is prepared, which hands the rate to WebContent and the Compositor.
        m_maximum_frames_per_second = s_refresh_rate;
        m_frame_timer.start();
        on_ready_to_paint = [this] { present(); };
    }

    // Runs for the tab's page and for every process hosting one of its cross-site frames.
    virtual void prepare_page_for_navigable(WebView::WebContentPage& page) override
    {
        WebView::HeadlessWebView::prepare_page_for_navigable(page);
        // The Compositor paces the page by the display's rate, which it only learns from the view (60 otherwise).
        WebView::Application::the().update_compositor_display_metadata(page.compositor_context_id(), display_id(), m_maximum_frames_per_second);
        send_screen_rect(page);
        if (on_prepare_page)
            on_prepare_page(page);
    }

    // The browser fills the phone's screen, so the view is the screen, turned as the view is. Without this pages see
    // the engine's default (1920x1080 device pixels, 768x432 here), and players that size their fullscreen video by
    // screen.width/height (YouTube) show a part of it, magnified.
    void send_screen_rect(WebView::WebContentPage& page)
    {
        Vector<Web::DevicePixelRect> screen_rects;
        screen_rects.append(Web::DevicePixelRect({}, viewport_size()));
        // The system keeps nothing of the screen for itself while the browser is in front.
        page.async_update_screen_rects(screen_rects, screen_rects, 0);
    }

    void present()
    {
        if (!m_has_usable_bitmap || !m_front_bitmap.shared_image_buffer || !on_present)
            return;
        auto const* dmabuf = m_front_bitmap.shared_image_buffer->linux_dmabuf_handle();
        if (!dmabuf)
            return;
        on_present(*dmabuf, m_front_bitmap.last_painted_size.to_type<int>());

        ++m_frames_presented;
        if (auto elapsed = m_frame_timer.elapsed_milliseconds(); elapsed >= 1000) {
            if (s_log_frame_rate)
                warnln("pacific: {} fps", m_frames_presented * 1000 / elapsed);
            m_frames_presented = 0;
            m_frame_timer.start();
        }
    }

    void send_tap(Gfx::IntPoint position)
    {
        send_mouse_event(Web::MouseEvent::Type::MouseMove, position);
        send_mouse_event(Web::MouseEvent::Type::MouseDown, position);
        send_mouse_event(Web::MouseEvent::Type::MouseUp, position);
    }

    void send_pending_tap()
    {
        if (auto tap = m_pending_tap; tap.has_value()) {
            m_pending_tap.clear();
            send_tap(*tap);
        }
    }

    // A press of the mouse that the page never sees as one: TouchEvents.h makes a touch event of it. Flagged, a
    // release makes touchcancel.
    void send_carrier(Web::MouseEvent::Type type, Gfx::IntPoint position, bool flagged = false)
    {
        Web::MouseEvent event {};
        event.type = type;
        event.position = position.to_type<Web::DevicePixels>();
        event.screen_position = position.to_type<Web::DevicePixels>();
        unsigned buttons = Pacific::touch_carrier_button | (flagged ? Pacific::touch_flag_button : 0);
        if (type == Web::MouseEvent::Type::MouseDown || type == Web::MouseEvent::Type::MouseUp) {
            event.button = Web::UIEvents::MouseButton::Primary;
            event.click_count = 1;
        }
        if (type != Web::MouseEvent::Type::MouseUp)
            buttons |= Web::UIEvents::MouseButton::Primary;
        event.buttons = static_cast<Web::UIEvents::MouseButton>(buttons);
        enqueue_input_event(move(event));
    }

    // Starts the scroll the finger asks for once the page has had its say on the touch, or has taken too long to.
    void scroll_if_page_allows(bool timed_out = false)
    {
        if (!m_awaiting_scroll || !m_active_touch.has_value())
            return;
        if (m_page_owns_touch) {
            m_awaiting_scroll = false;
            return;
        }
        if (!timed_out && (!m_page_reported_start || m_cancelable_moves_reported < m_cancelable_moves_needed))
            return;
        m_awaiting_scroll = false;
        m_is_scrolling = true;
        // NB: The page gets touchcancel once the scroll is the browser's, as it did in Chrome on Android. Touch
        //     events for the rest of the touch would wait on the page's script, and the scroll steps behind them
        //     (the engine takes input in turn), which leaves the scroll too slow at the end to fling.
        if (m_carrier_held) {
            m_carrier_held = false;
            send_carrier(Web::MouseEvent::Type::MouseUp, m_touch_position, true);
        }
        send_mouse_event(Web::MouseEvent::Type::MouseWheel, m_touch_position, m_touch_last - m_touch_position, Web::ScrollGesturePhase::Ongoing);
        m_touch_last = m_touch_position;
    }

    void touch_answer_timed_out()
    {
        if (m_awaiting_scroll)
            scroll_if_page_allows(true);
        // NB: The page is busy or the touch went to a frame that cannot answer: the tap still happens.
        send_pending_tap();
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
        if (type == Web::MouseEvent::Type::MouseDown || (type == Web::MouseEvent::Type::MouseMove && m_mouse_held))
            event.buttons = Web::UIEvents::MouseButton::Primary;
        // NB: Wheel deltas are in CSS pixels; touches arrive in device pixels.
        event.wheel_delta_x = wheel_delta.x() / m_device_pixel_ratio;
        event.wheel_delta_y = wheel_delta.y() / m_device_pixel_ratio;
        event.wheel_delta_precision = Web::WheelDeltaPrecision::Precise;
        event.scroll_gesture_phase = phase;
        enqueue_input_event(move(event));
    }

    static inline bool s_log_frame_rate = Core::Environment::has("LADYBIRD_SFOS_LOG_FPS"sv);
    // The screen's refresh rate, as the app found it; the engine assumes 60 otherwise.
    static inline double s_refresh_rate = [] {
        auto rate = Core::Environment::get("LADYBIRD_SFOS_REFRESH_RATE"sv).value_or({}).to_number<double>();
        return rate.has_value() && *rate >= 30 && *rate <= 240 ? *rate : 60.0;
    }();

    Optional<int> m_active_touch;
    Gfx::IntPoint m_touch_start;
    Gfx::IntPoint m_touch_last;
    bool m_is_scrolling { false };
    enum class TouchMode {
        Classic,
        Mouse,
        TouchEvents,
    };
    TouchMode m_touch_mode { TouchMode::Classic };
    // The finger's latest position, which a scroll the page has yet to allow catches up to.
    Gfx::IntPoint m_touch_position;
    // A mouse button held for a touch in fullscreen.
    bool m_mouse_held { false };
    // Touch events: what the page has said about this touch so far.
    // The press of the mouse that carries the touch events is down (see send_carrier).
    bool m_carrier_held { false };
    bool m_page_reported_start { false };
    bool m_page_owns_touch { false };
    bool m_awaiting_scroll { false };
    unsigned m_cancelable_moves_sent { 0 };
    unsigned m_cancelable_moves_needed { 0 };
    unsigned m_cancelable_moves_reported { 0 };
    Optional<Gfx::IntPoint> m_pending_tap;
    // How long the page may take to say what became of a touch before the front end goes on without it.
    RefPtr<Core::Timer> m_touch_answer_timer = Core::Timer::create_single_shot(250, [this] { touch_answer_timed_out(); });

    Core::ElapsedTimer m_frame_timer;
    i64 m_frames_presented { 0 };
};

static ErrorOr<Core::AnonymousBuffer> load_theme()
{
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    return Gfx::load_system_theme(theme_path.string());
}

// Positions come from the app, which reads the phone's location service. One provider serves every view; the
// views only ask it for pages the user let have their location (see EmbeddedViews::create_view).
class AppGeolocationProvider final : public Core::GeolocationProvider {
public:
    AK_ALLOC_WITH_KMALLOC;

    static inline EmbeddedHost* s_host { nullptr };
    static inline AppGeolocationProvider* s_the { nullptr };

    static ErrorOr<NonnullOwnPtr<Core::GeolocationProvider>> create() { return adopt_nonnull_own_or_enomem(new (nothrow) AppGeolocationProvider); }
    static bool is_available() { return true; }

    AppGeolocationProvider() { s_the = this; }
    virtual ~AppGeolocationProvider() override { s_the = nullptr; }

    virtual ErrorOr<RequestId, Core::GeolocationError> request_current_position(SuccessCallback on_success, ErrorCallback on_error) override
    {
        return start(move(on_success), move(on_error), false);
    }
    virtual ErrorOr<WatchId, Core::GeolocationError> start_watching_position(SuccessCallback on_success, ErrorCallback on_error) override
    {
        return start(move(on_success), move(on_error), true);
    }
    virtual void cancel_current_position_request(RequestId id) override { stop(id); }
    virtual void stop_watching_position(WatchId id) override { stop(id); }

    void position_arrived(i32 id, i32 error, StringView text)
    {
        // NB: Out of the map while it is called back: the page may stop the very request it is told about.
        auto request = m_requests.take(static_cast<u64>(id));
        if (!request.has_value())
            return;
        auto& callbacks = *request;
        m_delivering = static_cast<u64>(id);
        m_delivering_stopped = false;
        if (error) {
            auto type = error == 1 ? Core::GeolocationError::Type::PermissionDenied
                : error == 3       ? Core::GeolocationError::Type::Timeout
                                   : Core::GeolocationError::Type::PositionUnavailable;
            callbacks.on_error({ type, {} });
        } else if (auto json = JsonValue::from_string(text); !json.is_error() && json.value().is_object()) {
            auto const& position = json.value().as_object();
            Core::GeolocationCoordinates coordinates;
            coordinates.latitude = position.get_double_with_precision_loss("latitude"sv).value_or(0);
            coordinates.longitude = position.get_double_with_precision_loss("longitude"sv).value_or(0);
            coordinates.accuracy = position.get_double_with_precision_loss("accuracy"sv).value_or(0);
            coordinates.altitude = position.get_double_with_precision_loss("altitude"sv);
            coordinates.altitude_accuracy = position.get_double_with_precision_loss("altitudeAccuracy"sv);
            coordinates.heading = position.get_double_with_precision_loss("heading"sv);
            coordinates.speed = position.get_double_with_precision_loss("speed"sv);
            callbacks.on_success(coordinates);
        } else {
            // NB: Every request gets an answer, or the page waits on it for good.
            callbacks.on_error({ Core::GeolocationError::Type::PositionUnavailable, {} });
        }
        if (callbacks.is_watch && !m_delivering_stopped)
            m_requests.set(static_cast<u64>(id), move(callbacks));
        m_delivering = 0;
    }

private:
    struct Callbacks {
        SuccessCallback on_success;
        ErrorCallback on_error;
        bool is_watch { false };
    };

    ErrorOr<u64, Core::GeolocationError> start(SuccessCallback on_success, ErrorCallback on_error, bool is_watch)
    {
        if (!s_host)
            return Core::GeolocationError { Core::GeolocationError::Type::PositionUnavailable, {} };
        auto id = m_next_id++;
        m_requests.set(id, Callbacks { move(on_success), move(on_error), is_watch });
        s_host->send_numbers(0, LB_EMBED_GEOLOCATION_START, static_cast<i32>(id), is_watch);
        return id;
    }

    void stop(u64 id)
    {
        bool known = m_requests.remove(id);
        if (id == m_delivering) {
            m_delivering_stopped = true;
            known = true;
        }
        if (known && s_host)
            s_host->send_numbers(0, LB_EMBED_GEOLOCATION_STOP, static_cast<i32>(id));
    }

    HashMap<u64, Callbacks> m_requests;
    u64 m_next_id { 1 };
    u64 m_delivering { 0 };
    bool m_delivering_stopped { false };
};

// A Silica app shows our frames and sends us input over a socket. It has one view per tab.
class EmbeddedViews final : private WebView::FileDownloaderObserver {
public:
    explicit EmbeddedViews(EmbeddedHost& host, Core::AnonymousBuffer theme)
        : m_host(host)
        , m_theme(move(theme))
    {
        m_host.on_message = [this](lb_embed_header const& header, StringView text) { handle(header, text); };
    }

private:
    // The engine saves downloads itself; the app lists them and shows how far along they are.
    virtual void download_added(WebView::FileDownloader::Download const& download) override
    {
        // NB: Downloads from earlier runs are restored at start-up, finished ones included. Only what starts now
        //     is news. A private tab's download is shown too: its file is the user's to see and to stop, and the
        //     app keeps no list of downloads beyond the run.
        if (download.status != WebView::FileDownloader::DownloadStatus::InProgress)
            return;
        m_reported_downloads.set(download.id, MonotonicTime::now());
        report_download(download);
    }

    virtual void download_updated(WebView::FileDownloader::Download const& download) override
    {
        auto last_report = m_reported_downloads.get(download.id);
        if (!last_report.has_value())
            return;
        // NB: Progress arrives with every piece of the file that does; a few reports a second are plenty.
        auto now = MonotonicTime::now();
        if (WebView::FileDownloader::status_is_active(download.status)) {
            if (now - *last_report < AK::Duration::from_milliseconds(250))
                return;
            m_reported_downloads.set(download.id, now);
        } else {
            m_reported_downloads.remove(download.id);
        }
        report_download(download);
    }

    void report_download(WebView::FileDownloader::Download const& download)
    {
        using enum WebView::FileDownloader::DownloadStatus;
        auto status = LB_EMBED_DOWNLOAD_IN_PROGRESS;
        if (download.status == Completed)
            status = LB_EMBED_DOWNLOAD_COMPLETED;
        else if (download.status == Canceled)
            status = LB_EMBED_DOWNLOAD_CANCELLED;
        else if (download.status == Failed)
            status = LB_EMBED_DOWNLOAD_FAILED;

        JsonObject state;
        state.set("url"sv, download.url.serialize());
        state.set("path"sv, String::from_utf8_with_replacement_character(download.destination.string()));
        state.set("received"sv, download.downloaded_size);
        state.set("total"sv, download.total_size.has_value() ? static_cast<i64>(*download.total_size) : -1);
        state.set("error"sv, download.error.value_or({}));
        auto text = state.serialized();

        m_host.send_text(0, LB_EMBED_DOWNLOAD, text.bytes_as_string_view(), static_cast<i32>(download.id), status);
    }

    void start_download(StringView url_text, WebView::IsPrivate is_private)
    {
        auto url = WebView::sanitize_url(url_text);
        if (!url.has_value())
            return;
        auto name = url->basename();
        if (name.is_empty())
            name = "download"sv;
        auto destination = WebView::Application::the().default_path_for_downloaded_file(name);
        if (destination.is_error()) {
            warnln("pacific: nowhere to save {}: {}", *url, destination.error());
            return;
        }
        WebView::Application::the().file_downloader().download_file(is_private, *url, destination.release_value());
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

    void create_view(u32 id, Gfx::IntSize size, double device_pixel_ratio, WebView::IsPrivate is_private)
    {
        if (m_views.contains(id) || size.is_empty() || id >= LB_EMBED_FIRST_ENGINE_VIEW)
            return;
        add_view(id, PresentedWebView::create(m_theme, size, device_pixel_ratio, is_private));
    }

    PresentedWebView& add_view(u32 id, NonnullOwnPtr<PresentedWebView> view)
    {
        auto* raw_view = view.ptr();

        // A page opening a window gets a view of its own, which the app shows as a new tab.
        view->on_new_web_view = [this, raw_view](Web::HTML::ActivateTab activate, Web::HTML::WebViewHints, WebView::CanonicalTraversable& traversable) -> String {
            auto child_id = m_next_engine_view++;
            auto& child = add_view(child_id, PresentedWebView::create(m_theme, raw_view->size(), raw_view->device_pixel_ratio(), raw_view->is_private(), traversable));
            m_host.send_numbers(child_id, LB_EMBED_NEW_VIEW, activate == Web::HTML::ActivateTab::Yes);
            return child.handle();
        };
        view->on_close = [this, id] { m_host.send_numbers(id, LB_EMBED_CLOSE_VIEW, 0); };

        view->on_present = [this, id](Gfx::LinuxDmaBufHandle const& dmabuf, Gfx::IntSize painted_size) {
            m_host.send_frame(id, dmabuf, painted_size);
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
        view->on_top_level_navigation_commit = [this, id, raw_view] {
            raw_view->page_committed();
            m_host.send_flags(id, LB_EMBED_COMMIT, true);
        };
        view->on_input_method_state_change = [this, id, raw_view] {
            m_host.send_flags(id, LB_EMBED_TEXT_INPUT, raw_view->input_method_state().is_enabled);
        };
        view->on_web_content_crashed = [this, id](auto) { m_host.send_flags(id, LB_EMBED_CRASHED, true); };

        // The app's scripts in the page talk back through the console, the one way a page reaches us unasked.
        view->on_console_message = [this, id, raw_view](WebView::ConsoleOutput output) {
            auto const* log = output.output.get_pointer<WebView::ConsoleLog>();
            if (!log || log->arguments.size() != 2 || !log->arguments[0].is_string() || !log->arguments[1].is_string())
                return;
            if (log->arguments[0].as_string() == Pacific::touch_message_marker) {
                raw_view->handle_touch_report(log->arguments[1].as_string().bytes_as_string_view());
                return;
            }
            if (log->arguments[0].as_string() != LB_EMBED_PAGE_MESSAGE_MARKER ""sv)
                return;
            m_host.send_text(id, LB_EMBED_PAGE_MESSAGE, log->arguments[1].as_string().bytes_as_string_view());
        };
        view->on_received_js_console_result = [this, id](JsonValue result) {
            if (result.is_string())
                m_host.send_text(id, LB_EMBED_SCRIPT_RESULT, result.as_string().bytes_as_string_view());
        };
        view->on_find_in_page = [this, id](size_t current_match_index, Optional<size_t> const& total_match_count) {
            auto total = total_match_count.has_value() ? static_cast<i32>(*total_match_count) : -1;
            m_host.send_numbers(id, LB_EMBED_FIND_RESULT, total == 0 ? 0 : static_cast<i32>(current_match_index) + 1, total);
        };
        view->on_request_select_dropdown = [this, id](Gfx::IntPoint, i32, Vector<Web::HTML::SelectItem> items) {
            JsonArray options;
            auto add_option = [&](Web::HTML::SelectItemOption const& option) {
                JsonObject entry;
                entry.set("id"sv, option.id);
                entry.set("label"sv, option.label.to_utf8());
                entry.set("selected"sv, option.selected);
                entry.set("disabled"sv, option.disabled);
                options.must_append(move(entry));
            };
            for (auto const& item : items) {
                item.visit(
                    [&](Web::HTML::SelectItemOption const& option) { add_option(option); },
                    [&](Web::HTML::SelectItemOptionGroup const& group) {
                        JsonObject heading;
                        heading.set("group"sv, group.label.to_utf8());
                        options.must_append(move(heading));
                        for (auto const& option : group.items)
                            add_option(option);
                    },
                    [](Web::HTML::SelectItemSeparator const&) {});
            }
            auto text = options.serialized();
            m_host.send_text(id, LB_EMBED_SELECT_MENU, text.bytes_as_string_view());
        };

        // NB: A page that opens one of these waits for the answer, so each of them has to get one. The view we
        //     derive from only notes that a dialog is open, for WebDriver to answer.
        auto send_dialog = [this, id](lb_embed_dialog kind, Utf16String const& message, Utf16String const& default_text) {
            JsonObject dialog;
            dialog.set("message"sv, message.to_utf8());
            dialog.set("default"sv, default_text.to_utf8());
            auto text = dialog.serialized();
            m_host.send_text(id, LB_EMBED_DIALOG, text.bytes_as_string_view(), kind);
        };
        view->on_request_alert = [send_dialog](Utf16String const& message) { send_dialog(LB_EMBED_DIALOG_ALERT, message, {}); };
        view->on_request_confirm = [send_dialog](Utf16String const& message) { send_dialog(LB_EMBED_DIALOG_CONFIRM, message, {}); };
        view->on_request_prompt = [send_dialog](Utf16String const& message, Utf16String const& default_text) { send_dialog(LB_EMBED_DIALOG_PROMPT, message, default_text); };

        // The app paints the part of the screen it keeps the page out of (around a camera notch) in the page's colour.
        view->on_favicon_change = [this, id](Optional<Gfx::Bitmap const&> bitmap) {
            String url;
            if (bitmap.has_value()) {
                // NB: An icon too large for one message is left out rather than cut short.
                auto png = Gfx::PNGWriter::encode(*bitmap);
                if (png.is_error())
                    return;
                auto base64 = encode_base64(png.value());
                if (base64.is_error() || base64.value().bytes().size() + 32 > LB_EMBED_TEXT_MAX)
                    return;
                url = MUST(String::formatted("data:image/png;base64,{}", base64.value()));
            }
            m_host.send_text(id, LB_EMBED_FAVICON, url.bytes_as_string_view());
        };
        view->on_theme_color_change = [this, id](Gfx::Color color) {
            auto text = color.alpha() ? color.to_string_without_alpha() : String {};
            m_host.send_text(id, LB_EMBED_PAGE_COLOR, text.bytes_as_string_view(), LB_EMBED_PAGE_COLOR_THEME);
        };
        view->on_page_background_color_change = [this, id](Gfx::Color color) {
            auto text = color.to_string_without_alpha();
            m_host.send_text(id, LB_EMBED_PAGE_COLOR, text.bytes_as_string_view(), LB_EMBED_PAGE_COLOR_BACKGROUND);
        };

        view->on_request_file_picker = [this, id](Web::HTML::FileFilter const&, Web::HTML::AllowMultipleFiles allow_multiple_files) {
            m_host.send_numbers(id, LB_EMBED_FILE_PICKER, allow_multiple_files == Web::HTML::AllowMultipleFiles::Yes);
        };
        view->on_request_color_picker = [this, id](Color current_color) {
            auto text = current_color.to_string_without_alpha();
            m_host.send_text(id, LB_EMBED_COLOR_PICKER, text.bytes_as_string_view());
        };

        // The page goes fullscreen within the view it has; the app hides everything around it.
        view->on_fullscreen_window = [this, id, raw_view] {
            raw_view->set_is_fullscreen(Web::ViewportIsFullscreen::Yes);
            raw_view->is_page_fullscreen = true;
            m_host.send_flags(id, LB_EMBED_FULLSCREEN, true);
        };
        view->on_exit_fullscreen_window = [this, id, raw_view] {
            raw_view->set_is_fullscreen(Web::ViewportIsFullscreen::No);
            raw_view->is_page_fullscreen = false;
            m_host.send_flags(id, LB_EMBED_FULLSCREEN, false);
        };
        view->on_audio_play_state_changed = [this, id, raw_view](Web::HTML::AudioPlayState state) {
            raw_view->plays_audio = state == Web::HTML::AudioPlayState::Playing;
            m_host.send_flags(id, LB_EMBED_MEDIA, raw_view->plays_audio, raw_view->holds_wake_lock);
        };
        view->on_screen_wake_lock_state_changed = [this, id, raw_view](Web::ScreenWakeLockState state) {
            raw_view->holds_wake_lock = state == Web::ScreenWakeLockState::Acquired;
            m_host.send_flags(id, LB_EMBED_MEDIA, raw_view->plays_audio, raw_view->holds_wake_lock);
        };

        view->on_request_permission = [this, id](Utf16String const& name, URL::Origin const& origin, Function<void(bool)> on_decided) {
            ask_permission(id, name.to_utf8(), origin, move(on_decided));
        };

        // NB: The engine grants geolocation to any page that asks, and the view then wants a position from the
        //     provider. So the user is asked here, before the view's own handler runs.
        //     The question is about the tab's site, so a request from another page, which is a frame of another site
        //     (they have processes of their own), is refused: it would get what the user granted the tab's site.
        //     And a request the page took back while the user was being asked stays taken back.
        auto gate_geolocation = [this, id, raw_view](Function<void(WebView::WebContentPage&, u64)>& handler, Function<void(WebView::WebContentPage&, u64)>& cancel_handler) {
            auto native = adopt_ref(*new SharedGeolocationHandler(move(handler)));
            handler = [this, id, raw_view, native](WebView::WebContentPage& page, u64 request_id) {
                auto deny = [](WebView::WebContentPage& page, u64 request_id) {
                    page.async_geolocation_position_response(request_id, {}, to_underlying(Web::Geolocation::GeolocationPositionErrorCode::PermissionDenied));
                };
                if (&page != &raw_view->page()) {
                    deny(page, request_id);
                    return;
                }
                auto key = PendingGeolocation { &page, request_id };
                m_pending_geolocation.set(key);
                ask_permission(id, "geolocation"_string, raw_view->url().origin(), [this, native, deny, key, page = NonnullRefPtr(page), request_id](bool granted) {
                    if (!m_pending_geolocation.remove(key) || !page->is_open())
                        return;
                    if (granted)
                        native->call(*page, request_id);
                    else
                        deny(*page, request_id);
                });
            };
            auto native_cancel = adopt_ref(*new SharedGeolocationHandler(move(cancel_handler)));
            cancel_handler = [this, native_cancel](WebView::WebContentPage& page, u64 request_id) {
                if (m_pending_geolocation.remove(PendingGeolocation { &page, request_id }))
                    return;
                if (native_cancel->call)
                    native_cancel->call(page, request_id);
            };
        };
        gate_geolocation(view->on_request_geolocation_position, view->on_cancel_geolocation_position_request);
        gate_geolocation(view->on_start_geolocation_position_watch, view->on_stop_geolocation_position_watch);

        // NB: As early as we learn where the tab is going. A link to another site has asked for its document
        //     by then, so that one request goes out with the previous site's options.
        view->on_prepare_page = [this, raw_view](WebView::WebContentPage& page) { apply_site_options(*raw_view, page, raw_view->url()); };
        view->add_navigation_listener({
            .on_load_start = [this, raw_view](URL::URL const& url) { apply_site_options(*raw_view, raw_view->page(), url); },
            .on_load_finish = {},
        });


        for (auto* action : { &view->navigate_back_action(), &view->navigate_forward_action() }) {
            action->add_observer(make<HistoryObserver>(Function<void()> { [this, id, raw_view] {
                m_host.send_flags(id, LB_EMBED_HISTORY, raw_view->navigate_back_action().enabled(), raw_view->navigate_forward_action().enabled());
            } }));
        }

        m_views.set(id, move(view));
        return *raw_view;
    }

    // A geolocation request waiting for the user's answer.
    struct PendingGeolocation {
        WebView::WebContentPage* page { nullptr };
        u64 request_id { 0 };
        bool operator==(PendingGeolocation const&) const = default;
    };
    struct PendingGeolocationTraits : public DefaultTraits<PendingGeolocation> {
        static unsigned hash(PendingGeolocation const& key) { return pair_int_hash(ptr_hash(key.page), u64_hash(key.request_id)); }
    };
    HashTable<PendingGeolocation, PendingGeolocationTraits> m_pending_geolocation;

    // The view's own handler for a geolocation request, kept for when the user said yes.
    struct SharedGeolocationHandler final : RefCounted<SharedGeolocationHandler> {
        explicit SharedGeolocationHandler(Function<void(WebView::WebContentPage&, u64)> handler)
            : call(move(handler))
        {
        }
        Function<void(WebView::WebContentPage&, u64)> call;
    };

    void ask_permission(u32 view, String const& name, URL::Origin const& origin, Function<void(bool)> on_decided)
    {
        auto question = m_next_permission_question++;
        m_permission_answers.set(question, PermissionQuestion { view, move(on_decided) });
        JsonObject text;
        text.set("name"sv, name);
        text.set("origin"sv, origin.serialize());
        auto serialized = text.serialized();
        m_host.send_text(view, LB_EMBED_PERMISSION, serialized.bytes_as_string_view(), question);
    }

    static bool host_matches(StringView host, StringView entry)
    {
        return host == entry || (host.ends_with(entry) && host.length() > entry.length() && host[host.length() - entry.length() - 1] == '.');
    }

    // The user agent and scripting the site the view is going to gets. Everything else uses the engine's defaults.
    void apply_site_options(PresentedWebView& view, WebView::WebContentPage& page, URL::URL const& url)
    {
        // NB: data:, file: and about: URLs have no host, and asking for one aborts; they match no site.
        auto host = url.host().has_value() ? url.serialized_host().to_byte_string() : ByteString {};
        // The most specific entry wins: m.example.com over example.com.
        Optional<StringView> preset;
        size_t matched_length = 0;
        for (auto const& [site, name] : m_site_user_agents) {
            if (host_matches(host, site) && site.length() > matched_length) {
                preset = name;
                matched_length = site.length();
            }
        }
        if (!preset.has_value() && view.desktop_mode)
            preset = "Chrome Linux Desktop"sv;

        auto user_agent = Web::default_user_agent;
        auto compatibility_mode = "chrome"sv;
        if (preset.has_value()) {
            if (auto found = WebView::user_agents.get(*preset); found.has_value()) {
                user_agent = *found;
                compatibility_mode = preset->starts_with("Firefox"sv) ? "gecko"sv : preset->starts_with("Safari"sv) ? "webkit"sv : "chrome"sv;
            }
        }
        page.async_debug_request("spoof-user-agent"sv, user_agent);
        page.async_debug_request("navigator-compatibility-mode"sv, compatibility_mode);

        bool scripting = !any_of(m_no_script_sites, [&](auto const& site) { return host_matches(host, site); });
        page.async_debug_request("scripting"sv, scripting ? "on"sv : "off"sv);
    }

    // DuckDuckGo's autoconsent answers sites' cookie-consent dialogs with "reject" (scripts/fetch-autoconsent.sh).
    // The engine runs it in every http(s) document of every page, frames of other sites included, before the
    // document's own scripts (patch 0018), while cookie-banner blocking is on. Read once; empty if not shipped.
    static ByteString const& autoconsent_source()
    {
        static ByteString source = [] {
            auto path = LexicalPath::join(WebView::s_ladybird_resource_root, "pacific"sv, "autoconsent.js"sv);
            auto file = Core::File::open(path.string(), Core::File::OpenMode::Read);
            if (file.is_error()) {
                warnln("pacific: no autoconsent at {}: {}", path, file.error());
                return ByteString {};
            }
            auto contents = file.value()->read_until_eof();
            return contents.is_error() ? ByteString {} : ByteString { contents.value().bytes() };
        }();
        return source;
    }

    void apply_user_scripts()
    {
        Vector<ByteString> scripts;
        scripts.append(Pacific::touch_events_script);
        if (m_block_cookie_banners && !autoconsent_source().is_empty())
            scripts.append(autoconsent_source());
        WebView::Application::the().set_user_scripts(move(scripts));
    }

    void set_site_options(StringView text)
    {
        m_site_user_agents.clear();
        m_no_script_sites.clear();
        auto json = JsonValue::from_string(text);
        if (json.is_error() || !json.value().is_object())
            return;
        auto const& options = json.value().as_object();
        if (auto user_agents = options.get_object("userAgents"sv); user_agents.has_value()) {
            user_agents->for_each_member([&](String const& host, JsonValue const& name) {
                if (name.is_string())
                    m_site_user_agents.set(host.to_byte_string(), name.as_string().to_byte_string());
            });
        }
        if (auto hosts = options.get_array("noScript"sv); hosts.has_value()) {
            hosts->for_each([&](JsonValue const& host) {
                if (host.is_string())
                    m_no_script_sites.append(host.as_string().to_byte_string());
            });
        }
        for (auto& [id, view] : m_views)
            apply_site_options(*view, view->page(), view->url());
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

    // The engine has Brave's adblock engine and a set of filter list subscriptions built in, all switched off
    // until someone asks. These turn the app's two switches into which lists are on: the engine's own, and those
    // Atlantic used that it does not have, added once as subscriptions of our own.
    struct ExtraList {
        StringView name;
        StringView url;
    };
    static constexpr StringView ublock_lists_url = "https://raw.githubusercontent.com/uBlockOrigin/uAssets/master/filters/"sv;
    static constexpr StringView anti_circumvention_url = "https://gitlab.com/eyeo/anti-cv/abp-filters-anti-cv/-/raw/master/"sv;

    // uBlock Origin's lists, named by what their index files (filters.txt, annoyances.txt) include: the engine does
    // not follow !#include. Their scriptlets (+js) are not run, but their network and hiding rules are.
    static constexpr Array<ExtraList, 11> ad_lists { {
        { "uBlock filters"sv, "filters-general.txt"sv },
        { "uBlock filters 2020"sv, "filters-2020.txt"sv },
        { "uBlock filters 2021"sv, "filters-2021.txt"sv },
        { "uBlock filters 2022"sv, "filters-2022.txt"sv },
        { "uBlock filters 2023"sv, "filters-2023.txt"sv },
        { "uBlock filters 2024"sv, "filters-2024.txt"sv },
        { "uBlock filters 2025"sv, "filters-2025.txt"sv },
        { "uBlock filters 2026"sv, "filters-2026.txt"sv },
        { "uBlock filters for mobile"sv, "filters-mobile.txt"sv },
        { "uBlock annoyances"sv, "annoyances-others.txt"sv },
        { "Anti-circumvention"sv, "distribution/abp-filters-core.txt"sv },
    } };
    static constexpr ExtraList cookie_list { "uBlock cookie notices"sv, "annoyances-cookies.txt"sv };

    // A language's lists: the engine's list of its sites' ads, and eyeo's list against sites that defeat blockers in
    // it (english.txt, french.txt, ...). Both are worth having only for someone who reads that language.
    struct LanguageLists {
        StringView language;
        StringView engine_list;
        StringView anti_circumvention;
    };
    static constexpr Array<LanguageLists, 33> language_lists { {
        { "ar"sv, "listeAR"sv, "arabic"sv },
        { "be"sv, "ruAdList"sv, "russian"sv },
        { "bg"sv, "bulgarianList"sv, "bulgarian"sv },
        { "cs"sv, "easyListCzechAndSlovak"sv, "czech"sv },
        { "da"sv, "nordicFilters"sv, "nordic"sv },
        { "de"sv, "easyListGermany"sv, "german"sv },
        { "en"sv, {}, "english"sv },
        { "es"sv, "easyListSpanish"sv, "spanish"sv },
        { "fi"sv, "nordicFilters"sv, "nordic"sv },
        { "fr"sv, "listeFR"sv, "french"sv },
        { "he"sv, "easyListHebrew"sv, "hebrew"sv },
        { "hi"sv, "indianList"sv, "indian"sv },
        { "hu"sv, {}, "hungarian"sv },
        { "id"sv, "abpindo"sv, "indonesian"sv },
        { "is"sv, "nordicFilters"sv, "nordic"sv },
        { "it"sv, "easyListItaly"sv, "italian"sv },
        { "ko"sv, "koreanList"sv, {} },
        { "lt"sv, "easyListLithuania"sv, "lithuanian"sv },
        { "lv"sv, "latvianList"sv, "latvian"sv },
        { "ms"sv, "abpindo"sv, {} },
        { "nb"sv, "nordicFilters"sv, "nordic"sv },
        { "nl"sv, "easyListDutch"sv, "dutch"sv },
        { "nn"sv, "nordicFilters"sv, "nordic"sv },
        { "pl"sv, "easyListPolish"sv, "polish"sv },
        { "pt"sv, "easyListPortuguese"sv, "portuguese"sv },
        { "ro"sv, "roList"sv, "romanian"sv },
        { "ru"sv, "ruAdList"sv, "russian"sv },
        { "sk"sv, "easyListCzechAndSlovak"sv, "czech"sv },
        { "sv"sv, "nordicFilters"sv, "nordic"sv },
        { "ta"sv, "indianList"sv, "tamil"sv },
        { "tr"sv, {}, "turkish"sv },
        { "uk"sv, "ruAdList"sv, "russian"sv },
        { "vi"sv, "abpvn"sv, "vietnamese"sv },
    } };

    // A subscription of ours by its address: the one already in the settings, or a new one.
    static String subscription(StringView name, String const& url)
    {
        auto& settings = WebView::Application::settings();
        for (auto const& list : settings.content_blocker_lists()) {
            if (!list.built_in && list.url.has_value() && list.url->serialize() == url)
                return list.identifier;
        }
        return settings.add_content_blocker_list(MUST(String::from_utf8(name)), URL::Parser::basic_parse(url));
    }

    void apply_content_blocking()
    {
        auto& settings = WebView::Application::settings();

        auto locale = Core::Environment::get("LANG"sv).value_or({});
        auto language = locale.substring_view(0, min(locale.length(), locale.find('_').value_or(locale.length())));

        for (auto list : { "easyList"sv, "easyPrivacy"sv, "adblockWarningRemoval"sv, "fanboyAnnoyances"sv, "fanboySocial"sv })
            settings.set_content_blocker_list_enabled(list, m_block_ads);
        for (auto const& list : ad_lists) {
            auto base = list.url.starts_with("distribution/"sv) ? anti_circumvention_url : ublock_lists_url;
            settings.set_content_blocker_list_enabled(subscription(list.name, MUST(String::formatted("{}{}", base, list.url))), m_block_ads);
        }

        // NB: Every language's lists are switched, so that one left on by an earlier language goes off. Languages
        //     share some lists (the Nordic ones), so a list is on if the phone's language uses it. English sites
        //     are everybody's, so their anti-circumvention list is always on.
        auto uses = [&](auto member, StringView list) {
            return any_of(language_lists, [&](auto const& entry) { return entry.language == language && entry.*member == list; });
        };
        for (auto const& entry : language_lists) {
            if (!entry.engine_list.is_empty())
                settings.set_content_blocker_list_enabled(entry.engine_list, m_block_ads && uses(&LanguageLists::engine_list, entry.engine_list));
            if (entry.anti_circumvention.is_empty())
                continue;
            auto url = MUST(String::formatted("{}{}.txt", anti_circumvention_url, entry.anti_circumvention));
            bool on = m_block_ads && (entry.anti_circumvention == "english"sv || uses(&LanguageLists::anti_circumvention, entry.anti_circumvention));
            // Only a list someone needs is subscribed to at all.
            bool subscribed = any_of(settings.content_blocker_lists(), [&](auto const& list) { return list.url.has_value() && list.url->serialize() == url; });
            if (on || subscribed)
                settings.set_content_blocker_list_enabled(subscription(MUST(String::formatted("Anti-circumvention ({})", entry.anti_circumvention)), url), on);
        }

        settings.set_content_blocker_list_enabled("easyListCookie"sv, m_block_cookie_banners);
        settings.set_content_blocker_list_enabled(subscription(cookie_list.name, MUST(String::formatted("{}{}", ublock_lists_url, cookie_list.url))), m_block_cookie_banners);
        settings.set_content_blocker_enabled(m_block_ads || m_block_cookie_banners);
        apply_custom_filters();

        // NB: A list that was just switched on has never been downloaded. The engine fetches lists when updates
        //     are switched on, and then daily, so switching updates off and on again fetches what is missing
        //     (lists fetched within the last day are left alone). Switching off cancels a download in progress,
        //     and the engine only clears that up on a later turn of the event loop, where a request to start
        //     again would be dropped. So the two halves are spread over two timers, which also folds the app's
        //     two switches arriving together into one round of downloads.
        if (!m_restart_list_updates_timer) {
            m_restart_list_updates_timer = Core::Timer::create_single_shot(500, [this] {
                WebView::Application::settings().set_filter_list_updates_enabled(false);
                m_resume_list_updates_timer->restart();
            });
            m_resume_list_updates_timer = Core::Timer::create_single_shot(500, [this] {
                if (m_block_ads || m_block_cookie_banners)
                    WebView::Application::settings().set_filter_list_updates_enabled(true);
            });
        }
        m_resume_list_updates_timer->stop();
        m_restart_list_updates_timer->restart();
    }

    // Rules of our own for what the lists miss, found on the phone (Atlantic's atlantic-extra.txt), on top of the
    // app's per-site exceptions.
    static constexpr StringView extra_ad_filters =
        // jeuxvideo.com's ad library mutates the page on a ~74 Hz timer, forcing restyle, layout and repaint while
        // idle: forum threads scrolled at ~2 fps.
        "||getjad.io^\n"
        "||getjan.io^\n"
        // Reddit's "get the app" sheet, which the lists only hide with a selector the engine cannot honour.
        "reddit.com##.configured-xpromo\n"
        // A tap on a video went first to the site's own ad click tracker, which redirects to rotating casino
        // pages. $document, since untyped path rules do not match top-level navigations.
        "||pornhub.com/_xa/deep_click$document\n"
        "||youporn.com/_xa/deep_click$document\n"
        "||redtube.com/_xa/deep_click$document\n"
        "||tube8.com/_xa/deep_click$document\n"
        "||pornhub.com/_xa/deep_click\n"
        "||youporn.com/_xa/deep_click\n"
        "||redtube.com/_xa/deep_click\n"
        "||tube8.com/_xa/deep_click\n"
        "||epicrespin.com^\n"
        "||epicbet.com^\n"sv;

    void apply_custom_filters()
    {
        StringBuilder filters;
        if (m_block_ads)
            filters.append(extra_ad_filters);
        filters.append(m_app_filters);
        WebView::Application::settings().set_custom_content_blocker_filters(MUST(filters.to_string()));
    }

    void handle(lb_embed_header const& header, StringView text)
    {
        if (header.type == LB_EMBED_SET_AD_BLOCKING || header.type == LB_EMBED_SET_COOKIE_BANNER_BLOCKING) {
            auto& flag = header.type == LB_EMBED_SET_AD_BLOCKING ? m_block_ads : m_block_cookie_banners;
            if (flag != (header.a != 0)) {
                flag = header.a != 0;
                apply_content_blocking();
                // NB: Pages already open start or stop answering consent dialogs from their next document on.
                apply_user_scripts();
            }
            return;
        }
        if (header.type == LB_EMBED_SET_DARK_MODE) {
            m_dark_mode = header.a != 0;
            // NB: Every page that comes to host a tab gets the application's colour scheme again
            //     (Application::apply_view_options), so a view's own setting would last until its next navigation.
            //     The application's is only reachable through its colour scheme menu.
            auto wanted = m_dark_mode ? "Dark"sv : "Light"sv;
            for (auto& item : WebView::Application::the().color_scheme_menu().items()) {
                if (auto* action = item.get_pointer<NonnullRefPtr<WebView::Action>>(); action && (*action)->text() == wanted) {
                    (*action)->set_checked(true);
                    (*action)->activate();
                }
            }
            return;
        }
        if (header.type == LB_EMBED_SET_BLOCKER_FILTERS) {
            m_app_filters = String::from_utf8_with_replacement_character(text);
            apply_custom_filters();
            return;
        }
        if (header.type == LB_EMBED_DOWNLOAD_START) {
            start_download(text, header.a ? WebView::IsPrivate::Yes : WebView::IsPrivate::No);
            return;
        }
        if (header.type == LB_EMBED_DOWNLOAD_CANCEL) {
            WebView::Application::the().file_downloader().cancel_download(static_cast<u64>(header.a));
            return;
        }
        if (header.type == LB_EMBED_CLEAR_DATA) {
            // NB: Private views keep their session alive until they are gone; the next private tab starts afresh.
            if (header.a & LB_EMBED_CLEAR_PRIVATE_SESSION)
                WebView::Application::the().reset_private_browsing_session();
            if (!(header.a & (LB_EMBED_CLEAR_CACHE | LB_EMBED_CLEAR_SITE_DATA)))
                return;
            WebView::Application::ClearBrowsingDataOptions options;
            if (header.a & LB_EMBED_CLEAR_CACHE)
                options.delete_cached_files = WebView::Application::ClearBrowsingDataOptions::Delete::Yes;
            if (header.a & LB_EMBED_CLEAR_SITE_DATA)
                options.delete_site_data = WebView::Application::ClearBrowsingDataOptions::Delete::Yes;
            (void)WebView::Application::the().clear_browsing_data(options);
            return;
        }
        if (header.type == LB_EMBED_SET_CERTIFICATE_EXCEPTIONS) {
            Vector<ByteString> hosts;
            text.for_each_split_view('\n', SplitBehavior::Nothing, [&](StringView host) { hosts.append(host); });
            WebView::set_certificate_exceptions(move(hosts));
            return;
        }
        if (header.type == LB_EMBED_SET_SITE_OPTIONS) {
            set_site_options(text);
            return;
        }
        if (header.type == LB_EMBED_PERMISSION_DECIDED) {
            if (auto question = m_permission_answers.take(header.a); question.has_value())
                question->on_decided(header.b != 0);
            return;
        }
        if (header.type == LB_EMBED_GEOLOCATION_POSITION) {
            if (AppGeolocationProvider::s_the)
                AppGeolocationProvider::s_the->position_arrived(header.a, header.b, text);
            return;
        }
        if (header.type == LB_EMBED_CREATE_VIEW) {
            create_view(header.view, { header.a, header.b }, max(header.scale, 1.0), header.c ? WebView::IsPrivate::Yes : WebView::IsPrivate::No);
            return;
        }
        auto found = m_views.get(header.view);
        if (!found.has_value())
            return;
        auto& view = **found;

        switch (header.type) {
        case LB_EMBED_DESTROY_VIEW:
            // NB: Questions it still had will not be answered; their pages go with it.
            m_permission_answers.remove_all_matching([&](auto, auto const& question) { return question.view == header.view; });
            m_pending_geolocation.remove_all_matching([&](auto const& pending) { return pending.page == &view.page(); });
            m_views.remove(header.view);
            break;
        case LB_EMBED_LOAD:
            if (auto url = WebView::sanitize_url(text); url.has_value()) {
                apply_site_options(view, view.page(), *url);
                view.load(*url);
            }
            break;
        case LB_EMBED_TOUCH: {
            PresentedWebView::TouchPhase phase;
            if (header.a == LB_EMBED_TOUCH_DOWN)
                phase = PresentedWebView::TouchPhase::Down;
            else if (header.a == LB_EMBED_TOUCH_MOTION)
                phase = PresentedWebView::TouchPhase::Motion;
            else if (header.a == LB_EMBED_TOUCH_UP)
                phase = PresentedWebView::TouchPhase::Up;
            else if (header.a == LB_EMBED_TOUCH_CANCEL)
                phase = PresentedWebView::TouchPhase::Cancel;
            else
                break;
            view.handle_touch(phase, header.b, { header.c, header.d });
            break;
        }
        case LB_EMBED_RESIZE:
            if (header.a > 0 && header.b > 0)
                view.resize({ header.a, header.b }, max(header.scale, 1.0));
            break;
        case LB_EMBED_EXIT_FULLSCREEN:
            view.exit_fullscreen();
            break;
        case LB_EMBED_SET_DESKTOP_MODE:
            view.desktop_mode = header.a != 0;
            apply_site_options(view, view.page(), view.url());
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
        case LB_EMBED_RUN_SCRIPT:
            view.js_console_input(String::from_utf8_with_replacement_character(text));
            break;
        case LB_EMBED_FIND:
            switch (header.a) {
            case LB_EMBED_FIND_START:
                view.find_in_page(Utf16String::from_utf8(text), CaseSensitivity::CaseInsensitive, true);
                break;
            case LB_EMBED_FIND_NEXT:
                view.find_in_page_next_match();
                break;
            case LB_EMBED_FIND_PREVIOUS:
                view.find_in_page_previous_match();
                break;
            default:
                view.find_in_page_end();
                break;
            }
            break;
        case LB_EMBED_SELECT_MENU_CLOSED:
            view.select_dropdown_closed(header.a >= 0 ? Optional<u32> { static_cast<u32>(header.a) } : Optional<u32> {});
            break;
        case LB_EMBED_DIALOG_CLOSED:
            switch (header.b) {
            case LB_EMBED_DIALOG_ALERT:
                view.alert_closed();
                break;
            case LB_EMBED_DIALOG_CONFIRM:
                view.confirm_closed(header.a != 0);
                break;
            default:
                view.prompt_closed(header.a != 0 ? Optional<Utf16String> { Utf16String::from_utf8(text) } : Optional<Utf16String> {});
                break;
            }
            break;
        case LB_EMBED_FILE_PICKER_CLOSED: {
            Vector<Web::HTML::SelectedFile> files;
            text.for_each_split_view('\n', SplitBehavior::Nothing, [&](StringView path) {
                if (auto file = WebView::create_selected_file(path); !file.is_error())
                    files.append(file.release_value());
                else
                    warnln("pacific: cannot open the chosen file {}: {}", path, file.error());
            });
            view.file_picker_closed(move(files));
            break;
        }
        case LB_EMBED_COLOR_PICKED:
            view.color_picker_update(header.a ? Gfx::Color::from_string(text) : Optional<Gfx::Color> {}, Web::HTML::ColorPickerUpdateState::Closed);
            break;
        case LB_EMBED_PINCH:
            view.handle_pinch({ header.c, header.d }, header.scale);
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
    bool m_block_ads { false };
    bool m_block_cookie_banners { false };
    // The per-site exceptions the app sends (LB_EMBED_SET_BLOCKER_FILTERS).
    String m_app_filters;
    bool m_dark_mode { false };
    // The downloads the app knows of, and when it last heard of each.
    HashMap<u64, MonotonicTime> m_reported_downloads;
    RefPtr<Core::Timer> m_restart_list_updates_timer;
    RefPtr<Core::Timer> m_resume_list_updates_timer;
    // What to do with the user's answer to each question still open, and which view asked.
    struct PermissionQuestion {
        u32 view { 0 };
        Function<void(bool)> on_decided;
    };
    HashMap<i32, PermissionQuestion> m_permission_answers;
    // Sites the user gave a user agent of their own (host to preset name), and sites without scripts.
    HashMap<ByteString, ByteString> m_site_user_agents;
    Vector<ByteString> m_no_script_sites;
    i32 m_next_permission_question { 1 };
    u32 m_next_engine_view { LB_EMBED_FIRST_ENGINE_VIEW };
};

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    // NB: Before the application reads its settings, which only keep geolocation on with a provider in place.
    Core::GeolocationProvider::set_provider_functions(Ladybird::AppGeolocationProvider::create, Ladybird::AppGeolocationProvider::is_available);

    auto app = TRY(Ladybird::Application::create(arguments));

    auto socket_path = Core::Environment::get(StringView { LB_EMBED_SOCKET_ENVIRONMENT_VARIABLE, strlen(LB_EMBED_SOCKET_ENVIRONMENT_VARIABLE) });
    if (!socket_path.has_value())
        return Error::from_string_literal("The engine runs embedded by the browser (" LB_EMBED_SOCKET_ENVIRONMENT_VARIABLE ")");

    auto host = TRY(Ladybird::EmbeddedHost::connect(ByteString { *socket_path }));
    host->on_disconnect = [] {
        Ladybird::AppGeolocationProvider::s_host = nullptr;
        Core::EventLoop::current().quit(0);
    };
    Ladybird::AppGeolocationProvider::s_host = host.ptr();
    // The browser has no language setting of its own, so pages follow the phone, also in a profile made before the
    // engine asked for the phone's languages (those say English) and after the user changes the phone's language.
    if (auto languages = Ladybird::phone_languages(); !languages.is_empty() && languages != WebView::Application::settings().languages())
        WebView::Application::settings().set_languages(move(languages));
    // Pages still ask the user first, each time (see the gate in create_view).
    WebView::Application::settings().set_geolocation_enabled(true);
    // NB: The engine fetches a large download over several connections at once, in byte ranges. On a phone that
    //     gains little, and the last range sometimes never completes, which leaves the file unfinished for good.
    WebView::Application::settings().set_config_variable(WebView::ConfigVariableID::MaximumConnectionsPerDownload, 1);
    Ladybird::EmbeddedViews views { *host, TRY(Ladybird::load_theme()) };
    return app->execute();
}
