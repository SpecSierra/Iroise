/*
 * Iroise: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Format.h>
#include <UI/SFOS/EmbeddedHost.h>

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace Ladybird {

ErrorOr<NonnullOwnPtr<EmbeddedHost>> EmbeddedHost::connect(ByteString const& socket_path)
{
    auto host = adopt_own(*new EmbeddedHost);

    host->m_fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (host->m_fd < 0)
        return Error::from_syscall("socket"sv, errno);

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (socket_path.length() >= sizeof(address.sun_path))
        return Error::from_string_literal("The embedding socket path is too long");
    memcpy(address.sun_path, socket_path.characters(), socket_path.length());
    if (::connect(host->m_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
        return Error::from_syscall("connect"sv, errno);

    host->m_notifier = Core::Notifier::construct(host->m_fd, Core::Notifier::Type::Read);
    host->m_notifier->on_activation = [host = host.ptr()] { host->receive(); };
    host->m_write_notifier = Core::Notifier::construct(host->m_fd, Core::Notifier::Type::Write);
    host->m_write_notifier->set_enabled(false);
    host->m_write_notifier->on_activation = [host = host.ptr()] { host->flush_pending(); };
    return host;
}

EmbeddedHost::~EmbeddedHost()
{
    if (m_notifier)
        m_notifier->close();
    if (m_write_notifier)
        m_write_notifier->close();
    if (m_fd >= 0)
        ::close(m_fd);
}

EmbeddedHost::Sent EmbeddedHost::write(lb_embed_header const& header, StringView text, int fd)
{
    Array<iovec, 2> parts {};
    parts[0].iov_base = const_cast<lb_embed_header*>(&header);
    parts[0].iov_len = sizeof(header);
    parts[1].iov_base = const_cast<char*>(text.characters_without_null_termination());
    parts[1].iov_len = text.length();

    msghdr message {};
    message.msg_iov = parts.data();
    message.msg_iovlen = text.is_empty() ? 1 : 2;

    alignas(cmsghdr) Array<char, CMSG_SPACE(sizeof(int))> control {};
    if (fd >= 0) {
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        auto* control_header = CMSG_FIRSTHDR(&message);
        control_header->cmsg_level = SOL_SOCKET;
        control_header->cmsg_type = SCM_RIGHTS;
        control_header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(control_header), &fd, sizeof(int));
    }

    // NB: Never wait: a blocked engine is worse than anything the app could miss.
    if (::sendmsg(m_fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT) >= 0)
        return Sent::Yes;
    if (errno == EAGAIN)
        return Sent::WouldBlock;
    warnln("iroise: sending to the embedding app failed: {}", strerror(errno));
    return Sent::Failed;
}

void EmbeddedHost::send(lb_embed_header const& header, StringView text, int fd)
{
    // A frame the app is too busy to take is left out; the next one replaces it. It also must not overtake a message
    // still waiting.
    if (header.type == LB_EMBED_FRAME) {
        if (m_pending.is_empty())
            (void)write(header, text, fd);
        return;
    }
    if (m_pending.is_empty() && write(header, text, fd) != Sent::WouldBlock)
        return;
    m_pending.append({ header, MUST(ByteBuffer::copy(text.bytes())) });
    m_write_notifier->set_enabled(true);
}

void EmbeddedHost::flush_pending()
{
    while (!m_pending.is_empty()) {
        auto const& pending = m_pending.first();
        if (write(pending.header, StringView { pending.text.bytes() }, -1) == Sent::WouldBlock)
            return;
        m_pending.take_first();
    }
    m_write_notifier->set_enabled(false);
}

void EmbeddedHost::send_frame(u32 view, Gfx::LinuxDmaBufHandle const& dmabuf, Gfx::IntSize painted_size)
{
    lb_embed_header header {};
    header.type = LB_EMBED_FRAME;
    header.view = view;
    header.a = painted_size.width();
    header.b = painted_size.height();
    header.c = dmabuf.size.width();
    header.d = dmabuf.size.height();
    // Our GLES buffers are single-plane BGRA in one dma-buf object (GLESContext.cpp).
    VERIFY(dmabuf.planes.size() == 1 && dmabuf.objects.size() == 1);
    header.e = dmabuf.planes.first().pitch;
    auto identity = ByteString::number(dmabuf.identity);
    header.text_length = static_cast<u32>(identity.length());
    send(header, identity, dmabuf.objects.first().fd());
}

void EmbeddedHost::send_numbers(u32 view, lb_embed_message_type type, i32 a, i32 b)
{
    lb_embed_header header {};
    header.type = type;
    header.view = view;
    header.a = a;
    header.b = b;
    send(header);
}

void EmbeddedHost::send_text(u32 view, lb_embed_message_type type, StringView text, i32 a, i32 b)
{
    lb_embed_header header {};
    header.type = type;
    header.view = view;
    header.a = a;
    header.b = b;
    if (text.length() > LB_EMBED_TEXT_MAX) {
        warnln("iroise: message {} is {} bytes, more than the app takes; not sent", to_underlying(type), text.length());
        return;
    }
    header.text_length = static_cast<u32>(text.length());
    send(header, text);
}

void EmbeddedHost::send_flags(u32 view, lb_embed_message_type type, bool first, bool second)
{
    lb_embed_header header {};
    header.type = type;
    header.view = view;
    header.a = first ? 1 : 0;
    header.b = second ? 1 : 0;
    send(header);
}

void EmbeddedHost::receive()
{
    struct {
        lb_embed_header header;
        char text[LB_EMBED_TEXT_MAX];
    } message;

    for (;;) {
        iovec part { &message, sizeof(message) };
        msghdr header {};
        header.msg_iov = &part;
        header.msg_iovlen = 1;
        auto length = ::recvmsg(m_fd, &header, MSG_DONTWAIT);
        if (length < 0 && errno == EAGAIN)
            return;
        if (length <= 0) {
            m_notifier->close();
            if (on_disconnect)
                on_disconnect();
            return;
        }
        if (static_cast<size_t>(length) < sizeof(lb_embed_header))
            continue;
        if (header.msg_flags & MSG_TRUNC) {
            warnln("iroise: dropped a message ({}) longer than the protocol allows", message.header.type);
            continue;
        }

        auto text_length = min(static_cast<size_t>(message.header.text_length), static_cast<size_t>(length) - sizeof(lb_embed_header));
        if (on_message)
            on_message(message.header, StringView { message.text, text_length });
    }
}

}
