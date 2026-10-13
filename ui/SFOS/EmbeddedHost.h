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

    void send_frame(u32 view, Gfx::LinuxDmaBufHandle const&, Gfx::IntSize painted_size);
    void send_text(u32 view, lb_embed_message_type, StringView, i32 a = 0);
    void send_numbers(u32 view, lb_embed_message_type, i32 a, i32 b = 0);
    void send_flags(u32 view, lb_embed_message_type, bool first, bool second = false);

    // Everything the app sends: the header, and the text that came with it.
    Function<void(lb_embed_header const&, StringView text)> on_message;
    Function<void()> on_disconnect;

private:
    EmbeddedHost() = default;

    void send(lb_embed_header const&, StringView text = {}, int fd = -1);
    void receive();

    int m_fd { -1 };
    RefPtr<Core::Notifier> m_notifier;
};

}
