#!/usr/bin/env bash
set -euo pipefail

build_dir=${1:?usage: run_portal_install_test.sh MESON_BUILD_DIR}
build_dir=$(realpath "$build_dir")
if [[ ! -f "$build_dir/meson-private/coredata.dat" ]]; then
  printf 'Not a configured Meson build directory: %s\n' "$build_dir" >&2
  exit 2
fi

build_option() {
  meson introspect --buildoptions "$build_dir" |
    python3 -c 'import json,sys; name=sys.argv[1]; options=json.load(sys.stdin); value=next(o["value"] for o in options if o["name"] == name); print(str(value).lower() if isinstance(value,bool) else value)' "$1"
}

option_before=$(build_option portal_registration)
stage_root=$(mktemp -d "${TMPDIR:-/tmp}/plainroot-filechooser-install.XXXXXX")
restore_build_option() {
  meson configure "$build_dir" "-Dportal_registration=$option_before" \
    >/dev/null || true
  rm -rf -- "$stage_root"
}
trap restore_build_option EXIT

install_path() {
  python3 -c 'import pathlib,sys; prefix,subdir=sys.argv[1:]; p=pathlib.PurePosixPath(subdir); print(p if p.is_absolute() else pathlib.PurePosixPath(prefix)/p)' \
    "$(build_option prefix)" "$(build_option "$1")"
}

check_file() {
  local path="$1"
  if [[ ! -f "$stage_root$path" ]]; then
    printf 'Expected staged file is missing: %s\n' "$path" >&2
    exit 1
  fi
}

check_line() {
  local file="$1"
  local expected="$2"
  if ! grep -Fqx -- "$expected" "$stage_root$file"; then
    printf 'Expected line not found in %s: %s\n' "$file" "$expected" >&2
    exit 1
  fi
}

libexec_path=$(install_path libexecdir)
data_path=$(install_path datadir)

meson configure "$build_dir" -Dportal_registration=true >/dev/null
DESTDIR="$stage_root" meson install -C "$build_dir" >/dev/null

portal_descriptor="$data_path/xdg-desktop-portal/portals/plainroot.portal"
dbus_service="$data_path/dbus-1/services/org.freedesktop.impl.portal.desktop.plainroot.service"
user_service="$(build_option prefix)/lib/systemd/user/plainroot-filechooser-portal.service"
portals_config="$data_path/xdg-desktop-portal/plainrootde-portals.conf"
backend="$libexec_path/plainroot-filechooser-portal"
browser="$(install_path bindir)/plainroot-filechooser-browser"

for file in "$portal_descriptor" "$dbus_service" "$user_service" \
            "$portals_config" "$backend" "$browser"; do
  check_file "$file"
done
check_line "$portal_descriptor" 'DBusName=org.freedesktop.impl.portal.desktop.plainroot'
check_line "$portal_descriptor" 'Interfaces=org.freedesktop.impl.portal.FileChooser;'
check_line "$portal_descriptor" 'UseIn=plainrootde;'
check_line "$dbus_service" 'Name=org.freedesktop.impl.portal.desktop.plainroot'
check_line "$dbus_service" "Exec=$backend"
check_line "$dbus_service" 'SystemdService=plainroot-filechooser-portal.service'
check_line "$user_service" 'BusName=org.freedesktop.impl.portal.desktop.plainroot'
check_line "$user_service" "ExecStart=$backend"
check_line "$portals_config" 'default=hyprland;gtk;'
check_line "$portals_config" 'org.freedesktop.impl.portal.FileChooser=plainroot;gtk;'

printf 'Opt-in portal registration staging passed.\n'

rm -rf -- "$stage_root"
mkdir -m 700 -- "$stage_root"
meson configure "$build_dir" -Dportal_registration=false >/dev/null
DESTDIR="$stage_root" meson install -C "$build_dir" >/dev/null
for file in "$portal_descriptor" "$dbus_service" "$user_service" \
            "$portals_config"; do
  if [[ -e "$stage_root$file" ]]; then
    printf 'Default install unexpectedly contains portal registration: %s\n' \
      "$file" >&2
    exit 1
  fi
done
check_file "$backend"
check_file "$browser"
printf 'Default install omits portal registration and activation files.\n'
