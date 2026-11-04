#!/usr/bin/env bash
set -euo pipefail

if (($# != 1)); then
  printf 'Usage: %s MESON_BUILD_DIR\n' "$0" >&2
  exit 2
fi

build_dir=$(realpath "$1")
portal_broker=/usr/libexec/xdg-desktop-portal
gtk_descriptor=/usr/share/xdg-desktop-portal/portals/gtk.portal

for command_name in dbus-run-session gdbus meson python3 realpath timeout; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    printf 'SKIP: required command is unavailable: %s\n' "$command_name" >&2
    exit 77
  fi
done
if [[ ! -x "$portal_broker" || ! -f "$gtk_descriptor" ]]; then
  printf 'SKIP: xdg-desktop-portal and its GTK backend are required\n' >&2
  exit 77
fi
if [[ ! -f "$build_dir/meson-private/coredata.dat" ||
      ! -x "$build_dir/plainroot-filechooser-portal" ]]; then
  printf 'Not a built FileChooser Meson directory: %s\n' "$build_dir" >&2
  exit 2
fi

build_option() {
  meson introspect --buildoptions "$build_dir" |
    python3 -c 'import json,sys; name=sys.argv[1]; values=json.load(sys.stdin); value=next(x["value"] for x in values if x["name"] == name); print(str(value).lower() if isinstance(value,bool) else value)' "$1"
}

option_before=$(build_option portal_registration)
stage_root=$(mktemp -d "${TMPDIR:-/tmp}/plainroot-installed-portal.XXXXXX")
restore_build_option() {
  meson configure "$build_dir" "-Dportal_registration=$option_before" \
    >/dev/null || true
  rm -rf -- "$stage_root"
}
trap restore_build_option EXIT

install_path() {
  python3 -c 'import pathlib,sys; prefix,subdir=sys.argv[1:]; path=pathlib.PurePosixPath(subdir); print(path if path.is_absolute() else pathlib.PurePosixPath(prefix)/path)' \
    "$(build_option prefix)" "$(build_option "$1")"
}

meson configure "$build_dir" -Dportal_registration=true >/dev/null
DESTDIR="$stage_root" meson install -C "$build_dir" >/dev/null

data_path=$(install_path datadir)
libexec_path=$(install_path libexecdir)
staged_share="$stage_root$data_path"
portal_descriptor="$staged_share/xdg-desktop-portal/portals/plainroot.portal"
dbus_service="$staged_share/dbus-1/services/org.freedesktop.impl.portal.desktop.plainroot.service"
staged_config="$staged_share/xdg-desktop-portal/plainrootde-portals.conf"
user_service="$stage_root$(install_path prefix)/lib/systemd/user/plainroot-filechooser-portal.service"
staged_backend="$stage_root$libexec_path/plainroot-filechooser-portal"
build_backend="$build_dir/plainroot-filechooser-portal"

for installed_file in "$portal_descriptor" "$dbus_service" \
                      "$staged_config" "$user_service" "$staged_backend"; do
  if [[ ! -f "$installed_file" ]]; then
    printf 'Missing opt-in installed file: %s\n' "$installed_file" >&2
    exit 1
  fi
done

python3 - "$dbus_service" "$build_backend" <<'PY'
from pathlib import Path
import sys

service = Path(sys.argv[1])
backend = sys.argv[2]
lines = service.read_text().splitlines()
output = []
exec_count = 0
for line in lines:
    if line.startswith("Exec="):
        output.append(f"Exec={backend}")
        exec_count += 1
    elif not line.startswith("SystemdService="):
        output.append(line)
if exec_count != 1:
    raise SystemExit("installed D-Bus descriptor must have exactly one Exec line")
service.write_text("\n".join(output) + "\n")
PY

if ! grep -Fqx 'DBusName=org.freedesktop.impl.portal.desktop.plainroot' \
    "$portal_descriptor" ||
   ! grep -Fqx 'Interfaces=org.freedesktop.impl.portal.FileChooser;' \
    "$portal_descriptor" ||
   ! grep -Fqx 'org.freedesktop.impl.portal.FileChooser=plainroot;gtk' \
    "$staged_config"; then
  printf 'Installed portal descriptor or preference is incomplete\n' >&2
  exit 1
fi

run_route_case() {
  local case_name=$1 expected_portal=$2 include_staged_registration=$3
  local case_root="$stage_root/cases/$case_name"
  local data_home="$case_root/data"
  local config_home="$case_root/config"
  local config_dirs="$case_root/config-dirs"
  local runtime_dir="$case_root/runtime"
  local log_file="$case_root/session.log"
  local data_dirs

  mkdir -p "$data_home/dbus-1/services" "$config_home" "$config_dirs" \
    "$runtime_dir" "$case_root/home"
  chmod 700 "$runtime_dir" "$case_root/home"
  cat >"$data_home/dbus-1/services/org.freedesktop.portal.Desktop.service" <<EOF
[D-BUS Service]
Name=org.freedesktop.portal.Desktop
Exec=$portal_broker --verbose
EOF

  if [[ "$include_staged_registration" == true ]]; then
    data_dirs="$staged_share:/usr/share"
  else
    local fallback_share="$case_root/fallback-share"
    mkdir -p "$fallback_share/xdg-desktop-portal"
    cp "$staged_config" \
      "$fallback_share/xdg-desktop-portal/plainrootde-portals.conf"
    data_dirs="$fallback_share:/usr/share"
  fi

  if ! XDG_CONFIG_HOME="$config_home" \
      XDG_CONFIG_DIRS="$config_dirs" \
      XDG_DATA_HOME="$data_home" \
      XDG_DATA_DIRS="$data_dirs" \
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
    printf 'FAIL: staged portal routing case %s failed\n' "$case_name" >&2
    tail -n 50 "$log_file" >&2
    return 1
  fi

  if ! grep -Fq \
      "XDP: Using $expected_portal.portal for org.freedesktop.impl.portal.FileChooser (config)" \
      "$log_file"; then
    printf 'FAIL: staged broker did not select %s for FileChooser\n' \
      "$expected_portal" >&2
    tail -n 50 "$log_file" >&2
    return 1
  fi

  printf 'PASS: staged broker selected %s for FileChooser\n' \
    "$expected_portal"
}

run_route_case plainroot plainroot true
run_route_case gtk-fallback gtk false

printf 'Installed portal payload routes Plainroot first and retains GTK fallback in a private broker session.\n'
