/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
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
    return host;
}

EmbeddedHost::~EmbeddedHost()
{
    if (m_notifier)
        m_notifier->close();
    if (m_fd >= 0)
        ::close(m_fd);
}

void EmbeddedHost::send(lb_embed_header const& header, StringView text, int fd)
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

    // NB: A frame the app is too busy to take is worth less than a blocked engine, so never wait.
    if (::sendmsg(m_fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT) < 0 && errno != EAGAIN)
        warnln("ladybird-sfos: sending to the embedding app failed: {}", strerror(errno));
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
    header.e = static_cast<u32>(dmabuf.pitch);
    send(header, {}, dmabuf.file.fd());
}

void EmbeddedHost::send_text(u32 view, lb_embed_message_type type, StringView text)
{
    lb_embed_header header {};
    header.type = type;
    header.view = view;
    text = text.substring_view(0, min(text.length(), static_cast<size_t>(LB_EMBED_TEXT_MAX)));
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
        auto length = ::recv(m_fd, &message, sizeof(message), MSG_DONTWAIT);
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

        auto text_length = min(static_cast<size_t>(message.header.text_length), static_cast<size_t>(length) - sizeof(lb_embed_header));
        if (on_message)
            on_message(message.header, StringView { message.text, text_length });
    }
}

}
