#include "../src/save_files.hpp"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits.h>
#include <string>
#include <vector>

using plainroot::filechooser::detail::SaveFilesStatus;
using plainroot::filechooser::detail::parse_portal_save_files;
using plainroot::filechooser::detail::resolve_save_files;

namespace {

sd_bus *test_bus = nullptr;

sd_bus_message *make_message() {
  if (test_bus == nullptr)
    assert(sd_bus_open_user(&test_bus) >= 0);
  sd_bus_message *message = nullptr;
  assert(sd_bus_message_new_method_call(
             test_bus, &message, "org.plainroot.FileChooser.Test",
             "/org/plainroot/FileChooser/Test",
             "org.plainroot.FileChooser.Test", "Test") >= 0);
  return message;
}

sd_bus_message *make_file_array(
    const std::vector<std::vector<unsigned char>> &encoded_names) {
  sd_bus_message *message = make_message();
  assert(sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "ay") >= 0);
  for (const auto &encoded_name : encoded_names) {
    assert(sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "y") >=
           0);
    for (const unsigned char byte : encoded_name)
      assert(sd_bus_message_append_basic(message, SD_BUS_TYPE_BYTE, &byte) >=
             0);
    assert(sd_bus_message_close_container(message) >= 0);
  }
  assert(sd_bus_message_close_container(message) >= 0);
  assert(sd_bus_message_seal(message, 1U, 0U) >= 0);
  assert(sd_bus_message_rewind(message, true) >= 0);
  return message;
}

std::vector<unsigned char> encoded(std::string name) {
  name.push_back('\0');
  return {name.begin(), name.end()};
}

} // namespace

int main() {
  std::vector<std::string> filenames;
  sd_bus_message *message = make_file_array(
      {encoded("report.txt"), encoded("photo.png"), encoded(".hidden"),
       encoded(std::string("native-\xff", 8))});
  assert(parse_portal_save_files(message, filenames));
  assert(filenames.size() == 4U);
  assert(filenames[0] == "report.txt");
  assert(filenames[1] == "photo.png");
  assert(filenames[2] == ".hidden");
  assert(filenames[3] == std::string("native-\xff", 8));
  sd_bus_message_unref(message);

  message = make_file_array({});
  filenames.clear();
  assert(!parse_portal_save_files(message, filenames));
  sd_bus_message_unref(message);

  message = make_file_array({{'b', 'a', 'd'}});
  assert(!parse_portal_save_files(message, filenames));
  sd_bus_message_unref(message);

  message = make_file_array({{'b', '\0', 'a', '\0'}});
  assert(!parse_portal_save_files(message, filenames));
  sd_bus_message_unref(message);

  message = make_file_array({encoded("../escape")});
  assert(!parse_portal_save_files(message, filenames));
  sd_bus_message_unref(message);

  message = make_file_array({encoded("one"), encoded("one")});
  assert(parse_portal_save_files(message, filenames));
  assert(filenames.size() == 2U && filenames[0] == filenames[1]);
  sd_bus_message_unref(message);

  std::vector<std::vector<unsigned char>> excessive;
  excessive.reserve(257U);
  for (size_t index = 0; index <= 256U; ++index)
    excessive.push_back(encoded("file-" + std::to_string(index)));
  message = make_file_array(excessive);
  assert(!parse_portal_save_files(message, filenames));
  sd_bus_message_unref(message);

  char temporary_directory[] = "/tmp/plainroot-filechooser-save-XXXXXX";
  const char *created = mkdtemp(temporary_directory);
  assert(created != nullptr);
  const std::filesystem::path directory(created);
  {
    std::ofstream existing(directory / "report.txt");
    assert(existing.good());
  }
  const std::string maximum_name(static_cast<size_t>(NAME_MAX), 'x');
  {
    std::ofstream existing(directory / maximum_name);
    assert(existing.good());
  }

  const auto resolved = resolve_save_files(
      directory.string(),
      {"report.txt", "report.txt", "report (2).txt", ".hidden", ".hidden"});
  assert(resolved.status == SaveFilesStatus::success);
  assert(resolved.paths.size() == 5U);
  assert(resolved.paths[0] == (directory / "report (3).txt").string());
  assert(resolved.paths[1] == (directory / "report (4).txt").string());
  assert(resolved.paths[2] == (directory / "report (2).txt").string());
  assert(resolved.paths[3] == (directory / ".hidden").string());
  assert(resolved.paths[4] == (directory / ".hidden (2)").string());
  assert(!std::filesystem::exists(directory / "report (3).txt"));
  const auto maximum_name_result =
      resolve_save_files(directory.string(), {maximum_name});
  assert(maximum_name_result.status == SaveFilesStatus::success);
  assert(std::filesystem::path(maximum_name_result.paths.front())
             .filename()
             .string()
             .size() == static_cast<size_t>(NAME_MAX));
  assert(std::filesystem::path(maximum_name_result.paths.front())
             .filename()
             .string()
             .ends_with(" (2)"));

  assert(resolve_save_files("relative", {"file"}).status ==
         SaveFilesStatus::invalid_directory);
  assert(resolve_save_files((directory / "missing").string(), {"file"})
             .status == SaveFilesStatus::invalid_directory);
  assert(resolve_save_files((directory / "report.txt").string(), {"file"})
             .status == SaveFilesStatus::invalid_directory);
  assert(resolve_save_files(directory.string(), {}).status ==
         SaveFilesStatus::no_files);
  assert(resolve_save_files(directory.string(), {"../escape"}).status ==
         SaveFilesStatus::invalid_name);
  assert(resolve_save_files(
             directory.string(),
             {std::string(static_cast<size_t>(NAME_MAX) + 1U, 'x')})
             .status == SaveFilesStatus::name_too_long);
  assert(resolve_save_files(
             "/" + std::string(static_cast<size_t>(PATH_MAX), 'x'), {"file"})
             .status == SaveFilesStatus::path_too_long);

  std::filesystem::remove_all(directory);
  test_bus = sd_bus_unref(test_bus);
  return 0;
}
