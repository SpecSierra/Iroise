/*
 * Pacific: the engine's downloads, handed to the interface's download manager.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LadybirdDownloads.h"

#include "LadybirdEngine.h"
#include "downloadmanager.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>

LadybirdDownloads* LadybirdDownloads::instance()
{
    static LadybirdDownloads* downloads = new LadybirdDownloads(QCoreApplication::instance());
    return downloads;
}

LadybirdDownloads::LadybirdDownloads(QObject* parent)
    : QObject(parent)
{
}

WebKitDownload* LadybirdDownloads::start(const QByteArray& url)
{
    WebKitDownload* download = new WebKitDownload;
    download->url = url;
    m_requested.append(download);
    LadybirdEngine::instance()->send(0, LB_EMBED_DOWNLOAD_START, 0, 0, 0, 0, 0, url);
    return download;
}

void LadybirdDownloads::cancel(WebKitDownload* download)
{
    if (!download || download->cancelled)
        return;
    download->cancelled = true;
    m_requested.removeAll(download);
    if (download->engineId >= 0) {
        LadybirdEngine::instance()->send(0, LB_EMBED_DOWNLOAD_CANCEL, download->engineId);
        m_downloads.remove(download->engineId);
    }
}

void LadybirdDownloads::report(int engineId, int status, const QByteArray& json)
{
    QJsonObject state = QJsonDocument::fromJson(json).object();
    QString path = state.value(QStringLiteral("path")).toString();
    DownloadManager* manager = DownloadManager::instance();

    WebKitDownload* download = m_downloads.value(engineId);
    if (!download) {
        // The first we hear of it. One that is already over is from an earlier run.
        if (status != LB_EMBED_DOWNLOAD_IN_PROGRESS)
            return;
        QByteArray url = state.value(QStringLiteral("url")).toString().toUtf8();
        for (WebKitDownload* requested : m_requested) {
            if (requested->url == url) {
                download = requested;
                break;
            }
        }
        // NB: The engine may spell the URL another way than it was asked with. Whoever asked gets the next
        //     download to start in that case, which with one request outstanding is the right one.
        if (!download && !m_requested.isEmpty())
            download = m_requested.first();
        bool requested = download;
        if (requested) {
            m_requested.removeAll(download);
        } else {
            download = new WebKitDownload;
            download->url = url;
        }
        download->engineId = engineId;
        download->mimeType = QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchExtension).name().toUtf8();
        qint64 total = static_cast<qint64>(state.value(QStringLiteral("total")).toDouble());
        download->total = total > 0 ? static_cast<guint64>(total) : 0;
        m_downloads.insert(engineId, download);
        manager->prepareDownload(download, QFileInfo(path).fileName(), path);
    }

    download->received = static_cast<guint64>(state.value(QStringLiteral("received")).toDouble());
    qint64 total = static_cast<qint64>(state.value(QStringLiteral("total")).toDouble());
    if (total > 0)
        download->total = static_cast<guint64>(total);

    switch (status) {
    case LB_EMBED_DOWNLOAD_IN_PROGRESS:
        manager->updateDownload(download);
        return;
    case LB_EMBED_DOWNLOAD_COMPLETED:
        manager->downloadFinished(download);
        break;
    case LB_EMBED_DOWNLOAD_CANCELLED:
        manager->downloadFailed(download, QStringLiteral("Cancelled"));
        break;
    default:
        manager->downloadFailed(download, state.value(QStringLiteral("error")).toString());
        break;
    }
    m_downloads.remove(engineId);
}

void pacific_download_cancel(WebKitDownload* download)
{
    LadybirdDownloads::instance()->cancel(download);
}

WebKitDownload* pacific_download_start(const gchar* url)
{
    return LadybirdDownloads::instance()->start(QByteArray(url));
}
