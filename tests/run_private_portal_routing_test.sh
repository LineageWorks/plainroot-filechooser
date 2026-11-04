#!/usr/bin/env bash
set -euo pipefail

if (($# != 1)); then
  printf 'Usage: %s PLAINROOT_FILECHOOSER_PORTAL_BINARY\n' "$0" >&2
  exit 2
fi

backend_binary=$(realpath "$1")
project_root=$(realpath "$(dirname "$0")/..")
portal_broker=/usr/libexec/xdg-desktop-portal
gtk_descriptor=/usr/share/xdg-desktop-portal/portals/gtk.portal

for command_name in dbus-run-session gdbus realpath timeout; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    printf 'SKIP: required command is unavailable: %s\n' "$command_name" >&2
    exit 77
  fi
done
if [[ ! -x "$portal_broker" || ! -f "$gtk_descriptor" ]]; then
  printf 'SKIP: xdg-desktop-portal and its GTK backend are required\n' >&2
  exit 77
fi
if [[ ! -x "$backend_binary" ]]; then
  printf 'Not an executable portal backend: %s\n' "$backend_binary" >&2
  exit 2
fi

test_root=$(mktemp -d "${TMPDIR:-/tmp}/plainroot-portal-routing.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT

run_route_case() {
  local case_name=$1 expected_portal=$2 with_plainroot=$3
  local case_root="$test_root/$case_name"
  local data_home="$case_root/data"
  local config_home="$case_root/config"
  local runtime_dir="$case_root/runtime"
  local log_file="$case_root/session.log"

  mkdir -p "$data_home/dbus-1/services" \
    "$data_home/xdg-desktop-portal/portals" \
    "$config_home/xdg-desktop-portal" "$runtime_dir" "$case_root/home"
  chmod 700 "$runtime_dir" "$case_root/home"
  cp "$gtk_descriptor" "$data_home/xdg-desktop-portal/portals/gtk.portal"

  if [[ "$with_plainroot" == true ]]; then
    cp "$project_root/data/plainroot.portal" \
      "$data_home/xdg-desktop-portal/portals/plainroot.portal"
    cat > \
      "$data_home/dbus-1/services/org.freedesktop.impl.portal.desktop.plainroot.service" \
      <<EOF
[D-BUS Service]
Name=org.freedesktop.impl.portal.desktop.plainroot
Exec=$backend_binary
EOF
  fi

  cat > \
    "$data_home/dbus-1/services/org.freedesktop.portal.Desktop.service" \
    <<EOF
[D-BUS Service]
Name=org.freedesktop.portal.Desktop
Exec=$portal_broker --verbose
EOF

  if ! grep -Fqx 'org.freedesktop.impl.portal.FileChooser=plainroot;gtk' \
      "$project_root/data/plainrootde-portals.conf"; then
    printf 'Expected Plainroot then GTK preference is missing from source config\n' >&2
    return 1
  fi
  cat > "$config_home/xdg-desktop-portal/plainrootde-portals.conf" <<EOF
[preferred]
default=gtk
$(grep -F 'org.freedesktop.impl.portal.FileChooser=' \
  "$project_root/data/plainrootde-portals.conf")
EOF

  if ! XDG_CONFIG_HOME="$config_home" \
      XDG_DATA_HOME="$data_home" \
      XDG_DATA_DIRS="$data_home:/usr/share" \
      XDG_RUNTIME_DIR="$runtime_dir" \
      XDG_CURRENT_DESKTOP=PlainrootDE \
      WAYLAND_DISPLAY=plainroot-no-display \
      DISPLAY= \
      HOME="$case_root/home" \
      timeout 15s dbus-run-session -- bash -c '
        set -euo pipefail
        gdbus introspect --session \
          --dest org.freedesktop.portal.Desktop \
          --object-path /org/freedesktop/portal/desktop \
          >/dev/null 2>&1 || true
        sleep 0.5
      ' >"$log_file" 2>&1; then
    printf 'FAIL: private portal routing case %s failed\n' "$case_name" >&2
    tail -n 50 "$log_file" >&2
    return 1
  fi

  if ! grep -Fq \
      "XDP: Using $expected_portal.portal for org.freedesktop.impl.portal.FileChooser (config)" \
      "$log_file"; then
    printf 'FAIL: broker did not select %s for FileChooser\n' \
      "$expected_portal" >&2
    tail -n 50 "$log_file" >&2
    return 1
  fi

  printf 'PASS: private portal broker selected %s for FileChooser\n' \
    "$expected_portal"
}

run_route_case plainroot plainroot true
run_route_case gtk-fallback gtk false
