/*
 * Pacific: the engine's downloads, handed to the interface's download manager.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QHash>
#include <QList>
#include <QObject>

#include <wpe/webkit.h>

// The engine saves a download itself, into the downloads folder, and reports how it is getting on. This turns
// those reports into the calls DownloadManager expects, which is what lists the download in the Sailfish
// transfer list.
class LadybirdDownloads : public QObject {
    Q_OBJECT

public:
    static LadybirdDownloads* instance();

    // For DownloadManager, through the stand-in WebKit API.
    WebKitDownload* start(const QByteArray& url);
    void cancel(WebKitDownload*);

public slots:
    void report(int engineId, int status, const QByteArray& json);

private:
    explicit LadybirdDownloads(QObject* parent);

    QHash<int, WebKitDownload*> m_downloads;
    // Asked for by the interface and not reported by the engine yet.
    QList<WebKitDownload*> m_requested;
};
