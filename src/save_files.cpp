#include "save_files.hpp"

#include <cerrno>
#include <cstring>
#include <limits.h>
#include <string_view>
#include <sys/stat.h>
#include <unordered_set>
#include <utility>

namespace plainroot::filechooser::detail {
namespace {

bool valid_filename_component(std::string_view filename) {
  return !filename.empty() && filename != "." && filename != ".." &&
         filename.find('\0') == std::string_view::npos &&
         filename.find('/') == std::string_view::npos;
}

std::string join_path(std::string_view directory, std::string_view filename) {
  if (directory == "/")
    return "/" + std::string(filename);
  std::string path(directory);
  if (path.back() != '/')
    path.push_back('/');
  path.append(filename);
  return path;
}

std::string numbered_name(std::string_view filename, size_t number) {
  const size_t dot = filename.find_last_of('.');
  const size_t extension_start =
      dot != std::string_view::npos && dot != 0U ? dot : filename.size();
  const std::string_view stem = filename.substr(0, extension_start);
  const std::string_view extension = filename.substr(extension_start);
  const std::string suffix = " (" + std::to_string(number) + ")";
  if (suffix.size() + extension.size() > NAME_MAX)
    return {};
  const size_t maximum_stem = NAME_MAX - suffix.size() - extension.size();
  std::string result(stem.substr(0, maximum_stem));
  result += suffix;
  result += extension;
  return result;
}

bool path_is_available(const std::string &path, bool &available) {
  struct stat info {};
  if (lstat(path.c_str(), &info) == 0) {
    available = false;
    return true;
  }
  if (errno == ENOENT) {
    available = true;
    return true;
  }
  return false;
}

} // namespace

bool parse_portal_save_files(sd_bus_message *message,
                             std::vector<std::string> &filenames) {
  filenames.clear();
  int result = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY,
                                               "ay");
  if (result < 0)
    return false;

  size_t total_bytes = 0;
  while (true) {
    char type = 0;
    const char *contents = nullptr;
    result = sd_bus_message_peek_type(message, &type, &contents);
    if (result == 0)
      break;
    if (result < 0 || type != SD_BUS_TYPE_ARRAY || contents == nullptr ||
        std::strcmp(contents, "y") != 0 ||
        filenames.size() >= kMaximumSaveFiles)
      return false;

    const void *bytes = nullptr;
    size_t size = 0;
    result = sd_bus_message_read_array(message, SD_BUS_TYPE_BYTE, &bytes,
                                       &size);
    if (result < 0 || bytes == nullptr || size < 2U ||
        size > 1U + static_cast<size_t>(NAME_MAX) || size > 64U * 1024U ||
        total_bytes > 64U * 1024U - size)
      return false;
    const auto *name = static_cast<const char *>(bytes);
    if (name[size - 1U] != '\0' ||
        std::memchr(name, '\0', size - 1U) != nullptr)
      return false;
    const std::string_view filename(name, size - 1U);
    if (!valid_filename_component(filename))
      return false;
    total_bytes += size;
    filenames.emplace_back(filename);
  }
  if (result < 0)
    return false;
  return sd_bus_message_exit_container(message) >= 0 && !filenames.empty();
}

SaveFilesResolution resolve_save_files(
    const std::string &directory,
    const std::vector<std::string> &filenames) {
  if (directory.empty() || directory.front() != '/' ||
      directory.find('\0') != std::string::npos)
    return {SaveFilesStatus::invalid_directory, {}};
  if (directory.size() >= PATH_MAX)
    return {SaveFilesStatus::path_too_long, {}};
  if (filenames.empty())
    return {SaveFilesStatus::no_files, {}};
  if (filenames.size() > kMaximumSaveFiles)
    return {SaveFilesStatus::too_many_files, {}};

  struct stat directory_info {};
  if (stat(directory.c_str(), &directory_info) != 0)
    return {errno == ENOENT || errno == ENOTDIR
                ? SaveFilesStatus::invalid_directory
                : SaveFilesStatus::filesystem_error,
            {}};
  if (!S_ISDIR(directory_info.st_mode))
    return {SaveFilesStatus::invalid_directory, {}};

  std::unordered_set<std::string> requested_names;
  std::unordered_set<std::string> allocated_names;
  requested_names.reserve(filenames.size());
  allocated_names.reserve(filenames.size());
  for (const std::string &filename : filenames) {
    if (!valid_filename_component(filename))
      return {SaveFilesStatus::invalid_name, {}};
    if (filename.size() > NAME_MAX)
      return {SaveFilesStatus::name_too_long, {}};
    requested_names.insert(filename);
  }

  SaveFilesResolution resolution;
  resolution.status = SaveFilesStatus::success;
  resolution.paths.reserve(filenames.size());
  for (const std::string &filename : filenames) {
    std::string candidate = filename;
    bool available = false;
    std::string path = join_path(directory, candidate);
    if (path.size() >= PATH_MAX)
      return {SaveFilesStatus::path_too_long, {}};
    if (!path_is_available(path, available))
      return {SaveFilesStatus::filesystem_error, {}};

    if (!available || allocated_names.contains(candidate)) {
      bool found = false;
      for (size_t number = 2U; number < 1'000'000U; ++number) {
        candidate = numbered_name(filename, number);
        if (candidate.empty())
          return {SaveFilesStatus::name_too_long, {}};
        if (requested_names.contains(candidate) ||
            allocated_names.contains(candidate))
          continue;
        path = join_path(directory, candidate);
        if (path.size() >= PATH_MAX)
          return {SaveFilesStatus::path_too_long, {}};
        if (!path_is_available(path, available))
          return {SaveFilesStatus::filesystem_error, {}};
        if (available) {
          found = true;
          break;
        }
      }
      if (!found)
        return {SaveFilesStatus::filesystem_error, {}};
    }

    allocated_names.insert(candidate);
    resolution.paths.push_back(std::move(path));
  }
  return resolution;
}

} // namespace plainroot::filechooser::detail
