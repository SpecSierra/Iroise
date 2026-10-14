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
#define LB_EMBED_TEXT_MAX 32768
#define LB_EMBED_PAGE_MESSAGE_MARKER "\001pacific"
#define LB_EMBED_FIRST_ENGINE_VIEW 0x40000000u

enum lb_embed_message_type {
    /* Silica app -> engine. Every message names a view. The app picks the numbers, except for views the engine
       opens itself (LB_EMBED_NEW_VIEW), whose numbers start at LB_EMBED_FIRST_ENGINE_VIEW. */
    LB_EMBED_LOAD = 1,        /* text = URL */
    LB_EMBED_TOUCH = 2,       /* a = lb_embed_touch_phase, b = touch id, c = x, d = y (device pixels) */
    LB_EMBED_RESIZE = 3,      /* a = width, b = height (device pixels), scale = device pixel ratio */
    LB_EMBED_GO_BACK = 4,
    LB_EMBED_GO_FORWARD = 5,
    LB_EMBED_RELOAD = 6,
    LB_EMBED_CREATE_VIEW = 7, /* a = width, b = height (device pixels), scale = device pixel ratio; c = 1 for a
                                 private view (its own cookies and storage, nothing kept). Views a page opens
                                 are private when the page's is. */
    LB_EMBED_DESTROY_VIEW = 8,
    LB_EMBED_SET_VISIBLE = 9, /* a = 1 for the view on screen, 0 for one in the background */
    LB_EMBED_STOP = 10,
    LB_EMBED_COMMIT_TEXT = 11,  /* text = what the on-screen keyboard committed; a = characters before the
                                   caret that it replaces */
    LB_EMBED_COMPOSE_TEXT = 12, /* text = what is being composed; empty to stop composing */
    LB_EMBED_KEY = 13,          /* a = lb_embed_key, sent as a press followed by a release */
    /* These two are for the whole engine, not one view. Both are off until the app says otherwise. */
    LB_EMBED_SET_AD_BLOCKING = 14,            /* a = 1 to block advertising and trackers */
    LB_EMBED_SET_COOKIE_BANNER_BLOCKING = 15, /* a = 1 to block cookie banners */
    LB_EMBED_SET_DARK_MODE = 16,              /* a = 1 for pages to prefer their dark colours */
    LB_EMBED_CLEAR_DATA = 24,                 /* a = lb_embed_clear bits */
    LB_EMBED_DOWNLOAD_START = 25,             /* text = URL of a file to save into the downloads folder */
    LB_EMBED_DOWNLOAD_CANCEL = 26,            /* a = the download's id */
    LB_EMBED_SET_BLOCKER_FILTERS = 27,        /* text = filter rules of the app's own, in the syntax of the
                                                 lists, applied on top of them */
    LB_EMBED_SET_SITE_OPTIONS = 30,           /* text = JSON {"userAgents": {host: name of one of the engine's
                                                 user agent presets}, "noScript": [hosts]}; a host also covers
                                                 its subdomains */
    LB_EMBED_SET_CERTIFICATE_EXCEPTIONS = 33, /* text = hosts, one per line, whose certificates the user chose
                                                 to trust although they could not be verified */
    LB_EMBED_GEOLOCATION_POSITION = 29,       /* a = the request's id (LB_EMBED_GEOLOCATION_START); b = 0, or why
                                                 there is no position (1 denied, 2 unavailable, 3 timed out);
                                                 text = JSON {"latitude", "longitude", "accuracy" (metres), and
                                                 if known "altitude", "altitudeAccuracy", "heading", "speed"} */
    /* Back to messages for one view. */
    LB_EMBED_RUN_SCRIPT = 17, /* text = a JavaScript expression, run in the page. If its value is a string, the
                                 string comes back in LB_EMBED_SCRIPT_RESULT; the app puts what it needs to tell
                                 answers apart into that string. */
    LB_EMBED_FIND = 18,       /* a = lb_embed_find; text = what to look for, when starting */
    LB_EMBED_SELECT_MENU_CLOSED = 19, /* a = the id of the chosen option, or -1 for none */
    LB_EMBED_DIALOG_CLOSED = 20,      /* a = 1 for OK, 0 for Cancel; b = the lb_embed_dialog being answered;
                                         text = what was typed into a prompt */
    LB_EMBED_PINCH = 21,      /* c = x, d = y of the point between the fingers (device pixels); scale = how
                                 far apart the fingers are now, over how far they were at the last message */
    LB_EMBED_FILE_PICKER_CLOSED = 22, /* text = the chosen files' paths, one per line; empty for none */
    LB_EMBED_COLOR_PICKED = 23,       /* a = 1 if a colour was chosen; text = it, as #rrggbb */
    LB_EMBED_PERMISSION_DECIDED = 28, /* a = the question's id (LB_EMBED_PERMISSION); b = 1 to grant */
    LB_EMBED_EXIT_FULLSCREEN = 32,    /* the user left fullscreen (LB_EMBED_FULLSCREEN) from the app's side */
    LB_EMBED_SET_DESKTOP_MODE = 31,   /* a = 1 for the view to present itself as a desktop browser, where no site
                                         option says otherwise */

