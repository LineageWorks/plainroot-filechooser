#pragma once

namespace plainroot::filechooser::detail {

constexpr bool portal_selection_mode_valid(bool save_file, bool multiple,
                                           bool directory) noexcept {
  return !save_file || (!multiple && !directory);
}

constexpr bool entry_is_multi_selectable(bool allow_multiple,
                                         bool select_directories,
                                         bool entry_is_directory) noexcept {
  return allow_multiple && select_directories == entry_is_directory;
}

} // namespace plainroot::filechooser::detail
