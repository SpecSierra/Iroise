/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/StringView.h>
#include <LibCore/Notifier.h>
#include <LibGfx/Point.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/Size.h>
#include <UI/protocol/EmbedProtocol.h>

namespace Ladybird {

// The engine's end of the connection to the Silica app that shows our frames and sends us input.
class EmbeddedHost {
public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullOwnPtr<EmbeddedHost>> connect(ByteString const& socket_path);
    ~EmbeddedHost();

    void send_frame(Gfx::LinuxDmaBufHandle const&, Gfx::IntSize painted_size);
    void send_text(lb_embed_message_type, StringView);
    void send_loading(bool);

    Function<void(ByteString url)> on_load;
    Function<void(lb_embed_touch_phase, int id, Gfx::IntPoint)> on_touch;
    Function<void(Gfx::IntSize, double device_pixel_ratio)> on_resize;
    Function<void()> on_go_back;
    Function<void()> on_go_forward;
    Function<void()> on_reload;
    Function<void()> on_disconnect;

private:
    EmbeddedHost() = default;

    void send(lb_embed_header const&, StringView text = {}, int fd = -1);
    void receive();

    int m_fd { -1 };
    RefPtr<Core::Notifier> m_notifier;
};

}
