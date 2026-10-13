/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibWebCommon/CSS/PreferredColorScheme.h>
#include <LibWebCommon/HTML/ColorPickerUpdateState.h>
#include <LibWebCommon/HTML/FileFilter.h>
#include <LibWebCommon/HTML/SelectItem.h>
#include <LibWebCommon/HTML/SelectedFile.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebView/Application.h>
#include <LibWebView/FileDownloader.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Menu.h>
#include <LibWebView/Settings.h>
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

    // Never hand off to an already running instance: a windowed run is a throwaway probe, and an embedded engine
    // belongs to the app that started it.
    virtual bool should_coordinate_browser_process() const override { return false; }
    // A windowed run leaves nothing behind. An embedded engine is somebody's browser: its settings, its filter
    // lists and its site data have to be there next time.
    virtual bool should_use_temporary_profile_by_default() const override { return !is_embedded(); }

public:
    static bool is_embedded() { return Core::Environment::has(StringView { LB_EMBED_SOCKET_ENVIRONMENT_VARIABLE, strlen(LB_EMBED_SOCKET_ENVIRONMENT_VARIABLE) }); }

private:

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
        Cancel,
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
        if (phase == Cancel)
            return;
        send_mouse_event(Web::MouseEvent::Type::MouseMove, position);
        send_mouse_event(Web::MouseEvent::Type::MouseDown, position);
        send_mouse_event(Web::MouseEvent::Type::MouseUp, position);
    }

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
        //     is news.
        if (download.status != WebView::FileDownloader::DownloadStatus::InProgress || download.is_private == WebView::IsPrivate::Yes)
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

    void start_download(StringView url_text)
    {
        auto url = WebView::sanitize_url(url_text);
        if (!url.has_value())
            return;
        auto name = url->basename();
        if (name.is_empty())
            name = "download"sv;
        auto destination = WebView::Application::the().default_path_for_downloaded_file(name);
        if (destination.is_error()) {
            warnln("ladybird-sfos: nowhere to save {}: {}", *url, destination.error());
            return;
        }
        WebView::Application::the().file_downloader().download_file(WebView::IsPrivate::No, *url, destination.release_value());
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

        view->on_request_file_picker = [this, id](Web::HTML::FileFilter const&, Web::HTML::AllowMultipleFiles allow_multiple_files) {
            m_host.send_numbers(id, LB_EMBED_FILE_PICKER, allow_multiple_files == Web::HTML::AllowMultipleFiles::Yes);
        };
        view->on_request_color_picker = [this, id](Color current_color) {
            auto text = current_color.to_string_without_alpha();
            m_host.send_text(id, LB_EMBED_COLOR_PICKER, text.bytes_as_string_view());
        };

        view->set_preferred_color_scheme(m_dark_mode ? Web::CSS::PreferredColorScheme::Dark : Web::CSS::PreferredColorScheme::Light);

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

    // The engine has Brave's adblock engine and a set of filter list subscriptions built in, all switched off
    // until someone asks. These turn the app's two switches into which of those lists are on.
    void apply_content_blocking()
    {
        auto& settings = WebView::Application::settings();

        // The lists for a language's sites are worth having only for someone who reads that language.
        static constexpr Array<Array<StringView, 2>, 9> language_lists { {
            { "fr"sv, "listeFR"sv },
            { "de"sv, "easyListGermany"sv },
            { "it"sv, "easyListItaly"sv },
            { "es"sv, "easyListSpanish"sv },
            { "nl"sv, "easyListDutch"sv },
            { "pl"sv, "easyListPolish"sv },
            { "pt"sv, "easyListPortuguese"sv },
            { "ru"sv, "ruAdList"sv },
            { "uk"sv, "ruAdList"sv },
        } };
        auto language = Core::Environment::get("LANG"sv).value_or({});

        for (auto list : { "easyList"sv, "easyPrivacy"sv, "adblockWarningRemoval"sv })
            settings.set_content_blocker_list_enabled(list, m_block_ads);
        for (auto const& entry : language_lists)
            settings.set_content_blocker_list_enabled(entry[1], m_block_ads && language.starts_with(entry[0]));
        settings.set_content_blocker_list_enabled("easyListCookie"sv, m_block_cookie_banners);
        settings.set_content_blocker_enabled(m_block_ads || m_block_cookie_banners);

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

    void handle(lb_embed_header const& header, StringView text)
    {
        if (header.type == LB_EMBED_SET_AD_BLOCKING || header.type == LB_EMBED_SET_COOKIE_BANNER_BLOCKING) {
            auto& flag = header.type == LB_EMBED_SET_AD_BLOCKING ? m_block_ads : m_block_cookie_banners;
            if (flag != (header.a != 0)) {
                flag = header.a != 0;
                apply_content_blocking();
            }
            return;
        }
        if (header.type == LB_EMBED_SET_DARK_MODE) {
            m_dark_mode = header.a != 0;
            for (auto& entry : m_views)
                entry.value->set_preferred_color_scheme(m_dark_mode ? Web::CSS::PreferredColorScheme::Dark : Web::CSS::PreferredColorScheme::Light);
            return;
        }
        if (header.type == LB_EMBED_DOWNLOAD_START) {
            start_download(text);
            return;
        }
        if (header.type == LB_EMBED_DOWNLOAD_CANCEL) {
            WebView::Application::the().file_downloader().cancel_download(static_cast<u64>(header.a));
            return;
        }
        if (header.type == LB_EMBED_CLEAR_DATA) {
            WebView::Application::ClearBrowsingDataOptions options;
            if (header.a & LB_EMBED_CLEAR_CACHE)
                options.delete_cached_files = WebView::Application::ClearBrowsingDataOptions::Delete::Yes;
            if (header.a & LB_EMBED_CLEAR_SITE_DATA)
                options.delete_site_data = WebView::Application::ClearBrowsingDataOptions::Delete::Yes;
            (void)WebView::Application::the().clear_browsing_data(options);
            return;
        }
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
            auto phase = PresentedWebView::TouchPhase::Up;
            if (header.a == LB_EMBED_TOUCH_DOWN)
                phase = PresentedWebView::TouchPhase::Down;
            else if (header.a == LB_EMBED_TOUCH_MOTION)
                phase = PresentedWebView::TouchPhase::Motion;
            else if (header.a == LB_EMBED_TOUCH_CANCEL)
                phase = PresentedWebView::TouchPhase::Cancel;
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
                    warnln("ladybird-sfos: cannot open the chosen file {}: {}", path, file.error());
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
    bool m_dark_mode { false };
    // The downloads the app knows of, and when it last heard of each.
    HashMap<u64, MonotonicTime> m_reported_downloads;
    RefPtr<Core::Timer> m_restart_list_updates_timer;
    RefPtr<Core::Timer> m_resume_list_updates_timer;
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
