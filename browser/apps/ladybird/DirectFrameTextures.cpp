/*
 * Pacific: page frames as textures without copying them.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "DirectFrameTextures.h"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QQuickWindow>
#include <QSGTexture>
#include <QVector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <string.h>
#include <unistd.h>

#ifndef EGL_NATIVE_BUFFER_HYBRIS
#    define EGL_NATIVE_BUFFER_HYBRIS 0x3140
#endif

// Android's names for what we ask gralloc for.
static const EGLint pixelFormatBgra8888 = 5;
// NB: The software read and write bits are what make gralloc lay the buffer out as plain rows. Without them the
//     Mali gralloc picks AFBC, a compressed layout, and the engine's uncompressed pixels would be read as that.
static const EGLint usageSoftwareReadWriteOften = 0x33;
static const EGLint usageGpuTexture = 0x100;

// The engine cycles through two or three buffers per view, and replaces them all when the view is resized.
static const int maximumTextures = 4;

namespace {

struct Hybris {
    typedef EGLBoolean (*CreateNativeBuffer)(EGLint width, EGLint height, EGLint usage, EGLint format, EGLint* stride, EGLClientBuffer*);
    typedef EGLBoolean (*CreateRemoteBuffer)(EGLint width, EGLint height, EGLint usage, EGLint format, EGLint stride, int intCount, int* ints, int fdCount, int* fds, EGLClientBuffer*);
    typedef void (*GetNativeBufferInfo)(EGLClientBuffer, int* intCount, int* fdCount);
    typedef void (*SerializeNativeBuffer)(EGLClientBuffer, int* ints, int* fds);
    typedef EGLBoolean (*ReleaseNativeBuffer)(EGLClientBuffer);
    typedef EGLImageKHR (*CreateImage)(EGLDisplay, EGLContext, EGLenum target, EGLClientBuffer, const EGLint* attributes);
    typedef EGLBoolean (*DestroyImage)(EGLDisplay, EGLImageKHR);
    typedef void (*ImageTargetTexture)(GLenum target, void* image);

    CreateNativeBuffer createNativeBuffer = nullptr;
    CreateRemoteBuffer createRemoteBuffer = nullptr;
    GetNativeBufferInfo getNativeBufferInfo = nullptr;
    SerializeNativeBuffer serializeNativeBuffer = nullptr;
    ReleaseNativeBuffer releaseNativeBuffer = nullptr;
    CreateImage createImage = nullptr;
    DestroyImage destroyImage = nullptr;
    ImageTargetTexture imageTargetTexture = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    bool usable = false;

    Hybris()
    {
        if (!qgetenv("PACIFIC_UPLOAD_FRAMES").isEmpty())
            return;
        display = eglGetCurrentDisplay();
        const char* extensions = display != EGL_NO_DISPLAY ? eglQueryString(display, EGL_EXTENSIONS) : nullptr;
        if (!extensions || !strstr(extensions, "EGL_HYBRIS_native_buffer2"))
            return;
        createNativeBuffer = reinterpret_cast<CreateNativeBuffer>(eglGetProcAddress("eglHybrisCreateNativeBuffer"));
        createRemoteBuffer = reinterpret_cast<CreateRemoteBuffer>(eglGetProcAddress("eglHybrisCreateRemoteBuffer"));
        getNativeBufferInfo = reinterpret_cast<GetNativeBufferInfo>(eglGetProcAddress("eglHybrisGetNativeBufferInfo"));
        serializeNativeBuffer = reinterpret_cast<SerializeNativeBuffer>(eglGetProcAddress("eglHybrisSerializeNativeBuffer"));
        releaseNativeBuffer = reinterpret_cast<ReleaseNativeBuffer>(eglGetProcAddress("eglHybrisReleaseNativeBuffer"));
        createImage = reinterpret_cast<CreateImage>(eglGetProcAddress("eglCreateImageKHR"));
        destroyImage = reinterpret_cast<DestroyImage>(eglGetProcAddress("eglDestroyImageKHR"));
        imageTargetTexture = reinterpret_cast<ImageTargetTexture>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        usable = createNativeBuffer && createRemoteBuffer && getNativeBufferInfo && serializeNativeBuffer
            && releaseNativeBuffer && createImage && destroyImage && imageTargetTexture;
    }
};

Hybris& hybris()
{
    static Hybris instance;
    return instance;
}

bool isDmaBuf(int fd)
{
    char path[64];
    char target[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    ssize_t length = ::readlink(path, target, sizeof(target) - 1);
    if (length < 0)
        return false;
    target[length] = 0;
    return !strncmp(target, "/dmabuf:", 8) || !strncmp(target, "anon_inode:dmabuf", 17);
}

off_t sizeOf(int fd)
{
    return ::lseek(fd, 0, SEEK_END);
}

}

DirectFrameTextures::~DirectFrameTextures()
{
    for (Texture& texture : m_textures)
        release(texture);
}

void DirectFrameTextures::release(Texture& texture)
{
    delete texture.texture;
    if (texture.textureId) {
        if (QOpenGLContext* context = QOpenGLContext::currentContext())
            context->functions()->glDeleteTextures(1, &texture.textureId);
    }
    if (texture.image)
        hybris().destroyImage(hybris().display, texture.image);
    if (texture.buffer)
        hybris().releaseNativeBuffer(texture.buffer);
    texture = Texture();
    ++m_generation;
}

void DirectFrameTextures::forget(quint64 bufferId)
{
    auto found = m_textures.find(bufferId);
    if (found == m_textures.end())
        return;
    release(found.value());
    m_textures.erase(found);
}

bool DirectFrameTextures::import(QQuickWindow* window, const LadybirdFrame& frame, Texture& texture)
{
    Hybris& egl = hybris();
    const int width = frame.bufferSize.width();
    const int height = frame.bufferSize.height();
    const EGLint usage = usageGpuTexture | usageSoftwareReadWriteOften;

    // A gralloc buffer of our size tells us how this device lays such a buffer out, and its handle is the one
    // we copy. The buffer itself is given back as soon as we have the copy.
    EGLint stride = 0;
    EGLClientBuffer pattern = nullptr;
    if (!egl.createNativeBuffer(width, height, usage, pixelFormatBgra8888, &stride, &pattern) || !pattern)
        return false;

    int intCount = 0;
    int fdCount = 0;
    egl.getNativeBufferInfo(pattern, &intCount, &fdCount);
    QVector<int> ints(intCount);
    QVector<int> fds(fdCount);
    egl.serializeNativeBuffer(pattern, ints.data(), fds.data());

    // The pixels are the handle's largest dma-buf (Qualcomm's gralloc keeps its metadata in a small one of its own),
    // laid out the way the engine laid its own out, and no larger than the engine's, or the driver would reach past
    // the end of ours.
    int pixelFd = -1;
    for (int i = 0; i < fdCount; ++i) {
        if (isDmaBuf(fds[i]) && (pixelFd < 0 || sizeOf(fds[i]) > sizeOf(fds[pixelFd])))
            pixelFd = i;
    }
    if (pixelFd < 0 || stride * 4 != frame.pitch || sizeOf(fds[pixelFd]) > sizeOf(frame.bufferFd)) {
        egl.releaseNativeBuffer(pattern);
        return false;
    }

    // NB: Creating the buffer closes the file descriptors it is given, so it gets duplicates. The pattern's
    //     other descriptors (gralloc's bookkeeping) are shared with it and outlive the pattern that way.
    QVector<int> ours(fdCount);
    for (int i = 0; i < fdCount; ++i)
        ours[i] = ::dup(i == pixelFd ? frame.bufferFd : fds[i]);
    EGLClientBuffer buffer = nullptr;
    bool created = egl.createRemoteBuffer(width, height, usage, pixelFormatBgra8888, stride, intCount, ints.data(), fdCount, ours.data(), &buffer) && buffer;
    egl.releaseNativeBuffer(pattern);
    if (!created)
        return false;
    texture.buffer = buffer;

    texture.image = egl.createImage(egl.display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_HYBRIS, buffer, nullptr);
    if (texture.image == EGL_NO_IMAGE_KHR) {
        texture.image = nullptr;
        return false;
    }

    QOpenGLFunctions* gl = QOpenGLContext::currentContext()->functions();
    while (gl->glGetError() != GL_NO_ERROR) { }
    gl->glGenTextures(1, &texture.textureId);
    gl->glBindTexture(GL_TEXTURE_2D, texture.textureId);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    egl.imageTargetTexture(GL_TEXTURE_2D, texture.image);
    bool bound = gl->glGetError() == GL_NO_ERROR;
    window->resetOpenGLState();
    if (!bound)
        return false;

    // We delete the GL texture ourselves, together with the image behind it.
    texture.texture = window->createTextureFromId(texture.textureId, QSize(stride, height));
    return true;
}

QSGTexture* DirectFrameTextures::textureFor(QQuickWindow* window, const LadybirdFrame& frame)
{
    if (!hybris().usable || frame.bufferFd < 0 || !frame.bufferId)
        return nullptr;

    auto found = m_textures.find(frame.bufferId);
    if (found != m_textures.end()) {
        found->lastUsed = ++m_useCounter;
        return found->texture;
    }
    if (m_refused.contains(frame.bufferId))
        return nullptr;

    while (m_textures.size() >= maximumTextures) {
        auto oldest = m_textures.begin();
        for (auto it = m_textures.begin(); it != m_textures.end(); ++it) {
            if (it->lastUsed < oldest->lastUsed)
                oldest = it;
        }
        release(oldest.value());
        m_textures.erase(oldest);
    }

    Texture texture;
    if (!import(window, frame, texture)) {
        release(texture);
        if (m_refused.size() > 64)
            m_refused.clear();
        m_refused.insert(frame.bufferId);
        qWarning("DirectFrameTextures: cannot use a %dx%d buffer (pitch %d) as a texture; uploading its pixels instead",
            frame.bufferSize.width(), frame.bufferSize.height(), frame.pitch);
        return nullptr;
    }
    texture.lastUsed = ++m_useCounter;
    m_textures.insert(frame.bufferId, texture);
    ++m_generation;
    return texture.texture;
}
