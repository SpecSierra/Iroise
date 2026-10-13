/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCore/Notifier.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Size.h>

struct wl_buffer;
struct wl_callback;
struct wl_compositor;
struct wl_display;
struct wl_output;
struct wl_registry;
struct wl_seat;
struct wl_shell;
struct wl_shell_surface;
struct wl_shm;
struct wl_surface;
struct wl_touch;

namespace Ladybird {

// One full-screen toplevel on the system compositor, fed from CPU bitmaps.
class WaylandWindow {
public:
    AK_ALLOC_WITH_KMALLOC;

    enum class TouchPhase {
        Down,
        Motion,
        Up,
    };

    static ErrorOr<NonnullOwnPtr<WaylandWindow>> create();
    ~WaylandWindow();

    Gfx::IntSize size() const { return m_size; }

    // Copies the bitmap into a free buffer and commits it. Returns false when the compositor has not asked for a
    // new frame yet; on_frame_ready fires once it has.
    bool present(Gfx::Bitmap const&, Gfx::IntSize painted_size);

    Function<void()> on_frame_ready;
    Function<void(Gfx::IntSize)> on_resize;
    Function<void(TouchPhase, int id, Gfx::IntPoint)> on_touch;
    Function<void()> on_disconnect;

    // Wayland listener entry points.
    void did_announce_global(wl_registry*, u32 name, char const* interface, u32 version);
    void did_receive_output_mode(int width, int height);
    void did_configure(int width, int height);
    void did_finish_frame();
    void did_release_buffer(wl_buffer*);
    void did_receive_seat_capabilities(u32 capabilities);
    void did_touch(TouchPhase, int id, Gfx::IntPoint);

private:
    struct Buffer {
        wl_buffer* buffer { nullptr };
        u8* data { nullptr };
        size_t length { 0 };
        bool busy { false };
    };

    WaylandWindow() = default;

    ErrorOr<void> connect();
    ErrorOr<void> allocate_buffers();
    void destroy_buffers();
    void dispatch();
    void resize(Gfx::IntSize);

    wl_display* m_display { nullptr };
    wl_registry* m_registry { nullptr };
    wl_compositor* m_compositor { nullptr };
    wl_shm* m_shm { nullptr };
    wl_shell* m_shell { nullptr };
    wl_seat* m_seat { nullptr };
    wl_output* m_output { nullptr };
    wl_touch* m_touch { nullptr };
    wl_surface* m_surface { nullptr };
    wl_shell_surface* m_shell_surface { nullptr };
    wl_callback* m_frame_callback { nullptr };

    RefPtr<Core::Notifier> m_notifier;
    Vector<Buffer> m_buffers;
    Gfx::IntSize m_size;
    Vector<Gfx::IntPoint> m_last_touch_positions;
};

}