    /* engine -> Silica app */
    LB_EMBED_FRAME = 101,   /* a, b = painted size; c, d = buffer size; e = row pitch in bytes; fd = dma-buf (BGRA);
                               text = the buffer's identity, a decimal number that names it for its lifetime */
    LB_EMBED_URL = 102,     /* text = current URL */
    LB_EMBED_TITLE = 103,   /* text = page title */
    LB_EMBED_LOADING = 104, /* a = 1 when a load starts, 0 when it finishes */
    LB_EMBED_HISTORY = 105, /* a = 1 if the view can go back, b = 1 if it can go forward */
    LB_EMBED_TEXT_INPUT = 106, /* a = 1 while an editable element has focus */
    LB_EMBED_CRASHED = 107,
    LB_EMBED_SCRIPT_RESULT = 108, /* text = the string an LB_EMBED_RUN_SCRIPT expression evaluated to */
    LB_EMBED_FIND_RESULT = 109,   /* a = which match is current, counted from 1; b = how many there are, -1 if
                                     not known */
    LB_EMBED_SELECT_MENU = 110,   /* text = JSON array of the options of a <select> the user opened:
                                     {"id", "label", "selected", "disabled"}, or {"group": label} for a heading.
                                     Answered with LB_EMBED_SELECT_MENU_CLOSED. */
    LB_EMBED_DIALOG = 111,        /* a = lb_embed_dialog; text = JSON {"message", "default"}. The page waits
                                     for LB_EMBED_DIALOG_CLOSED. */
    LB_EMBED_FILE_PICKER = 112,   /* a = 1 if several files may be chosen. Answered with
                                     LB_EMBED_FILE_PICKER_CLOSED. */
    LB_EMBED_COLOR_PICKER = 113,  /* text = the colour an <input type=color> has now, as #rrggbb. Answered
                                     with LB_EMBED_COLOR_PICKED. */
    LB_EMBED_PAGE_MESSAGE = 116,  /* text = what a script of the app's posted from the page, by calling
                                     console.log(LB_EMBED_PAGE_MESSAGE_MARKER, text). Any script in the page can
                                     post one too, so the app treats these as hints, never as proof. */
    LB_EMBED_PAGE_COLOR = 115,
    LB_EMBED_NEW_VIEW = 122,      /* the engine opened this view for a page (a link with a target, window.open);
                                     a = 1 to show it now. The app adopts it as a new tab and sends its size. */
    LB_EMBED_CLOSE_VIEW = 123,    /* the page asked to close its view (window.close) */
    LB_EMBED_COMMIT = 125,        /* the document a load started (LB_EMBED_LOADING) is now the view's: frames
                                     from here on are of the new page */
    LB_EMBED_FAVICON = 124,       /* text = the page's icon as a data:image/png;base64 URL, empty when it has none.
                                     An icon too large for one message is left out. */
    LB_EMBED_FULLSCREEN = 120,    /* a = 1 when the page went fullscreen (a video, usually), 0 when it left */
    LB_EMBED_MEDIA = 121,         /* a = 1 while the page plays sound; b = 1 while it holds a screen wake lock */
    LB_EMBED_PERMISSION = 117,    /* a = an id for the answer, LB_EMBED_PERMISSION_DECIDED; text = JSON {"name"
                                     ("geolocation" or "microphone"), "origin"}. The page waits for the answer. */    /* a = lb_embed_page_color; text = the colour as #rrggbb, empty when the page
                                     no longer names one */
    /* For the whole engine (view 0). Downloads start when a page navigates to something that is not a page, or
       on LB_EMBED_DOWNLOAD_START; either way the engine saves the file itself and only reports. */
    LB_EMBED_DOWNLOAD = 114,      /* a = the download's id; b = lb_embed_download_status;
                                     text = JSON {"url", "path", "received", "total" (-1 if not known), "error"} */
    LB_EMBED_GEOLOCATION_START = 118, /* a = a request id; b = 1 to keep reporting (a watch), 0 for one position.
                                         Answered with LB_EMBED_GEOLOCATION_POSITION. Only sent for pages the user
                                         let have their location. */
    LB_EMBED_GEOLOCATION_STOP = 119,  /* a = the request id that is no longer wanted */
};

enum lb_embed_page_color {
    LB_EMBED_PAGE_COLOR_THEME = 0,      /* <meta name=theme-color> */
    LB_EMBED_PAGE_COLOR_BACKGROUND = 1, /* what the page paints behind its content */
};

enum lb_embed_download_status {
    LB_EMBED_DOWNLOAD_IN_PROGRESS = 0,
    LB_EMBED_DOWNLOAD_COMPLETED = 1,
    LB_EMBED_DOWNLOAD_CANCELLED = 2,
    LB_EMBED_DOWNLOAD_FAILED = 3,
};

enum lb_embed_clear {
    LB_EMBED_CLEAR_CACHE = 1,
    LB_EMBED_CLEAR_SITE_DATA = 2, /* cookies and what sites stored */
};

enum lb_embed_find {
    LB_EMBED_FIND_START = 0,
    LB_EMBED_FIND_NEXT = 1,
    LB_EMBED_FIND_PREVIOUS = 2,
    LB_EMBED_FIND_END = 3,
};

enum lb_embed_dialog {
    LB_EMBED_DIALOG_ALERT = 0,
    LB_EMBED_DIALOG_CONFIRM = 1,
    LB_EMBED_DIALOG_PROMPT = 2,
};

enum lb_embed_touch_phase {
    LB_EMBED_TOUCH_DOWN = 0,
    LB_EMBED_TOUCH_MOTION = 1,
    LB_EMBED_TOUCH_UP = 2,
    LB_EMBED_TOUCH_CANCEL = 3, /* the touch ends without being a tap: the app made something else of it */
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
