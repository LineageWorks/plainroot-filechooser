#include "open_filename.hpp"

#include <glib.h>

#include <cerrno>
#include <cstdlib>
#include <limits.h>
#include <string>
#include <sys/stat.h>
#include <utility>

namespace plainroot::filechooser::detail {
namespace {

std::string join_path(const std::string &directory, const std::string &name) {
  if (directory.empty() || directory == "/")
    return "/" + name;
  if (directory.back() == '/')
    return directory + name;
  return directory + "/" + name;
}

} // namespace

OpenFilenameResolution resolve_open_filename(const std::string &directory,
                                             const std::string &utf8_name) {
  if (utf8_name.empty())
    return {OpenFilenameStatus::empty, {}};
  if (utf8_name == "." || utf8_name == ".." ||
      utf8_name.find('\0') != std::string::npos ||
      utf8_name.find('/') != std::string::npos)
    return {OpenFilenameStatus::invalid_component, {}};

  GError *conversion_error = nullptr;
  gchar *native_name = g_filename_from_utf8(
      utf8_name.data(), static_cast<glong>(utf8_name.size()), nullptr, nullptr,
      &conversion_error);
  if (native_name == nullptr) {
    if (conversion_error != nullptr)
      g_error_free(conversion_error);
    return {OpenFilenameStatus::invalid_encoding, {}};
  }
  if (conversion_error != nullptr)
    g_error_free(conversion_error);

  const std::string native_name_string(native_name);
  g_free(native_name);
  if (native_name_string.size() > NAME_MAX)
    return {OpenFilenameStatus::too_long, {}};

  std::string path = join_path(directory, native_name_string);
  struct stat info{};
  if (stat(path.c_str(), &info) != 0) {
    const int error_code = errno;
    return {error_code == ENOENT ? OpenFilenameStatus::not_found
                                 : OpenFilenameStatus::io_error,
            {}};
  }
  if (S_ISDIR(info.st_mode))
    return {OpenFilenameStatus::directory, std::move(path)};
  return {OpenFilenameStatus::valid, std::move(path)};
}

} // namespace plainroot::filechooser::detail
