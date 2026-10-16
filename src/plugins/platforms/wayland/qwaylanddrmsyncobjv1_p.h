// Copyright (C) 2026 The Kestrel contributors
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QWAYLANDDRMSYNCOBJV1_P_H
#define QWAYLANDDRMSYNCOBJV1_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include <QtWaylandClient/private/qwayland-linux-drm-syncobj-v1.h>
#include <QtWaylandClient/qtwaylandclientglobal.h>
#include <QtCore/private/qglobal_p.h>

#include <QtCore/qmutex.h>

#include <deque>
#include <memory>

struct wl_surface;

QT_BEGIN_NAMESPACE

namespace QtWaylandClient {

class QWaylandWindow;

class QWaylandDrmSyncobjTimelineV1 : public QtWayland::wp_linux_drm_syncobj_timeline_v1
{
public:
    explicit QWaylandDrmSyncobjTimelineV1(struct ::wp_linux_drm_syncobj_timeline_v1 *object)
        : QtWayland::wp_linux_drm_syncobj_timeline_v1(object)
    {
    }
    ~QWaylandDrmSyncobjTimelineV1() override
    {
        destroy();
    }

private:
    Q_DISABLE_COPY_MOVE(QWaylandDrmSyncobjTimelineV1)
};

class QWaylandDrmSyncobjSurfaceV1 : public QtWayland::wp_linux_drm_syncobj_surface_v1
{
public:
    explicit QWaylandDrmSyncobjSurfaceV1(struct ::wp_linux_drm_syncobj_surface_v1 *object)
        : QtWayland::wp_linux_drm_syncobj_surface_v1(object)
    {
    }
    ~QWaylandDrmSyncobjSurfaceV1() override
    {
        destroy();
    }

private:
    Q_DISABLE_COPY_MOVE(QWaylandDrmSyncobjSurfaceV1)
};

/*
    Per-window explicit synchronization state for the linux-drm-syncobj-v1
    protocol.

    One instance of this tracker is created lazily for every EGL window
    (QWaylandWindow with windowType() == QWaylandWindow::Egl) when the first
    frame is swapped. It owns:

      * the wp_linux_drm_syncobj_surface_v1 object wrapping the wl_surface,
      * one client-side DRM syncobj timeline used for the acquire points of
        all frames,
      * one DRM syncobj per committed frame, exported to the compositor as a
        release timeline. Per the protocol, each buffer must use a separate
        release timeline because timeline point signaling is cumulative and
        compositors may signal release points out of order.

    All public functions are called from the render thread, with the EGL
    context current, immediately around eglSwapBuffers(). The mutex protects
    against concurrent surface teardown on the GUI thread.
*/
class Q_WAYLANDCLIENT_EXPORT QWaylandDrmSyncobjFrameTracker
{
public:
    explicit QWaylandDrmSyncobjFrameTracker(QWaylandWindow *window);
    ~QWaylandDrmSyncobjFrameTracker();

    Q_DISABLE_COPY_MOVE(QWaylandDrmSyncobjFrameTracker)

    bool isValid() const { return mValid; }

    // Drains signaled release points and, if too many frames are still
    // outstanding, blocks (with a bounded timeout) until the oldest release
    // point has been signaled, so that a buffer is never handed back to EGL
    // before the compositor released it.
    //
    // Returns false if explicit sync cannot be used for the upcoming frame;
    // in that case no protocol objects are touched and the frame is
    // committed with implicit synchronization, exactly like stock Qt.
    bool prepareFrame();

    // Imports the native fence fd (EGL_ANDROID_native_fence_sync) covering
    // all rendering submitted so far in the current EGL context as the
    // acquire point of this frame, creates a fresh release timeline and sends
    // set_acquire_point and set_release_point. Takes ownership of fenceFd.
    //
    // Must only be called after prepareFrame() returned true. Returns true if
    // both points were sent; in that case the next commit on the surface must
    // attach a buffer. On failure no points are sent and the internal state
    // falls back to implicit sync.
    bool commitFrame(int fenceFd);

    // Called when eglSwapBuffers() failed after commitFrame() returned true.
    // Destroys and re-creates the surface synchronization object so that the
    // pending acquire/release points (which would otherwise be applied to the
    // next commit, potentially one without a buffer) are discarded.
    void discardPendingFrame();

private:
    bool ensureSurfaceLocked();
    bool ensureAcquireTimelineLocked();
    void drainSignaledReleasesLocked();
    void destroySurfaceLocked();
    void disableLocked(const char *reason);

    QWaylandWindow *mWindow = nullptr;
    bool mValid = false;

    QMutex mMutex;

    struct ::wl_surface *mWlSurface = nullptr;
    std::unique_ptr<QWaylandDrmSyncobjSurfaceV1> mSyncobjSurface;

    // Acquire timeline: one DRM syncobj shared by all frames of this window.
    uint32_t mAcquireTimelineHandle = 0; // DRM syncobj handle, 0 when not created
    std::unique_ptr<QWaylandDrmSyncobjTimelineV1> mAcquireTimelineObject;
    uint64_t mNextAcquirePoint = 1;

    struct ReleasePoint
    {
        uint32_t handle = 0; // DRM syncobj handle
        std::unique_ptr<QWaylandDrmSyncobjTimelineV1> object; // compositor-side object
    };
    // Release points of committed frames that the compositor has not
    // signaled yet, oldest first. The point value is always 1 (one timeline
    // per frame), so only the handles are stored.
    std::deque<ReleasePoint> mOutstandingReleases;
};

} // namespace QtWaylandClient

QT_END_NAMESPACE

#endif // QWAYLANDDRMSYNCOBJV1_P_H
