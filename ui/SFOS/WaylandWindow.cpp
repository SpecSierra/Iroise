/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/StringView.h>
#include <LibGfx/Bitmap.h>
#include <UI/SFOS/WaylandWindow.h>

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

namespace Ladybird {

static constexpr size_t buffer_count = 3;

static WaylandWindow& window_from(void* data) { return *static_cast<WaylandWindow*>(data); }

static wl_registry_listener const s_registry_listener = {
    .global = [](void* data, wl_registry* registry, u32 name, char const* interface, u32 version) {
        window_from(data).did_announce_global(registry, name, interface, version);
    },
    .global_remove = [](void*, wl_registry*, u32) {},
};

static wl_output_listener const s_output_listener = {
    .geometry = [](void*, wl_output*, i32, i32, i32, i32, i32, char const*, char const*, i32) {},
    .mode = [](void* data, wl_output*, u32 flags, i32 width, i32 height, i32) {
        if (flags & WL_OUTPUT_MODE_CURRENT)
            window_from(data).did_receive_output_mode(width, height);
    },
    .done = [](void*, wl_output*) {},
    .scale = [](void*, wl_output*, i32) {},
    .name = [](void*, wl_output*, char const*) {},
    .description = [](void*, wl_output*, char const*) {},
};

static wl_shell_surface_listener const s_shell_surface_listener = {
    .ping = [](void*, wl_shell_surface* shell_surface, u32 serial) {
        wl_shell_surface_pong(shell_surface, serial);
    },
    .configure = [](void* data, wl_shell_surface*, u32, i32 width, i32 height) {
        window_from(data).did_configure(width, height);
    },
    .popup_done = [](void*, wl_shell_surface*) {},
};

static wl_callback_listener const s_frame_listener = {
    .done = [](void* data, wl_callback*, u32) {
        window_from(data).did_finish_frame();
    },
};

static wl_buffer_listener const s_buffer_listener = {
    .release = [](void* data, wl_buffer* buffer) {
        window_from(data).did_release_buffer(buffer);
    },
};

static wl_touch_listener const s_touch_listener = {
    .down = [](void* data, wl_touch*, u32, u32, wl_surface*, i32 id, wl_fixed_t x, wl_fixed_t y) {
        window_from(data).did_touch(WaylandWindow::TouchPhase::Down, id, { wl_fixed_to_int(x), wl_fixed_to_int(y) });
    },
    .up = [](void* data, wl_touch*, u32, u32, i32 id) {
        window_from(data).did_touch(WaylandWindow::TouchPhase::Up, id, {});
    },
    .motion = [](void* data, wl_touch*, u32, i32 id, wl_fixed_t x, wl_fixed_t y) {
        window_from(data).did_touch(WaylandWindow::TouchPhase::Motion, id, { wl_fixed_to_int(x), wl_fixed_to_int(y) });
    },
    .frame = [](void*, wl_touch*) {},
    .cancel = [](void*, wl_touch*) {},
    .shape = [](void*, wl_touch*, i32, wl_fixed_t, wl_fixed_t) {},
    .orientation = [](void*, wl_touch*, i32, wl_fixed_t) {},
};

static wl_seat_listener const s_seat_listener = {
    .capabilities = [](void* data, wl_seat*, u32 capabilities) {
        window_from(data).did_receive_seat_capabilities(capabilities);
    },
    .name = [](void*, wl_seat*, char const*) {},
};

ErrorOr<NonnullOwnPtr<WaylandWindow>> WaylandWindow::create()
{
    auto window = adopt_own(*new WaylandWindow);
    TRY(window->connect());
    return window;
}

WaylandWindow::~WaylandWindow()
{
    if (m_notifier)
        m_notifier->close();
    destroy_buffers();
    if (m_display)
        wl_display_disconnect(m_display);
}

ErrorOr<void> WaylandWindow::connect()
{
    m_display = wl_display_connect(nullptr);
    if (!m_display)
        return Error::from_string_literal("Could not connect to the Wayland compositor");

    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &s_registry_listener, this);
    // One round trip for the globals, one for what they announce (output mode, seat capabilities).
    wl_display_roundtrip(m_display);
    wl_display_roundtrip(m_display);

    if (!m_compositor || !m_shm || !m_shell)
        return Error::from_string_literal("The compositor lacks wl_compositor, wl_shm or wl_shell");
    if (m_size.is_empty())
        return Error::from_string_literal("The compositor did not report an output mode");

    m_surface = wl_compositor_create_surface(m_compositor);
    m_shell_surface = wl_shell_get_shell_surface(m_shell, m_surface);
    wl_shell_surface_add_listener(m_shell_surface, &s_shell_surface_listener, this);
    wl_shell_surface_set_title(m_shell_surface, "Ladybird");
    wl_shell_surface_set_class(m_shell_surface, "ladybird-sfos");
    wl_shell_surface_set_toplevel(m_shell_surface);

    TRY(allocate_buffers());

    m_notifier = Core::Notifier::construct(wl_display_get_fd(m_display), Core::Notifier::Type::Read);
    m_notifier->on_activation = [this] { dispatch(); };

    wl_display_flush(m_display);
    return {};
}

void WaylandWindow::dispatch()
{
    if (wl_display_dispatch(m_display) < 0) {
        warnln("ladybird-sfos: lost the Wayland connection: {}", strerror(errno));
        m_notifier->close();
        if (on_disconnect)
            on_disconnect();
        return;
    }
    wl_display_flush(m_display);
}

