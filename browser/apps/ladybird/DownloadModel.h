/*
 * Pacific: the downloads of this run, as the engine reports them.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QAbstractListModel>
#include <QVector>

// The engine saves every download into the downloads folder by itself and reports how it is getting on
// (LB_EMBED_DOWNLOAD). This lists them for the Downloads page, newest first.
class DownloadModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int activeCount READ activeCount NOTIFY activeCountChanged)

public:
    enum Status {
        InProgress,
        Completed,
        Cancelled,
        Failed,
    };
    Q_ENUM(Status)

    enum Role {
        DownloadIdRole = Qt::UserRole + 1,
        FileNameRole,
        PathRole,
        UrlRole,
        ReceivedRole,
        TotalRole,
        ProgressRole,
        StatusRole,
        ErrorRole,
    };

    static DownloadModel* instance();

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int activeCount() const;

    // Saves what is at url, as a link would that leads to something other than a page.
    Q_INVOKABLE void start(const QString& url);
    Q_INVOKABLE void cancel(int downloadId);
    // Forgets the finished ones (the files stay).
    Q_INVOKABLE void clearFinished();

    void report(int downloadId, int status, const QByteArray& json);

signals:
    void activeCountChanged();
    void started(const QString& fileName);
    void finished(const QString& fileName, const QString& path, bool completed);

private:
    explicit DownloadModel(QObject* parent);

    struct Download {
        int id = 0;
        QString path;
        QString url;
        qint64 received = 0;
        qint64 total = -1;
        Status status = InProgress;
        QString error;
    };
    QVector<Download> m_downloads;
};
