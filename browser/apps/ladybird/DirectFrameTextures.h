/*
 * Iroise: page frames as textures without copying them.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QSize>

#include "LadybirdEngine.h"

class QOpenGLContext;
class QQuickWindow;
class QSGTexture;

// Turns the engine's frame buffers into textures that ARE those buffers, so that showing a frame costs no upload.
//
// The engine paints into dma-bufs. The phone's EGL (an Android driver behind libhybris) cannot import a dma-buf
// directly, it only takes Android's own buffer handles. So we have gralloc allocate a buffer of the same layout,
// copy its handle with our dma-buf in place of gralloc's pixel memory, and import that copy.
//
// Where a buffer cannot be used that way, its pixels are uploaded into one texture of ours instead.
//
// Lives on the render thread: create it, use it and delete it there. Its GL objects belong to the scene graph's
// GL context of the time. Silica destroys that context while the app is in the background, and the next context
// hands out the same texture names for textures of its own, so ours must never be bound or deleted in it.
class DirectFrameTextures : public QObject {
    Q_OBJECT

public:
    DirectFrameTextures() { }
    ~DirectFrameTextures() override;

    // The texture for this frame's buffer, sized pitch / 4 by the buffer's height, or null when the buffer cannot be
    // used this way and its pixels have to be uploaded instead.
    QSGTexture* textureFor(QQuickWindow*, const LadybirdFrame&);
    void forget(quint64 bufferId);

    // The frame's pixels, uploaded into our texture, which is sized pitch / 4 by the buffer's height.
    QSGTexture* upload(QQuickWindow*, const LadybirdFrame&);
    // The last frame uploaded, if any, and its painted size.
    QSGTexture* uploaded() const { return m_uploaded; }
    QSize uploadedSize() const { return m_uploadedPaintedSize; }
    void dropUploaded();

    // Changes whenever a texture is created or deleted. A node must not outlive the generation it was made in, see
    // EngineView::updatePaintNode.
    quint64 generation() const { return m_generation; }

private:
    struct Texture {
        void* buffer = nullptr;
        void* image = nullptr;
        uint textureId = 0;
        QSGTexture* texture = nullptr;
        quint64 lastUsed = 0;
        quint64 serial = 0;
    };

    bool import(QQuickWindow*, const LadybirdFrame&, Texture&);
    void release(Texture&);
    // Whether GL names of ours may be used, which is only in the context they were made in. Forgets them all
    // (without deleting them) when that context has gone.
    void adoptCurrentContext();
    bool inOurContext() const;
    void deleteTexture(uint);

    QPointer<QOpenGLContext> m_context;
    bool m_hadContext = false;

    QSGTexture* m_uploaded = nullptr;
    uint m_uploadedId = 0;
    QSize m_uploadedTextureSize;
    QSize m_uploadedPaintedSize;
    quint64 m_uploadedSerial = 0;

    QHash<quint64, Texture> m_textures;
    QSet<quint64> m_refused;
    quint64 m_generation = 0;
    quint64 m_useCounter = 0;
};
