/*
 * Pacific: stand-ins for the WebKit download API.
 *
 * DownloadManager drove WebKit's download objects directly. The Ladybird engine protocol has no downloads yet, so
 * no download object ever exists and none of these is reached with one; they only keep DownloadManager, and the
 * interface built on it, compiling unchanged until downloads are wired to the engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

typedef char gchar;
typedef int gboolean;
typedef void* gpointer;
typedef uint64_t guint64;
#ifndef TRUE
#    define TRUE 1
#    define FALSE 0
#endif

typedef struct _WebKitDownload WebKitDownload;
typedef struct _WebKitURIResponse WebKitURIResponse;
typedef struct _WebKitURIRequest WebKitURIRequest;
typedef struct _WebKitNetworkSession WebKitNetworkSession;

inline void webkit_download_cancel(WebKitDownload*) { }
inline WebKitURIResponse* webkit_download_get_response(WebKitDownload*) { return nullptr; }
inline WebKitURIRequest* webkit_download_get_request(WebKitDownload*) { return nullptr; }
inline const gchar* webkit_uri_response_get_mime_type(WebKitURIResponse*) { return nullptr; }
inline guint64 webkit_uri_response_get_content_length(WebKitURIResponse*) { return 0; }
inline const gchar* webkit_uri_request_get_uri(WebKitURIRequest*) { return nullptr; }
inline void webkit_download_set_destination(WebKitDownload*, const gchar*) { }
inline void webkit_download_set_allow_overwrite(WebKitDownload*, gboolean) { }
inline double webkit_download_get_estimated_progress(WebKitDownload*) { return 0; }
inline guint64 webkit_download_get_received_data_length(WebKitDownload*) { return 0; }
inline WebKitNetworkSession* webkit_network_session_get_default() { return nullptr; }
inline WebKitDownload* webkit_network_session_download_uri(WebKitNetworkSession*, const gchar*) { return nullptr; }
inline gpointer g_object_ref(gpointer object) { return object; }
inline void g_object_unref(gpointer) { }
