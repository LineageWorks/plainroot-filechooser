# Plainroot FileChooser

`plainroot-filechooser` provides the Plainroot XDG FileChooser backend and
browser. Plainroot applications continue to call the toolkit's XDG portal
client; the backend must not call that client from its own process. Portal
registration is enabled by default for PlainrootDE packages. The build installs
the FileChooser portal descriptor, D-Bus and systemd user activation, and the
`plainrootde-portals.conf` that prefers Plainroot for FileChooser while keeping
Hyprland and GTK as defaults for other interfaces. The PlainrootDE session must
set `XDG_CURRENT_DESKTOP=PlainrootDE:Hyprland` for that desktop-specific
configuration to take precedence; a plain Hyprland session continues to use
GTK. Set `PLAINROOT_FILECHOOSER_PORTAL_REGISTRATION=false` during a workspace
build or `-Dportal_registration=false` with Meson to omit the registration
assets. Building or staging does not rewrite the host's portal preferences.
Installing into the system prefix only adds the PlainrootDE-specific routing
configuration. GTK is a capability and behavior reference, not a visual
template; Plainroot follows its own desktop design. Plainroot's build and
runtime dependency closure must stay free of GIO. Both enabled and disabled
`DESTDIR` payloads are covered by the install checks.

Run `just check-filechooser-portal-routing` to verify Plainroot-first selection
and GTK fallback in an isolated D-Bus session with temporary XDG directories.
This checks broker routing without changing host portal configuration; it does
not open an interactive chooser or replace live desktop acceptance.

Run `just check-filechooser-installed-routing` to repeat those routing checks
using the installed payload. It ensures `portal_registration` is enabled,
installs into a temporary `DESTDIR`, and starts private broker sessions using
`XDG_CURRENT_DESKTOP=PlainrootDE:Hyprland`
to verify Plainroot selection and GTK fallback, then checks that a plain
`Hyprland` session still selects GTK with the Plainroot backend installed.
Finally, it restores the build option and removes the staged files. It does not
change host portal preferences or launch an interactive chooser.

Run `just check-filechooser-install` to verify enabled and explicitly disabled
registration payloads in separate temporary `DESTDIR` installs. It restores the
build option and never changes host portal files.

For an interactive Editor check without changing the host portal setup, run
`PLAINROOT_STAGE=0 just staged-filechooser-session`, then launch
`plainroot-editor` from the shell it opens. This private D-Bus session starts
the staged Plainroot backend directly, routes FileChooser requests to it for
`PlainrootDE:Hyprland`, keeps GTK as the fallback, and enables
`PLAINROOT_FILECHOOSER_TRACE=1` for lifecycle diagnostics. The temporary
activation files and portal preference are removed when the session exits.
Alternatively, `PLAINROOT_STAGE=0 just staged-filechooser-editor` launches the
staged Editor directly inside this private session, avoiding the generic
`staged-session` portal configuration.

On 2026-11-05, the installed-payload routing check passed: the private broker
selected Plainroot for `PlainrootDE:Hyprland`, selected GTK for plain Hyprland,
and used GTK when the staged Plainroot descriptor was omitted. The check
restored `portal_registration=false` and left the active desktop unchanged.

## Current slice

