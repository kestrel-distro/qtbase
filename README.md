# Kestrel fork of qtbase

This branch (`kestrel/6.12.0-syncobj`) is Kestrel's fork of Qt Base. It adds
client-side `linux-drm-syncobj-v1` support — Wayland explicit synchronization —
for Qt 6.12.

Kestrel's Quickshell presents through Qt, so without explicit sync a frame can
be shown before its buffer is ready. The fork lets the Qt Wayland client attach
and wait on DRM syncobj timelines, keeping presentation synchronized with the
compositor.

## How Kestrel uses it

`kestrel-distro/desktop` packages this tree as `qt6-base-kestrel`
(`packaging/qt6-base`). That package keeps Arch's Qt version, uses a distinct
`-kestrel` name that `conflicts`/`replaces`/`provides` `qt6-base`, and is served
from the signed `[kestrel]` repository. The desktop library closure and the
Quickshell build link against it, so the whole desktop uses one Qt.

The ISO builder consumes the same recipe through
`kestrel/install/kestrel-source.packages`.

## Working on it

This is a Qt source tree; use Qt's own build and test instructions for the
branch. Kestrel-specific changes should stay limited to the explicit-sync
client path and be exercised in the VM with Hyprland + Quickshell before they
are published.

## Related

- [`desktop`](https://github.com/kestrel-distro/desktop) — package recipe and
  metapackage
- [`quickshell`](https://github.com/kestrel-distro/quickshell) — the client that
  presents through this Qt
- [`packages`](https://github.com/kestrel-distro/packages) — signed `[kestrel]`
  repository

Qt Base itself is licensed under the LGPL/GPL/commercial terms shipped with the
tree; see the upstream license files.
