// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#pragma once

#include "qwaylandeglinclude_p.h" //must be first

#include <QtWaylandClient/private/qwaylanddisplay_p.h>
#include <QtGui/private/qeglplatformcontext_p.h>
#include <qpa/qplatformopenglcontext.h>

QT_BEGIN_NAMESPACE

class QOpenGLShaderProgram;
class QOpenGLTextureCache;

namespace QtWaylandClient {

class QWaylandEglWindow;
class DecorationsBlitter;

class Q_WAYLANDCLIENT_EXPORT QWaylandGLContext : public QEGLPlatformContext
{
public:
    QWaylandGLContext();
    QWaylandGLContext(EGLDisplay eglDisplay, QWaylandDisplay *display, const QSurfaceFormat &format, QPlatformOpenGLContext *share);
    ~QWaylandGLContext();

    void initialize() override;
    void swapBuffers(QPlatformSurface *surface) override;

    bool makeCurrent(QPlatformSurface *surface) override;
    void doneCurrent() override;

    void beginFrame() override;
    void endFrame() override;

    GLuint defaultFramebufferObject(QPlatformSurface *surface) const override;

protected:
    EGLSurface eglSurfaceForPlatformSurface(QPlatformSurface *surface) override;
    EGLSurface createTemporaryOffscreenSurface() override;
    void destroyTemporaryOffscreenSurface(EGLSurface surface) override;
    void runGLChecks() override;

private:
    QWaylandDisplay *m_display = nullptr;
    EGLContext m_decorationsContext = EGL_NO_CONTEXT;
    DecorationsBlitter *m_blitter = nullptr;
    bool m_supportNonBlockingSwap = true;
    EGLenum m_api;
    wl_surface *m_wlSurface = nullptr;
    wl_egl_window *m_eglWindow = nullptr;
    QWaylandEglWindow *m_currentWindow = nullptr;
    QMetaObject::Connection m_reconnectionWatcher;
    bool m_doneCurrentWorkAround = false;

    // linux-drm-syncobj-v1 (explicit sync) support; resolved lazily on the
    // first swap. All are null/empty when EGL_ANDROID_native_fence_sync is
    // not available, in which case the frames are committed with implicit
    // synchronization exactly like stock Qt.
    bool m_checkedNativeFenceSync = false;
    bool m_hasNativeFenceSync = false;
    PFNEGLCREATESYNCKHRPROC m_eglCreateSyncKHR = nullptr;
    PFNEGLDESTROYSYNCKHRPROC m_eglDestroySyncKHR = nullptr;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC m_eglDupNativeFenceFDANDROID = nullptr;

    // Some EGL implementations (NVIDIA's libEGL + egl-wayland) implement
    // linux-drm-syncobj-v1 internally inside eglSwapBuffers(). Binding a
    // second syncobj surface from Qt would raise the surface_exists
    // protocol error, so the Qt-side path is skipped for those vendors.
    bool m_checkedEglSyncobjSupport = false;
    bool m_eglHandlesDrmSyncobj = false;
};

}

QT_END_NAMESPACE
