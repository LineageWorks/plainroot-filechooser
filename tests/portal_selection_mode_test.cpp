#include "../src/portal_selection_mode.hpp"

#include <cassert>

using plainroot::filechooser::detail::entry_is_multi_selectable;
using plainroot::filechooser::detail::portal_default_accept_label;
using plainroot::filechooser::detail::portal_selection_mode_valid;
using plainroot::filechooser::detail::symlink_target_matches_selection;
using plainroot::filechooser::detail::PortalDefaultAcceptLabel;

int main() {
  assert(portal_default_accept_label(false, false) ==
         PortalDefaultAcceptLabel::select);
  assert(portal_default_accept_label(false, true) ==
         PortalDefaultAcceptLabel::open);
  assert(portal_default_accept_label(true, false) ==
         PortalDefaultAcceptLabel::save);
  assert(portal_default_accept_label(true, true) ==
         PortalDefaultAcceptLabel::save);

  assert(portal_selection_mode_valid(false, false, false));
  assert(portal_selection_mode_valid(false, true, false));
  assert(portal_selection_mode_valid(false, false, true));
  assert(portal_selection_mode_valid(false, true, true));
  assert(portal_selection_mode_valid(true, false, false));
  assert(!portal_selection_mode_valid(true, true, false));
  assert(!portal_selection_mode_valid(true, false, true));

  assert(!entry_is_multi_selectable(
      false, false, PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE));
  assert(entry_is_multi_selectable(
      true, false, PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE));
  assert(!entry_is_multi_selectable(
      true, false, PLAINROOT_CORE_FS_ENTRY_DIRECTORY));
  assert(!entry_is_multi_selectable(
      true, false, PLAINROOT_CORE_FS_ENTRY_OTHER));
  assert(!entry_is_multi_selectable(
      false, true, PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE));
  assert(!entry_is_multi_selectable(
      true, true, PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE));
  assert(entry_is_multi_selectable(
      true, true, PLAINROOT_CORE_FS_ENTRY_DIRECTORY));
  assert(!entry_is_multi_selectable(
      true, true, PLAINROOT_CORE_FS_ENTRY_OTHER));
  assert(entry_is_multi_selectable(true, false,
                                   PLAINROOT_CORE_FS_ENTRY_SYMLINK));
  assert(entry_is_multi_selectable(true, true,
                                   PLAINROOT_CORE_FS_ENTRY_SYMLINK));

  assert(symlink_target_matches_selection(
      false, PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE));
  assert(!symlink_target_matches_selection(
      false, PLAINROOT_CORE_FS_ENTRY_DIRECTORY));
  assert(symlink_target_matches_selection(
      true, PLAINROOT_CORE_FS_ENTRY_DIRECTORY));
  assert(!symlink_target_matches_selection(
      true, PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE));
  assert(!symlink_target_matches_selection(
      true, PLAINROOT_CORE_FS_ENTRY_OTHER));
  return 0;
}
