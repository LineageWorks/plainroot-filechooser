#include "../src/open_filename.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits.h>
#include <string>

using plainroot::filechooser::detail::OpenFilenameStatus;
using plainroot::filechooser::detail::resolve_open_filename;

int main() {
  char temporary_directory[] = "/tmp/plainroot-filechooser-open-XXXXXX";
  const char *created = mkdtemp(temporary_directory);
  assert(created != nullptr);
  const std::filesystem::path directory(created);
  const std::filesystem::path file = directory / "document.txt";
  const std::filesystem::path folder = directory / "folder";
  {
    std::ofstream output(file);
    assert(output.good());
    output << "file chooser test";
  }
  std::filesystem::create_directory(folder);

  assert(resolve_open_filename(directory.string(), "").status ==
         OpenFilenameStatus::empty);
  assert(resolve_open_filename(directory.string(), "../document.txt").status ==
         OpenFilenameStatus::invalid_component);
  assert(resolve_open_filename(directory.string(), std::string("bad\0name", 8))
             .status == OpenFilenameStatus::invalid_component);
  assert(resolve_open_filename(directory.string(), "missing.txt").status ==
         OpenFilenameStatus::not_found);
  assert(resolve_open_filename(directory.string(), "folder").status ==
         OpenFilenameStatus::directory);

  const auto valid = resolve_open_filename(directory.string(), "document.txt");
  assert(valid.status == OpenFilenameStatus::valid);
  assert(valid.path == file.string());

  const auto too_long = resolve_open_filename(
      directory.string(), std::string(static_cast<size_t>(NAME_MAX) + 1U, 'x'));
  assert(too_long.status == OpenFilenameStatus::too_long);

  const auto invalid_encoding =
      resolve_open_filename(directory.string(), std::string("\xff", 1));
  assert(invalid_encoding.status == OpenFilenameStatus::invalid_encoding);

  std::filesystem::remove_all(directory);
  return 0;
}
