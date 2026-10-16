// Copyright (C) 2026 The Kestrel contributors
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only
// Qt-Security score:significant reason:default

#include "qwaylanddrmsyncobjv1_p.h"

#include "qwaylanddisplay_p.h"
#include "qwaylandwindow_p.h"

#include <QtCore/qbytearray.h>
#include <QtCore/qdebug.h>

#if QT_WAYLANDCLIENT_HAVE_LIBDRM
#include <drm.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <vector>
#endif

QT_BEGIN_NAMESPACE

namespace QtWaylandClient {

#if QT_WAYLANDCLIENT_HAVE_LIBDRM

namespace {

// Mesa's wayland EGL platform keeps four color buffers per surface
// (dri2_egl_surface::color_buffers[4]), so a buffer from commit N may be
// reused as early as commit N+4. Bounding the number of outstanding release
// points to three guarantees that the release point of a buffer has been
// signaled before EGL can recycle that buffer, no matter which of the free
// buffers EGL picks.
constexpr qsizetype kMaxOutstandingReleases = 3;

// Upper bound for blocking on a release point before giving up. A compositor
// that never signals release points must not be able to stall the client
// forever; after the timeout the point is dropped (a potential glitch, not a
// hang).
constexpr qint64 kReleaseWaitTimeoutNs = 1000 * 1000 * 1000; // 1s

QMutex g_drmDeviceMutex;
int g_drmDeviceFd = -2; // -2: not probed yet, -1: no suitable device

bool deviceSupportsSyncobj(int fd)
{
    struct drm_get_cap cap = {};
    cap.capability = DRM_CAP_SYNCOBJ;
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) != 0 || cap.value == 0)
        return false;

    memset(&cap, 0, sizeof(cap));
    cap.capability = DRM_CAP_SYNCOBJ_TIMELINE;
    if (ioctl(fd, DRM_IOCTL_GET_CAP, &cap) != 0 || cap.value == 0)
        return false;

    // Probe that we can actually create a syncobj (fails e.g. for primary
    // nodes without DRM master).
    struct drm_syncobj_create create = {};
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) != 0)
        return false;

    struct drm_syncobj_destroy destroy = {};
    destroy.handle = create.handle;
    ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    return true;
}

// Opens a DRM device file that supports DRM syncobjs. Render nodes are
// preferred because they do not require authentication or DRM master. The fd
// is shared by all windows for the lifetime of the process.
int drmDeviceFd()
{
    QMutexLocker locker(&g_drmDeviceMutex);
    if (g_drmDeviceFd != -2)
        return g_drmDeviceFd;

    g_drmDeviceFd = -1;

    for (int i = 128; i < 192 && g_drmDeviceFd == -1; ++i) {
        const QByteArray path = "/dev/dri/renderD" + QByteArray::number(i);
        const int fd = open(path.constData(), O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (deviceSupportsSyncobj(fd))
            g_drmDeviceFd = fd;
        else
            close(fd);
    }

    for (int i = 0; i < 64 && g_drmDeviceFd == -1; ++i) {
        const QByteArray path = "/dev/dri/card" + QByteArray::number(i);
        const int fd = open(path.constData(), O_RDWR | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (deviceSupportsSyncobj(fd))
            g_drmDeviceFd = fd;
        else
            close(fd);
    }

    if (g_drmDeviceFd < 0)
        qCWarning(lcQpaWayland) << "linux-drm-syncobj-v1: no DRM device with syncobj support found,"
                                << "falling back to implicit sync";
    return g_drmDeviceFd;
}

bool syncobjCreate(int fd, uint32_t *handle)
{
    struct drm_syncobj_create create = {};
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &create) != 0)
        return false;
    *handle = create.handle;
    return true;
}

void syncobjDestroy(int fd, uint32_t handle)
{
    struct drm_syncobj_destroy destroy = {};
    destroy.handle = handle;
    ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
}

