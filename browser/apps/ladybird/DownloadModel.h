/*
 * Iroise: the downloads of this run, as the engine reports them.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QAbstractListModel>
#include <QStringList>
#include <QVector>

// The engine saves every download by itself, as soon as a page starts one, and reports how it is getting on
// (LB_EMBED_DOWNLOAD). It saves into a folder of the app's: the user is asked first (asking*, answer()), and only a
// download the user chose to save is listed here and goes into the Downloads folder once complete. One the user
// refused is stopped and its file deleted. A download the user started from the interface (start()) is not asked
// about. Newest first.
class DownloadModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int activeCount READ activeCount NOTIFY activeCountChanged)
    // The download the user is being asked about, -1 for none; then its file's name, the site it comes from, and
    // its size in bytes (-1 if not known).
    Q_PROPERTY(int askingId READ askingId NOTIFY askingChanged)
    Q_PROPERTY(QString askingFileName READ askingFileName NOTIFY askingChanged)
    Q_PROPERTY(QString askingHost READ askingHost NOTIFY askingChanged)
    Q_PROPERTY(qint64 askingSize READ askingSize NOTIFY askingChanged)

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
    int askingId() const;
    QString askingFileName() const;
    QString askingHost() const;
    qint64 askingSize() const;

    // Where the engine saves (the app's own folder), and where a download the user chose to save goes.
    void setFolders(const QString& incoming, const QString& downloads);

    // Saves what is at url, as a link would that leads to something other than a page. The user asked for it.
    Q_INVOKABLE void start(const QString& url, bool isPrivate);
    Q_INVOKABLE void cancel(int downloadId);
    // The user's answer about the download asked about (askingId).
    Q_INVOKABLE void answer(bool save);
    // Forgets the finished ones (the files stay).
    Q_INVOKABLE void clearFinished();

    void report(int downloadId, int status, const QByteArray& json);

signals:
    void activeCountChanged();
    void askingChanged();
    void started(const QString& fileName);
    void finished(const QString& fileName, const QString& path, bool completed);

private:
    explicit DownloadModel(QObject* parent);

    enum class Decision {
        Asking,
        Save,
        Refused,
    };
    struct Download {
        int id = 0;
        QString path; // the engine's file, then the one in the Downloads folder
        QString url;
        qint64 received = 0;
        qint64 total = -1;
        Status status = InProgress;
        QString error;
        Decision decision = Decision::Asking;
    };

    // A download the user is still being asked about, or refused and is not over yet.
    Download* unlisted(int downloadId);
    int rowOf(int downloadId) const;
    void list(const Download&);
    void moveToDownloads(Download&);
    void discard(const Download&);
    void askNext();

    QVector<Download> m_downloads;
    // In the order they came, the first is the one asked about.
    QVector<Download> m_unlisted;
    QStringList m_startedByUser;
    QString m_incoming;
    QString m_downloadsFolder;
};
