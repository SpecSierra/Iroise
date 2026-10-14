/*
 * Pacific: page frames as textures without copying them.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QHash>
#include <QObject>
#include <QSet>

#include "LadybirdEngine.h"

class QQuickWindow;
class QSGTexture;

// Turns the engine's frame buffers into textures that ARE those buffers, so that showing a frame costs no upload.
//
// The engine paints into dma-bufs. The phone's EGL (an Android driver behind libhybris) cannot import a dma-buf
// directly, it only takes Android's own buffer handles. So we have gralloc allocate a buffer of the same layout,
// copy its handle with our dma-buf in place of gralloc's pixel memory, and import that copy.
//
// Lives on the render thread: create it, use it and delete it there.
class DirectFrameTextures : public QObject {
    Q_OBJECT

public:
    DirectFrameTextures() { }
    ~DirectFrameTextures() override;

    // The texture for this frame's buffer, sized pitch / 4 by the buffer's height, or null when the buffer cannot be
    // used this way and its pixels have to be uploaded instead.
    QSGTexture* textureFor(QQuickWindow*, const LadybirdFrame&);
    void forget(quint64 bufferId);

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

    QHash<quint64, Texture> m_textures;
    QSet<quint64> m_refused;
    quint64 m_generation = 0;
    quint64 m_useCounter = 0;
};
