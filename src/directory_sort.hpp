#pragma once

#include "directory_loader.hpp"

#include <vector>

namespace plainroot::filechooser::detail {

enum class DirectorySortOrder {
  name_ascending = 0,
  name_descending = 1,
  size_ascending = 2,
  size_descending = 3,
  type_ascending = 4,
  type_descending = 5,
  modified_newest_first = 6,
  modified_oldest_first = 7,
  recent_first = 8,
};

std::vector<const DirectoryEntry *>
sort_directory_entries(const std::vector<const DirectoryEntry *> &entries,
                       DirectorySortOrder order);

} // namespace plainroot::filechooser::detail