// Exports a syncobj as an fd that another DRM fd (in our case: the
// compositor's) can import with DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE.
bool syncobjExport(int fd, uint32_t handle, int *objectFd)
{
    struct drm_syncobj_handle arg = {};
    arg.handle = handle;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &arg) != 0)
        return false;
    *objectFd = arg.fd;
    return true;
}

// Imports a sync_file (e.g. from EGL_ANDROID_native_fence_sync) as a fence
// signaling the given point of a timeline syncobj.
bool syncobjImportSyncFile(int fd, uint32_t handle, uint64_t point, int syncFileFd)
{
    struct drm_syncobj_handle arg = {};
    arg.handle = handle;
    arg.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE
            | DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE;
    arg.fd = syncFileFd;
    arg.point = point;
    return ioctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &arg) == 0;
}

bool syncobjWait(int fd, const uint32_t *handles, const uint64_t *points, uint32_t count,
                 int64_t timeoutNs, uint32_t *firstSignaled)
{
    struct drm_syncobj_timeline_wait arg = {};
    arg.handles = reinterpret_cast<uint64_t>(handles);
    arg.points = reinterpret_cast<uint64_t>(points);
    arg.timeout_nsec = timeoutNs;
    arg.count_handles = count;
    // Without DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL the wait completes when any
    // point is signaled. WAIT_FOR_SUBMIT keeps the ioctl from erroring out
    // when the compositor has not submitted a fence for the point yet.
    arg.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
    if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &arg) != 0)
        return false;
    if (firstSignaled)
        *firstSignaled = arg.first_signaled;
    return true;
}

int64_t monotonicClockNs()
{
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000 * 1000 * 1000 + ts.tv_nsec;
}

} // namespace

#endif // QT_WAYLANDCLIENT_HAVE_LIBDRM

QWaylandDrmSyncobjFrameTracker::QWaylandDrmSyncobjFrameTracker(QWaylandWindow *window)
    : mWindow(window)
{
#if QT_WAYLANDCLIENT_HAVE_LIBDRM
    if (qEnvironmentVariableIsSet("QT_WAYLAND_DISABLE_DRMSYNCOBJ"))
        return;
    mValid = drmDeviceFd() >= 0;
#endif
}

QWaylandDrmSyncobjFrameTracker::~QWaylandDrmSyncobjFrameTracker()
{
#if QT_WAYLANDCLIENT_HAVE_LIBDRM
    const int drmFd = drmDeviceFd();

    QMutexLocker locker(&mMutex);

    destroySurfaceLocked();

    if (mAcquireTimelineHandle) {
        if (drmFd >= 0)
            syncobjDestroy(drmFd, mAcquireTimelineHandle);
        mAcquireTimelineHandle = 0;
    }
    mAcquireTimelineObject.reset();

    if (drmFd >= 0) {
        for (const ReleasePoint &point : mOutstandingReleases)
            syncobjDestroy(drmFd, point.handle);
    }
    mOutstandingReleases.clear();
#endif
}

bool QWaylandDrmSyncobjFrameTracker::prepareFrame()
{
#if QT_WAYLANDCLIENT_HAVE_LIBDRM
    QMutexLocker locker(&mMutex);

    if (!mValid || !ensureSurfaceLocked())
        return false;

    drainSignaledReleasesLocked();

    const int drmFd = drmDeviceFd();
    while (qsizetype(mOutstandingReleases.size()) > kMaxOutstandingReleases) {
        // Too many unreleased frames: block until the oldest release point is
        // signaled so that EGL cannot recycle a buffer the compositor is
        // still reading.
        const ReleasePoint &oldest = mOutstandingReleases.front();
        const uint32_t handle = oldest.handle;
        const uint64_t point = 1;
        const int64_t deadline = monotonicClockNs() + kReleaseWaitTimeoutNs;

        if (syncobjWait(drmFd, &handle, &point, 1, deadline, nullptr)) {
            mOutstandingReleases.pop_front();
        } else {
            qCWarning(lcQpaWayland) << "linux-drm-syncobj-v1: timed out waiting for a release"
                                    << "point; dropping it (the compositor may now read a buffer"
                                    << "that is being re-rendered)";
            mOutstandingReleases.pop_front();
        }
    }

    return true;
#else
    return false;
#endif
}

