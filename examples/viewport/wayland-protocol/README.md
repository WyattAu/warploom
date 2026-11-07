# Vendored Wayland protocol: xdg-shell (stable)

Source: `stable/xdg-shell/xdg-shell.xml` from wayland-protocols 1.50
(gitlab.freedesktop.org/wayland/wayland-protocols), MIT-licensed —
see the `<copyright>` block at the top of the XML.

Why vendored, not found at build time: the `wayland-protocols` data
package is not installed on this machine (and cannot be assumed on CI);
the file is a stable, versioned spec (~64 KB), and the build generates
the client bindings from it with the already-required `wayland-scanner`.
Upgrading means replacing this one file.

Generated at build time (viewport target only):
`xdg-shell-client-protocol.h` + `xdg-shell-protocol.cpp`.
