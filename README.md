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
The current folder path uses clickable ancestor breadcrumbs. Search is applied
to the displayed listing, hidden entries can be shown, and
the listing supports locale-aware, natural filename ordering in either
direction while keeping directories first. Single-file open and save modes
provide an editable filename field; open requests validate a typed name in the
current folder and navigate when that name identifies a folder. File selection
returns the native path on stdout. It consumes
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
The New folder action reveals an inline name field and creates exactly one
directory asynchronously through `plainroot-core`, so slow mounted filesystems
do not block the chooser. It rejects path components, reports collisions and
filesystem errors, then refreshes the current listing.

The build also includes an unregistered `plainroot-filechooser-portal`
prototype. It exports the portal FileChooser and Request interfaces through
`sd-bus`, supports single-file, multiple-file, and directory `OpenFile`
requests, and supports `SaveFile` requests with suggested names and overwrite
confirmation. `SaveFiles` requests show the requested names, ask for a
destination folder, and return ordered `file://` URIs with available names
when requested names collide at selection time. The UI explains that existing
or repeated names receive numbered variants; the portal does not create or
reserve files. Multi-file `OpenFile` selection uses checkboxes and returns
every selected file as a `file://` URI. Successful requests, request closure,
and user cancellation are handled. Glob filters can select the visible file
set, and the selected `current_filter` is returned with successful responses.
Portal `choices` are rendered as adaptive dropdowns or boolean checkboxes and
returned as selected ID/value pairs. Request parsing bounds group and option counts and
string sizes, and rejects duplicate IDs or invalid initial selections. MIME
filters use the shared MIME database's `globs2`, aliases, and subclass data,
plus libmagic content sniffing for ambiguous or unmatched filenames. MIME
classification runs on the directory worker and is requested only when the
selected filter uses MIME patterns. Glob and MIME patterns in one filter are
combined as alternatives. The backend and browser link libmagic but remain
free of GIO. Portal parent identifiers and the `modal` option are forwarded to
toolkit window creation. On Wayland, `xdg-foreign` v2 transient parenting and
the optional testing `xdg-dialog-v1` presentation hint are used when available.
The modal hint does not enforce parent-event filtering, and parent destruction
or handle invalidation now routes through the toolkit's normal window-close
callback and cancels the portal request. X11 parent identifiers are ignored.
The prototype is not advertised to the portal broker; live compositor/broker
and multi-file, SaveFiles, filter, and choice acceptance remain open. Keep GTK
selected as the fallback until those capabilities and request lifecycle are
validated.

Live removable-volume mount and SSHFS connect/disconnect acceptance are still
pending; the current host does not have the `sshfs` executable installed.

The browser accepts an optional absolute starting directory. Directory names
remain native byte paths for navigation and selection; names that are not valid
UTF-8 are rendered in a safe display form. Directory I/O runs outside the UI
thread, and results are applied on the toolkit application thread. MIME data is
read from user and system XDG MIME directories; libmagic is used for content
sniffing when filename rules do not resolve to one unambiguous type.

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
