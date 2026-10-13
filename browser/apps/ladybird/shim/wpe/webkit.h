/*
 * Pacific: the WebKit download API, as far as DownloadManager uses it, on top of the Ladybird engine's downloads.
 *
 * DownloadManager was written against WebKit's download objects, and the interface (the Sailfish transfer list,
 * cancelling, browser.downloads) is written against DownloadManager. So a download the engine reports becomes one
 * of these objects, and DownloadManager stays as it was. See LadybirdDownloads.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QByteArray>
#include <stdint.h>

typedef char gchar;
typedef int gboolean;
typedef void* gpointer;
typedef uint64_t guint64;
#ifndef TRUE
#    define TRUE 1
#    define FALSE 0
#endif

// What the engine last said about one download.
struct _WebKitDownload {
    // The engine's number for it; -1 for one we asked for that the engine has not reported yet.
    int engineId = -1;
    QByteArray url;
    QByteArray mimeType;
    guint64 received = 0;
    guint64 total = 0;
    // Set once the interface cancelled it, after which the engine's reports about it are of no interest.
    bool cancelled = false;
};
typedef struct _WebKitDownload WebKitDownload;
// NB: A WebKit download has a request and a response object. Here both are the download itself.
typedef struct _WebKitDownload WebKitURIResponse;
typedef struct _WebKitDownload WebKitURIRequest;
typedef struct _WebKitNetworkSession WebKitNetworkSession;

void pacific_download_cancel(WebKitDownload*);
WebKitDownload* pacific_download_start(const gchar* url);

inline void webkit_download_cancel(WebKitDownload* download) { pacific_download_cancel(download); }
inline WebKitURIResponse* webkit_download_get_response(WebKitDownload* download) { return download; }
inline WebKitURIRequest* webkit_download_get_request(WebKitDownload* download) { return download; }
inline const gchar* webkit_uri_response_get_mime_type(WebKitURIResponse* response) { return response->mimeType.constData(); }
inline guint64 webkit_uri_response_get_content_length(WebKitURIResponse* response) { return response->total; }
inline const gchar* webkit_uri_request_get_uri(WebKitURIRequest* request) { return request->url.constData(); }
// The engine has already chosen where the file goes and is writing it.
inline void webkit_download_set_destination(WebKitDownload*, const gchar*) { }
inline void webkit_download_set_allow_overwrite(WebKitDownload*, gboolean) { }
inline double webkit_download_get_estimated_progress(WebKitDownload* download)
{
    return download->total ? static_cast<double>(download->received) / static_cast<double>(download->total) : 0;
}
inline guint64 webkit_download_get_received_data_length(WebKitDownload* download) { return download->received; }
inline WebKitNetworkSession* webkit_network_session_get_default() { return reinterpret_cast<WebKitNetworkSession*>(1); }
inline WebKitDownload* webkit_network_session_download_uri(WebKitNetworkSession*, const gchar* url) { return pacific_download_start(url); }
// NB: These objects are never freed: DownloadManager keeps their addresses as keys past the end of a download,
//     and one is a few dozen bytes.
inline gpointer g_object_ref(gpointer object) { return object; }
inline void g_object_unref(gpointer) { }
