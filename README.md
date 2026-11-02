# Plainroot FileChooser

`plainroot-filechooser` is the Plainroot XDG FileChooser backend and browser
project. It will provide a native portal backend while Plainroot applications
continue to call the toolkit's XDG portal client. It must not call that client
from its backend process, and Plainroot's build and runtime dependency closure
must stay free of GIO.

## Current slice

The `plainroot-filechooser-browser` executable is an integration prototype for
the local browser UI. It uses `plainroot-toolkit` for its window and widgets and
`plainroot-core` asynchronous filesystem listing for directory navigation.
Search is applied to the displayed listing, hidden entries can be shown, and
file selection returns the native path on stdout. It consumes
`plainroot-storage-client` snapshots to list removable volumes and mounted
network locations. Local volume mount and confirmed unmount requests go through
`plainroot-storaged`. SFTP locations can be connected through the asynchronous
`plainroot-storage-network-client`, which talks to the unprivileged user-session
`plainroot-networkd` service. The connection form accepts a server address,
optional username and remote folder, and a port defaulting to 22. The chooser
offers disconnect only for network mounts owned by
`plainroot-networkd`. Partial local snapshots suppress local volume operations.
Local file browsing remains available when either storage service is
unavailable. Closing the browser never unmounts a volume or network location.
The window follows the active Plainroot appearance theme through the toolkit.

The build also includes an unregistered `plainroot-filechooser-portal`
prototype. It exports the portal FileChooser and Request interfaces through
`sd-bus`, supports single-file, multiple-file, and directory `OpenFile`
requests, and supports `SaveFile` requests with suggested names and overwrite
confirmation. Multiple selection uses checkboxes and returns every selected
file as a `file://` URI. Successful requests, request closure, and user
cancellation are handled. It is not advertised to the portal broker yet.
Filters, choices, parent-window modality, and live multiple-file portal
acceptance remain open. Keep GTK selected as the fallback until those
capabilities and request lifecycle are validated.

Live removable-volume mount and SSHFS connect/disconnect acceptance are still
pending; the current host does not have the `sshfs` executable installed.

The browser accepts an optional absolute starting directory. Directory names
remain native byte paths for navigation and selection; names that are not valid
UTF-8 are rendered in a safe display form. Directory I/O runs outside the UI
thread, and results are applied on the toolkit application thread.

## Build

Build through the `plainroot-desktop` workspace script so dependencies resolve
from the workspace's uninstalled Meson packages:

```sh
./scripts/workspace.sh build
```

After workspace setup, rebuild just this project with
`meson compile -C build/plainroot-filechooser`.

The browser requires the Wayland toolkit runtime. For a localized run, use the
workspace's `run-localized.sh` helper with the generated `po` directory.

## Design constraints

- Use `plainroot-core` for filesystem access, `plainroot-storage-client` for
  removable-device discovery and local mount lifecycle, and
  `plainroot-storage-network-client` for managed SFTP locations.
- Keep privileged mount policy in `plainroot-storaged`.
- Implement the portal backend with `sd-bus`; do not route it through the
  toolkit's portal client.
- Keep the portal request protocol private to this project.
- Preserve keyboard and pointer operation, RTL layout, and responsive behavior
  at smartphone, tablet, and laptop reference sizes.

See the [FileChooser roadmap](../plainroot-docs/plainroot-filechooser-roadmap.md)
for the staged delivery and parity requirements.
