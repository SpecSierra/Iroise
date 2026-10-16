/*
 * Pacific: the downloads of this run, as the engine reports them.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "DownloadModel.h"

#include "LadybirdEngine.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

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

void DownloadModel::start(const QString& url, bool isPrivate)
{
    // A private tab's download goes out with the private session's cookies, not the user's own.
    LadybirdEngine::instance()->send(0, LB_EMBED_DOWNLOAD_START, isPrivate ? 1 : 0, 0, 0, 0, 0, url.toUtf8());
}

void DownloadModel::cancel(int downloadId)
{
    LadybirdEngine::instance()->send(0, LB_EMBED_DOWNLOAD_CANCEL, downloadId);
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

void DownloadModel::report(int downloadId, int status, const QByteArray& json)
{
    QJsonObject state = QJsonDocument::fromJson(json).object();
    int row = 0;
    while (row < m_downloads.size() && m_downloads.at(row).id != downloadId)
        ++row;
    const bool isNew = row == m_downloads.size();
    if (isNew) {
        // NB: The engine only reports downloads that start in this run.
        Download download;
        download.id = downloadId;
        download.url = state.value(QStringLiteral("url")).toString();
        beginInsertRows(QModelIndex(), 0, 0);
        m_downloads.prepend(download);
        endInsertRows();
        row = 0;
    }

    Download& download = m_downloads[row];
    const bool wasActive = download.status == InProgress;
    download.path = state.value(QStringLiteral("path")).toString();
    download.received = static_cast<qint64>(state.value(QStringLiteral("received")).toDouble());
    download.total = static_cast<qint64>(state.value(QStringLiteral("total")).toDouble(-1));
    download.error = state.value(QStringLiteral("error")).toString();
    download.status = status == LB_EMBED_DOWNLOAD_COMPLETED ? Completed
        : status == LB_EMBED_DOWNLOAD_CANCELLED             ? Cancelled
        : status == LB_EMBED_DOWNLOAD_FAILED                ? Failed
                                                            : InProgress;
    emit dataChanged(index(row), index(row));

    const QString fileName = QFileInfo(download.path).fileName();
    if (isNew) {
        emit activeCountChanged();
        emit started(fileName);
    }
    if (wasActive && download.status != InProgress) {
        emit activeCountChanged();
        emit finished(fileName, download.path, download.status == Completed);
    }
}
