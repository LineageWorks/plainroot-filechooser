#pragma once

#include <plainroot/core/filesystem.h>

namespace plainroot::filechooser::detail {

enum class PortalDefaultAcceptLabel {
  select,
  open,
  save,
};

constexpr PortalDefaultAcceptLabel
portal_default_accept_label(bool saving, bool multiple) noexcept {
  if (saving)
    return PortalDefaultAcceptLabel::save;
  return multiple ? PortalDefaultAcceptLabel::open
                  : PortalDefaultAcceptLabel::select;
}

constexpr bool portal_selection_mode_valid(bool save_file, bool multiple,
                                           bool directory) noexcept {
  return !save_file || (!multiple && !directory);
}

constexpr bool entry_is_multi_selectable(bool allow_multiple,
                                         bool select_directories,
                                         plainroot_core_fs_entry_kind_v1 kind)
    noexcept {
  if (!allow_multiple)
    return false;
  if (kind == PLAINROOT_CORE_FS_ENTRY_SYMLINK)
    return true;
  return select_directories
             ? kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY
             : kind == PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE;
}

constexpr bool symlink_target_matches_selection(
    bool select_directories,
    plainroot_core_fs_entry_kind_v1 target_kind) noexcept {
  return select_directories
             ? target_kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY
             : target_kind == PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE;
}

} // namespace plainroot::filechooser::detail