ErrorOr<void> WaylandWindow::allocate_buffers()
{
    auto stride = m_size.width() * 4;
    auto length = static_cast<size_t>(stride) * m_size.height();

    for (size_t i = 0; i < buffer_count; ++i) {
        int fd = memfd_create("ladybird-sfos-frame", MFD_CLOEXEC);
        if (fd < 0)
            return Error::from_syscall("memfd_create"sv, errno);
        if (ftruncate(fd, length) < 0) {
            close(fd);
            return Error::from_syscall("ftruncate"sv, errno);
        }
        auto* data = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            close(fd);
            return Error::from_syscall("mmap"sv, errno);
        }

        auto* pool = wl_shm_create_pool(m_shm, fd, length);
        auto* buffer = wl_shm_pool_create_buffer(pool, 0, m_size.width(), m_size.height(), stride, WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);

        wl_buffer_add_listener(buffer, &s_buffer_listener, this);
        m_buffers.append({ buffer, static_cast<u8*>(data), length, false });
    }
    return {};
}

void WaylandWindow::destroy_buffers()
{
    for (auto& buffer : m_buffers) {
        wl_buffer_destroy(buffer.buffer);
        munmap(buffer.data, buffer.length);
    }
    m_buffers.clear();
}

void WaylandWindow::resize(Gfx::IntSize size)
{
    if (size.is_empty() || size == m_size)
        return;
    m_size = size;
    if (!m_surface)
        return;

    destroy_buffers();
    if (auto result = allocate_buffers(); result.is_error()) {
        warnln("ladybird-sfos: could not allocate {}x{} buffers: {}", size.width(), size.height(), result.error());
        return;
    }
    if (on_resize)
        on_resize(m_size);
}

bool WaylandWindow::present(Gfx::Bitmap const& bitmap, Gfx::IntSize painted_size)
{
    if (m_frame_callback)
        return false;

    auto free_buffer = m_buffers.find_if([](auto const& buffer) { return !buffer.busy; });
    if (free_buffer.is_end())
        return false;

    auto width = min(min(bitmap.width(), painted_size.width()), m_size.width());
    auto height = min(min(bitmap.height(), painted_size.height()), m_size.height());
    auto stride = static_cast<size_t>(m_size.width()) * 4;

    // NB: The engine's BGRx/BGRA byte order is what Wayland calls XRGB8888 on little-endian machines.
    for (int y = 0; y < height; ++y)
        memcpy(free_buffer->data + y * stride, bitmap.scanline_u8(y), static_cast<size_t>(width) * 4);

    free_buffer->busy = true;
    wl_surface_attach(m_surface, free_buffer->buffer, 0, 0);
    wl_surface_damage(m_surface, 0, 0, m_size.width(), m_size.height());
    m_frame_callback = wl_surface_frame(m_surface);
    wl_callback_add_listener(m_frame_callback, &s_frame_listener, this);
    wl_surface_commit(m_surface);
    wl_display_flush(m_display);
    return true;
}

void WaylandWindow::did_announce_global(wl_registry* registry, u32 name, char const* interface, u32 version)
{
    StringView interface_name { interface, strlen(interface) };

    if (interface_name == "wl_compositor"sv) {
        m_compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 1));
    } else if (interface_name == "wl_shm"sv) {
        m_shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (interface_name == "wl_shell"sv) {
        m_shell = static_cast<wl_shell*>(wl_registry_bind(registry, name, &wl_shell_interface, 1));
    } else if (interface_name == "wl_seat"sv) {
        m_seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        wl_seat_add_listener(m_seat, &s_seat_listener, this);
    } else if (interface_name == "wl_output"sv && !m_output) {
        m_output = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, min(version, 2u)));
        wl_output_add_listener(m_output, &s_output_listener, this);
    }
}

void WaylandWindow::did_receive_output_mode(int width, int height)
{
    resize({ width, height });
}

void WaylandWindow::did_configure(int width, int height)
{
    resize({ width, height });
}

void WaylandWindow::did_finish_frame()
{
    wl_callback_destroy(m_frame_callback);
    m_frame_callback = nullptr;
    if (on_frame_ready)
        on_frame_ready();
}

void WaylandWindow::did_release_buffer(wl_buffer* released)
{
    for (auto& buffer : m_buffers) {
        if (buffer.buffer == released)
            buffer.busy = false;
    }
}

void WaylandWindow::did_receive_seat_capabilities(u32 capabilities)
{
    if ((capabilities & WL_SEAT_CAPABILITY_TOUCH) && !m_touch) {
        m_touch = wl_seat_get_touch(m_seat);
        wl_touch_add_listener(m_touch, &s_touch_listener, this);
    }
}

void WaylandWindow::did_touch(TouchPhase phase, int id, Gfx::IntPoint position)
{
    if (id < 0 || id > 15)
        return;
    if (m_last_touch_positions.size() <= static_cast<size_t>(id))
        m_last_touch_positions.resize(id + 1);

    // wl_touch.up carries no position, so report where the finger last was.
    if (phase == TouchPhase::Up)
        position = m_last_touch_positions[id];
    else
        m_last_touch_positions[id] = position;

    if (on_touch)
        on_touch(phase, id, position);
}

}
