#pragma once

#include "directory_loader.hpp"
#include "directory_sort.hpp"

#include <string>

namespace plainroot::filechooser::detail {

struct DirectoryMetadataDisplay {
  std::string label_message;
  std::string value;
};

DirectoryMetadataDisplay format_directory_metadata_display(
    const DirectoryEntry &entry, DirectorySortOrder order,
    const std::string &folder_label, const std::string &unknown_type_label);

} // namespace plainroot::filechooser::detail
