#include "../src/directory_breadcrumbs.hpp"

#include <cassert>

using plainroot::filechooser::detail::directory_breadcrumbs;

int main() {
  const auto root = directory_breadcrumbs("/");
  assert(root.size() == 1U);
  assert(root[0].name == "/");
  assert(root[0].path == "/");
  assert(root[0].current);

  const auto nested = directory_breadcrumbs("/home/alex/Documents");
  assert(nested.size() == 4U);
  assert(nested[0].name == "/");
  assert(nested[0].path == "/");
  assert(!nested[0].current);
  assert(nested[1].name == "home");
  assert(nested[1].path == "/home");
  assert(!nested[1].current);
  assert(nested[2].name == "alex");
  assert(nested[2].path == "/home/alex");
  assert(!nested[2].current);
  assert(nested[3].name == "Documents");
  assert(nested[3].path == "/home/alex/Documents");
  assert(nested[3].current);

  const auto normalized = directory_breadcrumbs("//var///tmp/");
  assert(normalized.size() == 3U);
  assert(normalized[1].path == "/var");
  assert(normalized[2].path == "/var/tmp");
  assert(normalized[2].current);

  assert(directory_breadcrumbs("").empty());
  assert(directory_breadcrumbs("relative/path").empty());
  return 0;
}
