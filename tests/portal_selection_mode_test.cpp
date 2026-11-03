#include "../src/portal_selection_mode.hpp"

#include <cassert>

using plainroot::filechooser::detail::entry_is_multi_selectable;
using plainroot::filechooser::detail::portal_selection_mode_valid;

int main() {
  assert(portal_selection_mode_valid(false, false, false));
  assert(portal_selection_mode_valid(false, true, false));
  assert(portal_selection_mode_valid(false, false, true));
  assert(portal_selection_mode_valid(false, true, true));
  assert(portal_selection_mode_valid(true, false, false));
  assert(!portal_selection_mode_valid(true, true, false));
  assert(!portal_selection_mode_valid(true, false, true));

  assert(!entry_is_multi_selectable(false, false, false));
  assert(entry_is_multi_selectable(true, false, false));
  assert(!entry_is_multi_selectable(true, false, true));
  assert(!entry_is_multi_selectable(false, true, false));
  assert(!entry_is_multi_selectable(true, true, false));
  assert(entry_is_multi_selectable(true, true, true));
  return 0;
}
