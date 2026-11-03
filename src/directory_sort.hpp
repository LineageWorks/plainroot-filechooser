#pragma once

#include "directory_loader.hpp"

#include <vector>

namespace plainroot::filechooser::detail {

enum class DirectorySortOrder {
  name_ascending,
  name_descending,
  recent_first,
};

std::vector<const DirectoryEntry *>
sort_directory_entries(const std::vector<const DirectoryEntry *> &entries,
                       DirectorySortOrder order);

} // namespace plainroot::filechooser::detail