The `plainroot-filechooser-browser` executable is an integration prototype for
the local browser UI. It uses `plainroot-toolkit` for its window and widgets and
`plainroot-core` asynchronous filesystem listing for directory navigation.
The location controls include Home and a Places dropdown populated from
configured XDG user folders that currently exist. Saved local bookmarks use
GTK's interoperable `$XDG_CONFIG_HOME/gtk-3.0/bookmarks` format (defaulting to
`~/.config/gtk-3.0/bookmarks`); the current folder can be added to or removed
from that list, and existing non-local GTK bookmark URIs are preserved when the
file is updated. The current folder path uses
clickable ancestor breadcrumbs. The Location action or `Ctrl+L` opens an
adaptive path field. It accepts absolute and current-folder-relative paths, as
well as `~` and `~/...`; path normalization is lexical and existence checks run
asynchronously. Directories open in the browser, files are selected for open
requests, and save requests use a typed file path to set its parent folder and
filename. Single-select file names align to the logical start edge of their
rows, keeping the listing easy to scan in both LTR and RTL locales. Search is
applied to the displayed listing, hidden entries can be shown, and the listing
supports locale-aware, natural filename ordering in either
direction while keeping directories first. The **Show file details** switch
shows size, file type, and modified time together; metadata is read
asynchronously on the directory worker only when details are shown or a
metadata sort is selected. Choosing a metadata sort also enables the details
view, and folders show their type instead of a misleading allocated size.
Selecting an image displays an aspect-preserving thumbnail in an adaptive
preview pane. JPEG EXIF
orientation is applied by `plainroot-image`; image decoding and scaling run on
the directory worker. Single-file open and save modes
provide an editable filename field; open requests validate a typed name in the
current folder and navigate when that name identifies a folder. Search, file
name, folder name, and location inputs keep stable localized accessible names
separate from their changing values. File selection returns the native path on
stdout. It consumes
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
If a normal unmount fails because the location is in use, the chooser preserves
the typed busy result and shows localized guidance to close files using it.
Networkd checks FUSE device and mount capability before accepting a connection;
when the session cannot mount FUSE locations, the chooser gives a localized
setup hint and leaves the endpoint form available for retry. Plainroot does not
change `fusermount3` permissions.
The window follows the active Plainroot appearance theme through the toolkit.
The New folder action reveals an inline name field and creates exactly one
directory asynchronously through `plainroot-core`, so slow mounted filesystems
do not block the chooser. It rejects path components, reports collisions and
filesystem errors, then refreshes the current listing.
The SFTP form actions and managed-share actions reflow into compact stacked
rows when the available width is constrained. The storage-view regression
checks both form buttons and activates Connect at phone portrait (390×844),
phone landscape (844×390), tablet portrait (768×1024), tablet landscape
(1024×768), and laptop (1366×768) sizes. It also checks mounted-share actions
and disconnect confirmation within those widths.
The form remains visible while Networkd processes an accepted connection and
closes only after a successful mount event. If the request cannot start or the
mount fails asynchronously, the endpoint fields remain available for retry.

The build also includes the `plainroot-filechooser-portal` backend. It exports
the portal FileChooser and Request interfaces through
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
returned as selected ID/value pairs. `OpenFile` requests also offer an
“Open files read-only” checkbox. Its state is returned as the portal `writable`
boolean, defaulting to true when the checkbox remains unchecked. Request
parsing bounds group and option counts and string sizes, and rejects duplicate
IDs or invalid initial selections. MIME
filters use the shared MIME database's `globs2`, aliases, subclass data, and
compiled XDG `magic` rules for ambiguous or unmatched filenames. MIME
classification runs on the directory worker and is requested only when the
selected filter uses MIME patterns. Glob and MIME patterns in one filter are
combined as alternatives. Content sniffing reads the same XDG MIME database as
filename matching, including rule priorities, masks, ranges, nested matches,
and `__NOMAGIC__` overrides. The backend and browser remain free of GIO and
libmagic. Portal parent identifiers and the `modal` option are forwarded to
toolkit window creation. On Wayland, `xdg-foreign` v2 transient parenting and
the optional testing `xdg-dialog-v1` presentation hint are used when available.
The modal hint does not enforce parent-event filtering, and parent destruction
or handle invalidation now routes through the toolkit's normal window-close
callback and cancels the portal request. X11 parent identifiers are ignored.
With portal registration enabled, the staged backend is discoverable when the
portal broker starts with the staged data directory and `PlainrootDE` desktop
configuration. Private-broker checks have completed single-file and multiple
file `OpenFile`, folder selection, `SaveFile` with both a suggested name and an
existing `current_file`, overwrite confirmation, `SaveFiles`, glob and MIME
filters, and enumerated choices. A live user-bus check also activated the
staged backend through its user-systemd unit and stopped it cleanly. A staged
interactive Editor check with an existing document confirmed that selecting a
replacement file loads its contents and closes the Plainroot chooser normally.
Installed desktop registration and remaining GTK capability acceptance remain
open; keep registration opt-in until those checks pass. Visual acceptance is
evaluated against Plainroot's own desktop design, not GTK appearance.

