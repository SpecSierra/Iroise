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
    /* Silica app -> engine. Every message names a view; the app picks the numbers. */
    LB_EMBED_LOAD = 1,        /* text = URL */
    LB_EMBED_TOUCH = 2,       /* a = lb_embed_touch_phase, b = touch id, c = x, d = y (device pixels) */
    LB_EMBED_RESIZE = 3,      /* a = width, b = height (device pixels), scale = device pixel ratio */
    LB_EMBED_GO_BACK = 4,
    LB_EMBED_GO_FORWARD = 5,
    LB_EMBED_RELOAD = 6,
    LB_EMBED_CREATE_VIEW = 7, /* a = width, b = height (device pixels), scale = device pixel ratio */
    LB_EMBED_DESTROY_VIEW = 8,
    LB_EMBED_SET_VISIBLE = 9, /* a = 1 for the view on screen, 0 for one in the background */
    LB_EMBED_STOP = 10,
    LB_EMBED_COMMIT_TEXT = 11,  /* text = what the on-screen keyboard committed; a = characters before the
                                   caret that it replaces */
    LB_EMBED_COMPOSE_TEXT = 12, /* text = what is being composed; empty to stop composing */
    LB_EMBED_KEY = 13,          /* a = lb_embed_key, sent as a press followed by a release */

    /* engine -> Silica app */
    LB_EMBED_FRAME = 101,   /* a, b = painted size; c, d = buffer size; e = row pitch in bytes; fd = dma-buf (BGRA) */
    LB_EMBED_URL = 102,     /* text = current URL */
    LB_EMBED_TITLE = 103,   /* text = page title */
    LB_EMBED_LOADING = 104, /* a = 1 when a load starts, 0 when it finishes */
    LB_EMBED_HISTORY = 105, /* a = 1 if the view can go back, b = 1 if it can go forward */
    LB_EMBED_TEXT_INPUT = 106, /* a = 1 while an editable element has focus */
    LB_EMBED_CRASHED = 107,
};

enum lb_embed_touch_phase {
    LB_EMBED_TOUCH_DOWN = 0,
    LB_EMBED_TOUCH_MOTION = 1,
    LB_EMBED_TOUCH_UP = 2,
};

enum lb_embed_key {
    LB_EMBED_KEY_BACKSPACE = 1,
    LB_EMBED_KEY_RETURN = 2,
    LB_EMBED_KEY_TAB = 3,
    LB_EMBED_KEY_LEFT = 4,
    LB_EMBED_KEY_RIGHT = 5,
    LB_EMBED_KEY_UP = 6,
    LB_EMBED_KEY_DOWN = 7,
    LB_EMBED_KEY_DELETE = 8,
    LB_EMBED_KEY_ESCAPE = 9,
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
    uint32_t view;
};