bool QWaylandDrmSyncobjFrameTracker::commitFrame(int fenceFd)
{
#if QT_WAYLANDCLIENT_HAVE_LIBDRM
    const int drmFd = drmDeviceFd();
    uint32_t releaseHandle = 0;
    std::unique_ptr<QWaylandDrmSyncobjTimelineV1> releaseObject;

    {
        QMutexLocker locker(&mMutex);

        auto cleanupRelease = [&] {
            if (releaseHandle) {
                syncobjDestroy(drmFd, releaseHandle);
                releaseHandle = 0;
            }
            releaseObject.reset();
        };

        if (!mValid || !ensureSurfaceLocked()) {
            close(fenceFd);
            return false;
        }

        // Import the rendering fence as the acquire point of this frame.
        const uint64_t acquirePoint = mNextAcquirePoint++;
        if (!syncobjImportSyncFile(drmFd, mAcquireTimelineHandle, acquirePoint, fenceFd)) {
            qCWarning(lcQpaWayland) << "linux-drm-syncobj-v1: failed to import the acquire fence;"
                                    << "falling back to implicit sync for this surface";
            close(fenceFd);
            disableLocked("failed to import acquire fence");
            return false;
        }
        close(fenceFd);

        // Create a fresh timeline for the release point of this frame. Per
        // the protocol, each buffer gets its own release timeline because
        // point signaling is cumulative and release points may be signaled
        // out of order.
        int releaseFd = -1;
        const bool created = syncobjCreate(drmFd, &releaseHandle);
        const bool exported = created && syncobjExport(drmFd, releaseHandle, &releaseFd);
        if (!created || !exported) {
            qCWarning(lcQpaWayland) << "linux-drm-syncobj-v1: failed to create the release timeline;"
                                    << "falling back to implicit sync for this surface";
            cleanupRelease();
            disableLocked("failed to create release timeline");
            return false;
        }

        releaseObject.reset(new QWaylandDrmSyncobjTimelineV1(
                mWindow->display()->drmSyncobjManager()->import_timeline(releaseFd)));
        close(releaseFd);

        if (!releaseObject->object()) {
            qCWarning(lcQpaWayland) << "linux-drm-syncobj-v1: failed to import the release timeline;"
                                    << "falling back to implicit sync for this surface";
            cleanupRelease();
            disableLocked("failed to import release timeline");
            return false;
        }

        mOutstandingReleases.push_back(ReleasePoint{ releaseHandle, std::move(releaseObject) });
        releaseHandle = 0;

        QWaylandDrmSyncobjTimelineV1 *releaseTimeline =
                mOutstandingReleases.back().object.get();

        // Everything is in place; from here on the commit must carry both
        // points or the compositor will raise a protocol error.
        mSyncobjSurface->set_acquire_point(mAcquireTimelineObject->object(),
                                           uint32_t(acquirePoint >> 32),
                                           uint32_t(acquirePoint & 0xffffffff));
        mSyncobjSurface->set_release_point(releaseTimeline->object(), 0, 1);
        return true;
    }
#else
    Q_UNUSED(fenceFd);
    return false;
#endif
}

void QWaylandDrmSyncobjFrameTracker::discardPendingFrame()
{
#if QT_WAYLANDCLIENT_HAVE_LIBDRM
    QMutexLocker locker(&mMutex);
    // Destroying the surface synchronization object discards any acquire and
    // release points set since the last commit.
    destroySurfaceLocked();
#endif
}

#if QT_WAYLANDCLIENT_HAVE_LIBDRM

