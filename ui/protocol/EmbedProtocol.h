/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* The wire format between the Ladybird engine process and the Silica app that embeds it.
 *
 * One message per SOCK_SEQPACKET datagram: this header, then text_length bytes of UTF-8. A frame message also
 * carries the frame's dma-buf as an SCM_RIGHTS file descriptor. Plain C so that the Qt 5.6 side (C++11) and the
 * engine side (C++23) can share it. Both ends run on the same machine, so there is no byte-order handling. */

#pragma once

#include <stdint.h>

#define LB_EMBED_SOCKET_ENVIRONMENT_VARIABLE "LADYBIRD_SFOS_EMBED_SOCKET"
#define LB_EMBED_TEXT_MAX 4096

enum lb_embed_message_type {
    /* Silica app -> engine */
    LB_EMBED_LOAD = 1,    /* text = URL */
    LB_EMBED_TOUCH = 2,   /* a = lb_embed_touch_phase, b = touch id, c = x, d = y (device pixels) */
    LB_EMBED_RESIZE = 3,  /* a = width, b = height (device pixels), scale = device pixel ratio */
    LB_EMBED_GO_BACK = 4,
    LB_EMBED_GO_FORWARD = 5,
    LB_EMBED_RELOAD = 6,

    /* engine -> Silica app */
    LB_EMBED_FRAME = 101,   /* a, b = painted size; c, d = buffer size; e = row pitch in bytes; fd = dma-buf (BGRA) */
    LB_EMBED_URL = 102,     /* text = current URL */
    LB_EMBED_TITLE = 103,   /* text = page title */
    LB_EMBED_LOADING = 104, /* a = 1 when a load starts, 0 when it finishes */
};

enum lb_embed_touch_phase {
    LB_EMBED_TOUCH_DOWN = 0,
    LB_EMBED_TOUCH_MOTION = 1,
    LB_EMBED_TOUCH_UP = 2,
};

struct lb_embed_header {
    uint32_t type;
    int32_t a;
    int32_t b;
    int32_t c;
    int32_t d;
    uint32_t e;
    double scale;
    uint32_t text_length;
    uint32_t reserved;
};
