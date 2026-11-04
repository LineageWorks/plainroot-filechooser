#pragma once

#include "directory_loader.hpp"

#include <string>
#include <vector>

namespace plainroot::filechooser::detail {

struct DirectoryMetadataDisplay {
  std::string label_message;
  std::string value;
  bool value_is_ltr = false;
};

std::vector<DirectoryMetadataDisplay> format_directory_metadata_displays(
    const DirectoryEntry &entry, const std::string &folder_label,
    const std::string &unknown_type_label);

} // namespace plainroot::filechooser::detail