Storaged's local-volume operation passed live systemd read/write mount and
normal-unmount acceptance on `/dev/sdb1` on 2026-11-03, including a user
write/read/delete check. On 2026-11-03, the user also confirmed that mounting,
browsing, and unmounting the removable volume through this chooser UI
succeeded. SSHFS is installed, and
a strict-host-key SFTP handshake to the active localhost `sshd` succeeds. A
direct host-session SSHFS mount failed at FUSE setup with `Operation not
permitted`; `/dev/fuse` is present, but `fusermount3` is not setuid. The
production `plainroot-networkd` service mounted `localhost:/etc`, read
`/etc/os-release`, and disconnected cleanly in an isolated user and mount
namespace using a fake user-systemd manager. On 2026-11-04, a live host
user-systemd Networkd mount of `localhost:/etc` was browsed through the
FileChooser; `/etc/os-release` remained readable after closing the browser,
and the mount stayed active until Networkd explicitly disconnected it. The
temporary FUSE helper permission change was restored to mode `0755`. This
confirms browser access to an existing managed SSHFS mount. Later on
2026-11-04, the rebuilt chooser connected to `localhost:/etc`, displayed
`os-release`, and disconnected the managed share with the helper temporarily
set to root-owned mode `04755`; the helper was restored to `0755`. Persistent
FUSE setup remains open. The USB was also physically removed during a Storaged
mounted-filesystem read; the read returned an I/O error and complete catalog
snapshots dropped the device and mount. After reinsertion, the probe checksum
matched, the test file was removed, and the volume was normally unmounted. The
USB is currently present and unmounted. A follow-up
chooser-initiated SFTP attempt on 2026-11-04 reached
the transient SSHFS unit's `Started` state and then a normal Networkd unmount
about three seconds later. No mount or recent FileChooser core dump remained.
During a later diagnostic, two monitored browser processes remained mapped and
no FileChooser coredump was found. Direct Networkd connection attempts ended
with `MOUNT_FAILED` while the helper was mode `0755`, and the mount snapshot
remained empty. This did not reproduce a process crash. The user later
clarified that a quick close may have been caused by the test agent; no visible
error was observed, so the report is inconclusive and is not treated as a
confirmed application crash. A fresh workspace browser subsequently displayed
the FUSE setup hint and stayed open after the user submitted `localhost:/etc`;
`GetMounts` remained empty and no transient unit or mountpoint was created.
The chooser keeps the SFTP form open until the mount succeeds, making failed
attempts retryable. The rebuilt Networkd's positive FUSE path was also verified
directly on 2026-11-04 with the helper temporarily set to root-owned `4755`:
it mounted `localhost:/etc`, allowed reading `/etc/os-release`, and
disconnected normally. The helper was restored to `0755` after both the direct
service and chooser UI checks.

Both the browser and portal backend support opt-in diagnostics with
`PLAINROOT_FILECHOOSER_TRACE=1` in the process environment. They record portal
request and response lifecycle, SFTP form visibility and submission, typed
network request IDs and states, the Toolkit window-close request result, and
the standalone browser event-loop result. They do not log the host, username,
remote path, or selected filenames. When the backend is activated by D-Bus or
systemd, set the variable in that backend's environment rather than only in the
requesting application's environment.

Storaged's physical-removal acceptance completed on 2026-11-04. The USB was
removed during a bounded read from a mounted VFAT probe file, then reinserted;
the current catalog enumerates `/dev/sdb1` and Storaged reports it unmounted.

The browser accepts an optional absolute starting directory. Directory names
remain native byte paths for navigation and selection; names that are not valid
UTF-8 are rendered in a safe display form. Directory I/O runs outside the UI
thread, and results are applied on the toolkit application thread. MIME data is
read from user and system XDG MIME directories; their compiled `magic` rules
classify content when filename rules do not resolve to one unambiguous type.

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

To verify both the opt-in portal registration payload and the safe default
install without modifying the host, run:

```sh
tests/run_portal_install_test.sh ../build/plainroot-filechooser
```

The script installs into temporary `DESTDIR` roots and restores the build's
previous `portal_registration` option when it finishes.

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
for staged delivery and capability acceptance requirements.
