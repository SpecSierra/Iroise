/*
 * Pacific: Ladybird engine connection.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <QSize>
#include <QVector>

#include <sys/types.h>

#include "../../../ui/protocol/EmbedProtocol.h"

class QSocketNotifier;

// A frame the engine finished: BGRA pixels in a buffer shared with the engine's GPU process.
struct LadybirdFrame {
    const uchar* pixels = nullptr;
    // The buffer itself (a dma-buf, ours for as long as the frame is valid), and a number that names it: the
    // engine paints its frames into the same few buffers over and over.
    int bufferFd = -1;
    quint64 bufferId = 0;
    int pitch = 0;
    QSize bufferSize;
    QSize paintedSize;
    quint64 serial = 0;
};

// What a view hears from the engine.
class LadybirdEngineClient {
public:
    virtual ~LadybirdEngineClient() { }
    virtual void engineFrame(const LadybirdFrame&) = 0;
    virtual void engineMessage(const lb_embed_header&, const QString& text) = 0;
    // This buffer is about to be unmapped and closed; a frame that points at it must be dropped.
    virtual void engineBufferGone(quint64 bufferId) = 0;
};

// The one Ladybird engine process behind every tab, and the socket to it. The engine is started on first use and
// connects back to us; whatever is sent before it has connected is queued.
class LadybirdEngine : public QObject {
    Q_OBJECT

public:
    static LadybirdEngine* instance();

    quint32 addView(LadybirdEngineClient*, const QSize& size, qreal devicePixelRatio, bool isPrivate);
    void removeView(quint32 view);
    // The app is ending: the engine is asked to go, and the socket removed.
    void shutdown();
    // The engine that had this view is gone: the view is made anew under another number (addView).
    void forgetView(quint32 view);

    void send(quint32 view, lb_embed_message_type type, int a = 0, int b = 0, int c = 0, int d = 0,
        double scale = 0, const QByteArray& text = QByteArray());

    bool isConnected() const { return m_connection >= 0; }

    // A view the engine opened for a page (LB_EMBED_NEW_VIEW) belongs to whoever claims it; what the engine says
    // about it until then is kept for them.
    void claimView(quint32 view, LadybirdEngineClient*);

signals:
    void connectedChanged();
    void viewOffered(quint32 view);
    // A new engine took over from one that went away; the views have to be made again.
    void restarted();

private slots:
    void acceptEngine();
    void receive();

private:
    struct Mapping {
        dev_t device;
        ino_t inode;
        quint64 identity;
        quint32 view;
        uchar* pixels;
        size_t length;
        int fd;
        quint64 id;
        quint64 lastUsed;
    };
    struct Queued {
        lb_embed_header header;
        QByteArray text;
    };

    explicit LadybirdEngine(QObject* parent = nullptr);
    ~LadybirdEngine() override;

    void start();
    void launch();
    void engineGone();
    static bool isSetting(lb_embed_message_type);
    void write(const lb_embed_header&, const QByteArray& text);
    void handleFrame(const lb_embed_header&, int fd, quint64 identity);
    const Mapping* mappingFor(quint32 view, int fd, size_t length, quint64 identity);

    QProcess m_process;
    QString m_socketPath;
    int m_listener = -1;
    int m_connection = -1;
    QSocketNotifier* m_listenerNotifier = nullptr;
    QSocketNotifier* m_connectionNotifier = nullptr;
    QVector<Queued> m_queue;
    // The last of each of the app's settings sent, for an engine started anew.
    QMap<int, Queued> m_settings;
    QVector<qint64> m_recentRestarts;
    bool m_restarting = false;
    bool m_gaveUp = false;

    QHash<quint32, LadybirdEngineClient*> m_views;
    QHash<quint32, QVector<QPair<lb_embed_header, QString>>> m_unclaimedViews;
    quint32 m_nextView = 1;
    QVector<Mapping> m_mappings;
    quint64 m_frameSerial = 0;
    quint64 m_nextBufferId = 1;
};
