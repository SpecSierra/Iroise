/*
 * Pacific: Ladybird engine connection.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QByteArray>
#include <QHash>
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
    // The buffer behind these pixels is about to be unmapped; a frame that points at it must be dropped.
    virtual void engineBufferGone(const uchar* pixels) = 0;
};

// The one Ladybird engine process behind every tab, and the socket to it. The engine is started on first use and
// connects back to us; whatever is sent before it has connected is queued.
class LadybirdEngine : public QObject {
    Q_OBJECT

public:
    static LadybirdEngine* instance();

    quint32 addView(LadybirdEngineClient*, const QSize& size, qreal devicePixelRatio);
    void removeView(quint32 view);

    void send(quint32 view, lb_embed_message_type type, int a = 0, int b = 0, int c = 0, int d = 0,
        double scale = 0, const QByteArray& text = QByteArray());

    bool isConnected() const { return m_connection >= 0; }

signals:
    void connectedChanged();

private slots:
    void acceptEngine();
    void receive();

private:
    struct Mapping {
        dev_t device;
        ino_t inode;
        uchar* pixels;
        size_t length;
        quint64 lastUsed;
    };
    struct Queued {
        lb_embed_header header;
        QByteArray text;
    };

    explicit LadybirdEngine(QObject* parent = nullptr);
    ~LadybirdEngine() override;

    void start();
    void write(const lb_embed_header&, const QByteArray& text);
    void handleFrame(const lb_embed_header&, int fd);
    const uchar* mappingFor(int fd, size_t length);

    QProcess m_process;
    QString m_socketPath;
    int m_listener = -1;
    int m_connection = -1;
    QSocketNotifier* m_listenerNotifier = nullptr;
    QSocketNotifier* m_connectionNotifier = nullptr;
    QVector<Queued> m_queue;

    QHash<quint32, LadybirdEngineClient*> m_views;
    quint32 m_nextView = 1;
    QVector<Mapping> m_mappings;
    quint64 m_frameSerial = 0;
};
