#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace plainroot::filechooser::detail {

struct DirectoryBreadcrumb {
  std::string name;
  std::string path;
  bool current = false;
};

std::vector<DirectoryBreadcrumb>
directory_breadcrumbs(std::string_view absolute_path);

} // namespace plainroot::filechooser::detail
