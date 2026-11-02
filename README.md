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
file selection returns the native path on stdout. It is not yet registered as
an XDG portal backend; request handling, mounted-volume actions, filters, save
flows, and backend registration remain on the roadmap.
The window follows the active Plainroot appearance theme through the toolkit.

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

- Use `plainroot-core` for filesystem access and `plainroot-storage-client` for
  device and mount discovery or lifecycle.
- Keep privileged mount policy in `plainroot-storaged`.
- Implement the portal backend with `sd-bus`; do not route it through the
  toolkit's portal client.
- Keep the portal request protocol private to this project.
- Preserve keyboard and pointer operation, RTL layout, and responsive behavior
  at smartphone, tablet, and laptop reference sizes.

See the [FileChooser roadmap](../plainroot-docs/plainroot-filechooser-roadmap.md)
for the staged delivery and parity requirements.
