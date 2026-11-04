#include "../src/location_path.hpp"

#include <cassert>
#include <string>

using plainroot::filechooser::detail::LocationPathStatus;
using plainroot::filechooser::detail::resolve_location_path;

int main() {
  constexpr std::string_view current = "/tmp/work";
  constexpr std::string_view home = "/home/test";

  const auto absolute = resolve_location_path(
      current, "/tmp/a/../documents", home);
  assert(absolute.status == LocationPathStatus::valid);
  assert(absolute.path == "/tmp/documents");

  const auto relative = resolve_location_path(
      current, "drafts/../report.txt", home);
  assert(relative.status == LocationPathStatus::valid);
  assert(relative.path == "/tmp/work/report.txt");

  const auto current_folder = resolve_location_path(current, ".", home);
  assert(current_folder.status == LocationPathStatus::valid);
  assert(current_folder.path == current);

  const auto home_folder = resolve_location_path(current, "~", home);
  assert(home_folder.status == LocationPathStatus::valid);
  assert(home_folder.path == home);

  const auto home_child = resolve_location_path(current, "~/Documents", home);
  assert(home_child.status == LocationPathStatus::valid);
  assert(home_child.path == "/home/test/Documents");

  const auto tilde_name = resolve_location_path(current, "~other", home);
  assert(tilde_name.status == LocationPathStatus::valid);
  assert(tilde_name.path == "/tmp/work/~other");

  assert(resolve_location_path(current, "", home).status ==
         LocationPathStatus::empty);
  assert(resolve_location_path(current, std::string("bad\0path", 8), home)
             .status == LocationPathStatus::invalid_encoding);
  assert(resolve_location_path(current, std::string("\xff", 1), home).status ==
         LocationPathStatus::invalid_encoding);
  assert(resolve_location_path("relative", "child", home).status ==
         LocationPathStatus::invalid_base);
  assert(resolve_location_path(current, "~", "relative").status ==
         LocationPathStatus::invalid_base);

  const std::string too_long(4096, 'x');
  assert(resolve_location_path(current, too_long, home).status ==
         LocationPathStatus::too_long);
  return 0;
}