void QWaylandDrmSyncobjFrameTracker::destroySurfaceLocked()
{
    mSyncobjSurface.reset();
    mWlSurface = nullptr;
}

void QWaylandDrmSyncobjFrameTracker::disableLocked(const char *reason)
{
    qCWarning(lcQpaWayland) << "linux-drm-syncobj-v1: disabling explicit sync for a surface:" << reason;
    // Destroy the wrapper so that upcoming buffer commits without acquire and
    // release points remain valid (implicit sync) again.
    destroySurfaceLocked();
}

bool QWaylandDrmSyncobjFrameTracker::ensureSurfaceLocked()
{
    QWaylandDisplay *display = mWindow->display();
    QtWayland::wp_linux_drm_syncobj_manager_v1 *manager = display->drmSyncobjManager();
    if (!manager)
        return false; // compositor does not advertise linux-drm-syncobj-v1

    struct ::wl_surface *surface = mWindow->wlSurface();
    if (!surface)
        return false;

    if (mSyncobjSurface && mWlSurface == surface) {
        if (ensureAcquireTimelineLocked())
            return true;
        destroySurfaceLocked();
        return false;
    }

    // The wl_surface was (re)created: re-bind.
    destroySurfaceLocked();

    std::unique_ptr<QWaylandDrmSyncobjSurfaceV1> wrapper(
            new QWaylandDrmSyncobjSurfaceV1(manager->get_surface(surface)));
    if (!wrapper->object()) {
        qCWarning(lcQpaWayland) << "linux-drm-syncobj-v1: get_surface failed;"
                                << "falling back to implicit sync for this surface";
        return false;
    }

    mSyncobjSurface = std::move(wrapper);
    mWlSurface = surface;

    if (!ensureAcquireTimelineLocked()) {
        // Without an acquire timeline every buffer commit would miss the
        // acquire point; unbind again so the frame uses implicit sync.
        destroySurfaceLocked();
        return false;
    }
    return true;
}

bool QWaylandDrmSyncobjFrameTracker::ensureAcquireTimelineLocked()
{
    if (mAcquireTimelineHandle && mAcquireTimelineObject)
        return true;

    const int drmFd = drmDeviceFd();
    uint32_t handle = 0;
    int timelineFd = -1;

    if (!syncobjCreate(drmFd, &handle) || !syncobjExport(drmFd, handle, &timelineFd))
        return false;

    mAcquireTimelineObject.reset(new QWaylandDrmSyncobjTimelineV1(
            mWindow->display()->drmSyncobjManager()->import_timeline(timelineFd)));
    close(timelineFd);

    if (!mAcquireTimelineObject->object()) {
        syncobjDestroy(drmFd, handle);
        mAcquireTimelineObject.reset();
        return false;
    }

    mAcquireTimelineHandle = handle;
    return true;
}

void QWaylandDrmSyncobjFrameTracker::drainSignaledReleasesLocked()
{
    if (mOutstandingReleases.empty())
        return;

    const int drmFd = drmDeviceFd();

    std::vector<uint32_t> handles;
    std::vector<uint64_t> points;
    handles.reserve(mOutstandingReleases.size());
    points.reserve(mOutstandingReleases.size());
    for (const ReleasePoint &point : mOutstandingReleases) {
        handles.push_back(point.handle);
        points.push_back(1);
    }

    while (!handles.empty()) {
        uint32_t firstSignaled = 0;
        if (!syncobjWait(drmFd, handles.data(), points.data(), handles.size(), 0, &firstSignaled))
            break; // nothing signaled (yet)
        if (firstSignaled >= handles.size())
            break;
        handles.erase(handles.begin() + firstSignaled);
        points.erase(points.begin() + firstSignaled);
        mOutstandingReleases.erase(mOutstandingReleases.begin() + firstSignaled);
    }
}

#endif // QT_WAYLANDCLIENT_HAVE_LIBDRM

} // namespace QtWaylandClient

QT_END_NAMESPACE
