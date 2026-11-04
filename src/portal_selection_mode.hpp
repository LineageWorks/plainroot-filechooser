#pragma once

#include <plainroot/core/filesystem.h>

namespace plainroot::filechooser::detail {

constexpr bool portal_selection_mode_valid(bool save_file, bool multiple,
                                           bool directory) noexcept {
  return !save_file || (!multiple && !directory);
}

constexpr bool entry_is_multi_selectable(bool allow_multiple,
                                         bool select_directories,
                                         plainroot_core_fs_entry_kind_v1 kind)
    noexcept {
  return allow_multiple &&
         (kind == PLAINROOT_CORE_FS_ENTRY_SYMLINK ||
          select_directories ==
              (kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY));
}

constexpr bool symlink_target_matches_selection(
    bool select_directories,
    plainroot_core_fs_entry_kind_v1 target_kind) noexcept {
  return select_directories
             ? target_kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY
             : target_kind == PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE;
}

} // namespace plainroot::filechooser::detail
