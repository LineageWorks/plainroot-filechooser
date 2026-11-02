#include "directory_breadcrumbs.hpp"

namespace plainroot::filechooser::detail {

std::vector<DirectoryBreadcrumb>
directory_breadcrumbs(std::string_view absolute_path) {
  if (absolute_path.empty() || absolute_path.front() != '/')
    return {};

  std::vector<std::string> components;
  size_t start = 1U;
  while (start < absolute_path.size()) {
    while (start < absolute_path.size() && absolute_path[start] == '/')
      ++start;
    if (start >= absolute_path.size())
      break;
    const size_t end = absolute_path.find('/', start);
    const size_t component_end =
        end == std::string_view::npos ? absolute_path.size() : end;
    components.emplace_back(absolute_path.substr(start, component_end - start));
    start = component_end;
  }

  if (components.empty())
    return {{"/", "/", true}};

  std::vector<DirectoryBreadcrumb> breadcrumbs;
  breadcrumbs.reserve(components.size() + 1U);
  breadcrumbs.push_back({"/", "/", false});
  std::string path;
  for (size_t index = 0; index < components.size(); ++index) {
    path.push_back('/');
    path.append(components[index]);
    breadcrumbs.push_back(
        {components[index], path, index + 1U == components.size()});
  }
  return breadcrumbs;
}

} // namespace plainroot::filechooser::detail
