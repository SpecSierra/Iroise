/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QProcess>
#include <QQuickItem>
#include <QVector>

#include <sys/types.h>

class QSGTexture;
class QSocketNotifier;
struct lb_embed_header;

// Shows a page rendered by a Ladybird engine process. The engine sends each finished frame as a dma-buf, which is
// mapped here and uploaded to a texture in the Qt scene graph, so Silica chrome can be drawn around and over it.
class LadybirdView : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString url READ url NOTIFY urlChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool engineRunning READ engineRunning NOTIFY engineRunningChanged)
    Q_PROPERTY(QString enginePath READ enginePath WRITE setEnginePath NOTIFY enginePathChanged)
    Q_PROPERTY(qreal devicePixelRatio READ devicePixelRatio WRITE setDevicePixelRatio NOTIFY devicePixelRatioChanged)
    Q_PROPERTY(int framesPerSecond READ framesPerSecond NOTIFY framesPerSecondChanged)

public:
    explicit LadybirdView(QQuickItem* parent = nullptr);
    ~LadybirdView() override;

    QString url() const { return m_url; }
    QString title() const { return m_title; }
    bool loading() const { return m_loading; }
    bool engineRunning() const { return m_connection >= 0; }
    QString enginePath() const { return m_enginePath; }
    void setEnginePath(const QString&);
    qreal devicePixelRatio() const { return m_devicePixelRatio; }
    void setDevicePixelRatio(qreal);
    int framesPerSecond() const { return m_framesPerSecond; }

    Q_INVOKABLE void load(const QString& url);
    Q_INVOKABLE void goBack();
    Q_INVOKABLE void goForward();
    Q_INVOKABLE void reload();

signals:
    void urlChanged();
    void titleChanged();
    void loadingChanged();
    void engineRunningChanged();
    void enginePathChanged();
    void devicePixelRatioChanged();
    void framesPerSecondChanged();

protected:
    void componentComplete() override;
    void geometryChanged(const QRectF& newGeometry, const QRectF& oldGeometry) override;
    QSGNode* updatePaintNode(QSGNode*, UpdatePaintNodeData*) override;
    void releaseResources() override;
    void touchEvent(QTouchEvent*) override;
    void timerEvent(QTimerEvent*) override;

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
    struct Frame {
        const uchar* pixels = nullptr;
        int pitch = 0;
        QSize bufferSize;
        QSize paintedSize;
        quint64 serial = 0;
    };

    void startEngine();
    void send(const lb_embed_header&, const QByteArray& text = QByteArray());
    void sendSize();
    void handleFrame(const lb_embed_header&, int fd);
    const uchar* mappingFor(int fd, size_t length);

    QString m_url;
    QString m_title;
    QString m_enginePath;
    QString m_pendingUrl;
    bool m_loading = false;
    qreal m_devicePixelRatio = 2.5;

    QProcess m_engine;
    QString m_socketPath;
    int m_listener = -1;
    int m_connection = -1;
    QSocketNotifier* m_listenerNotifier = nullptr;
    QSocketNotifier* m_connectionNotifier = nullptr;

    QVector<Mapping> m_mappings;
    Frame m_frame;
    quint64 m_frameSerial = 0;

    // Render thread state. The GUI thread is blocked while updatePaintNode runs, so it may read m_frame.
    QSGTexture* m_texture = nullptr;
    uint m_textureId = 0;
    QSize m_textureSize;
    quint64 m_uploadedSerial = 0;

    int m_framesThisSecond = 0;
    int m_framesPerSecond = 0;
    int m_frameRateTimer = 0;
};
