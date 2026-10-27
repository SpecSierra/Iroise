/*
 * Iroise: the downloads of this run, as the engine reports them.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "DownloadModel.h"

#include "LadybirdEngine.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

DownloadModel* DownloadModel::instance()
{
    static DownloadModel* model = new DownloadModel(QCoreApplication::instance());
    return model;
}

DownloadModel::DownloadModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int DownloadModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_downloads.size();
}

QVariant DownloadModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_downloads.size())
        return QVariant();
    const Download& download = m_downloads.at(index.row());
    switch (role) {
    case DownloadIdRole:
        return download.id;
    case FileNameRole:
        return QFileInfo(download.path).fileName();
    case PathRole:
        return download.path;
    case UrlRole:
        return download.url;
    case ReceivedRole:
        return download.received;
    case TotalRole:
        return download.total;
    case ProgressRole:
        return download.total > 0 ? qreal(download.received) / qreal(download.total) : -1.0;
    case StatusRole:
        return download.status;
    case ErrorRole:
        return download.error;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> DownloadModel::roleNames() const
{
    return {
        { DownloadIdRole, "downloadId" },
        { FileNameRole, "fileName" },
        { PathRole, "path" },
        { UrlRole, "url" },
        { ReceivedRole, "received" },
        { TotalRole, "total" },
        { ProgressRole, "progress" },
        { StatusRole, "status" },
        { ErrorRole, "error" },
    };
}

int DownloadModel::activeCount() const
{
    int count = 0;
    for (const Download& download : m_downloads)
        count += download.status == InProgress;
    return count;
}

int DownloadModel::askingId() const
{
    for (const Download& download : m_unlisted) {
        if (download.decision == Decision::Asking)
            return download.id;
    }
    return -1;
}

QString DownloadModel::askingFileName() const
{
    for (const Download& download : m_unlisted) {
        if (download.decision == Decision::Asking)
            return QFileInfo(download.path).fileName();
    }
    return QString();
}

QString DownloadModel::askingHost() const
{
    for (const Download& download : m_unlisted) {
        if (download.decision == Decision::Asking)
            return QUrl(download.url).host();
    }
    return QString();
}

qint64 DownloadModel::askingSize() const
{
    for (const Download& download : m_unlisted) {
        if (download.decision == Decision::Asking)
            return download.total;
    }
    return -1;
}

void DownloadModel::setFolders(const QString& incoming, const QString& downloads)
{
    m_incoming = QDir(incoming).absolutePath();
    m_downloadsFolder = QDir(downloads).absolutePath();
    // What an earlier run left there, never answered (the app ended first). NB: Not what is recent: the engine may
    // take a download of the last run up again.
    const QDateTime dayAgo = QDateTime::currentDateTime().addDays(-1);
    for (const QFileInfo& file : QDir(m_incoming).entryInfoList(QDir::Files | QDir::Hidden)) {
        if (file.lastModified() < dayAgo)
            QFile::remove(file.absoluteFilePath());
    }
}

void DownloadModel::start(const QString& url, bool isPrivate)
{
    m_startedByUser.append(url);
    // A private tab's download goes out with the private session's cookies, not the user's own.
    LadybirdEngine::instance()->send(0, LB_EMBED_DOWNLOAD_START, isPrivate ? 1 : 0, 0, 0, 0, 0, url.toUtf8());
}

void DownloadModel::cancel(int downloadId)
{
    LadybirdEngine::instance()->send(0, LB_EMBED_DOWNLOAD_CANCEL, downloadId);
}

void DownloadModel::answer(bool save)
{
    int index = 0;
    while (index < m_unlisted.size() && m_unlisted.at(index).decision != Decision::Asking)
        ++index;
    if (index == m_unlisted.size())
        return;
    Download& download = m_unlisted[index];
    if (save) {
        download.decision = Decision::Save;
        Download saved = m_unlisted.takeAt(index);
        list(saved);
    } else {
        download.decision = Decision::Refused;
        if (download.status == InProgress) {
            // Its file goes once the engine says it stopped.
            cancel(download.id);
        } else {
            discard(download);
            m_unlisted.removeAt(index);
        }
    }
    emit askingChanged();
}

void DownloadModel::clearFinished()
{
    for (int row = m_downloads.size() - 1; row >= 0; --row) {
        if (m_downloads.at(row).status == InProgress)
            continue;
        beginRemoveRows(QModelIndex(), row, row);
        m_downloads.removeAt(row);
        endRemoveRows();
    }
}

DownloadModel::Download* DownloadModel::unlisted(int downloadId)
{
    for (Download& download : m_unlisted) {
        if (download.id == downloadId)
            return &download;
    }
    return nullptr;
}

int DownloadModel::rowOf(int downloadId) const
{
    for (int row = 0; row < m_downloads.size(); ++row) {
        if (m_downloads.at(row).id == downloadId)
            return row;
    }
    return -1;
}

// A download the user chose to save: on the list, and in the Downloads folder if it is already complete.
void DownloadModel::list(const Download& saved)
{
    beginInsertRows(QModelIndex(), 0, 0);
    m_downloads.prepend(saved);
    endInsertRows();
    Download& download = m_downloads.first();
    if (download.status == InProgress) {
        emit activeCountChanged();
        emit started(QFileInfo(download.path).fileName());
        return;
    }
    if (download.status == Completed)
        moveToDownloads(download);
    emit dataChanged(index(0), index(0));
    emit finished(QFileInfo(download.path).fileName(), download.path, download.status == Completed);
}

void DownloadModel::moveToDownloads(Download& download)
{
    if (m_incoming.isEmpty() || !download.path.startsWith(m_incoming + QLatin1Char('/')))
        return;
    const QFileInfo file(download.path);
    // NB: Never over a file the user has: "name (2).ext" and on.
    const QString base = file.completeBaseName();
    const QString suffix = file.suffix().isEmpty() ? QString() : QLatin1Char('.') + file.suffix();
    QString target = m_downloadsFolder + QLatin1Char('/') + file.fileName();
    for (int number = 2; QFileInfo::exists(target); ++number)
        target = m_downloadsFolder + QLatin1Char('/') + base + QStringLiteral(" (%1)").arg(number) + suffix;
    if (!QFile::rename(download.path, target)) {
        if (!QFile::copy(download.path, target)) {
            qWarning("DownloadModel: cannot move %s into %s", qPrintable(download.path), qPrintable(m_downloadsFolder));
            return;
        }
        QFile::remove(download.path);
    }
    download.path = target;
}

void DownloadModel::discard(const Download& download)
{
    if (!m_incoming.isEmpty() && download.path.startsWith(m_incoming + QLatin1Char('/')))
        QFile::remove(download.path);
}

void DownloadModel::report(int downloadId, int status, const QByteArray& json)
{
    const QJsonObject state = QJsonDocument::fromJson(json).object();
    auto update = [&](Download& download) {
        download.path = state.value(QStringLiteral("path")).toString();
        download.received = static_cast<qint64>(state.value(QStringLiteral("received")).toDouble());
        download.total = static_cast<qint64>(state.value(QStringLiteral("total")).toDouble(-1));
        download.error = state.value(QStringLiteral("error")).toString();
        download.status = status == LB_EMBED_DOWNLOAD_COMPLETED ? Completed
            : status == LB_EMBED_DOWNLOAD_CANCELLED             ? Cancelled
            : status == LB_EMBED_DOWNLOAD_FAILED                ? Failed
                                                                : InProgress;
    };

    const int row = rowOf(downloadId);
    if (row >= 0) {
        Download& download = m_downloads[row];
        const bool wasActive = download.status == InProgress;
        update(download);
        if (wasActive && download.status == Completed)
            moveToDownloads(download);
        emit dataChanged(index(row), index(row));
        if (wasActive && download.status != InProgress) {
            emit activeCountChanged();
            emit finished(QFileInfo(download.path).fileName(), download.path, download.status == Completed);
        }
        return;
    }

    Download* known = unlisted(downloadId);
    if (!known) {
        // NB: The engine only reports downloads that start in this run.
        Download download;
        download.id = downloadId;
        download.url = state.value(QStringLiteral("url")).toString();
        update(download);
        if (m_startedByUser.removeOne(download.url)) {
            download.decision = Decision::Save;
            list(download);
            return;
        }
        if (download.status != InProgress) {
            discard(download);
            return;
        }
        m_unlisted.append(download);
        emit askingChanged();
        return;
    }

    const bool asked = known->decision == Decision::Asking && known->id == askingId();
    const qint64 total = known->total;
    update(*known);
    if (known->status == InProgress) {
        if (asked && known->total != total)
            emit askingChanged();
        return;
    }
    // A complete one waits for the answer. One that failed, or that the page or the user stopped, is gone.
    if (known->status == Completed && known->decision == Decision::Asking)
        return;
    discard(*known);
    for (int i = 0; i < m_unlisted.size(); ++i) {
        if (m_unlisted.at(i).id == downloadId) {
            m_unlisted.removeAt(i);
            break;
        }
    }
    if (asked)
        emit askingChanged();
}
