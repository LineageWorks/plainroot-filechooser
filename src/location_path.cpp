#include "location_path.hpp"

#include <glib.h>

#include <limits.h>

#include <string>
#include <utility>

namespace plainroot::filechooser::detail {

LocationPathResolution resolve_location_path(
    std::string_view current_directory, std::string_view utf8_location,
    std::string_view home_directory) {
  if (utf8_location.empty())
    return {LocationPathStatus::empty, {}};
  if (utf8_location.find('\0') != std::string_view::npos)
    return {LocationPathStatus::invalid_encoding, {}};
  if (utf8_location.size() > static_cast<size_t>(PATH_MAX))
    return {LocationPathStatus::too_long, {}};
  if (current_directory.empty() || current_directory.front() != '/')
    return {LocationPathStatus::invalid_base, {}};

  GError *error = nullptr;
  gchar *native = g_filename_from_utf8(
      utf8_location.data(), static_cast<glong>(utf8_location.size()), nullptr,
      nullptr, &error);
  if (native == nullptr) {
    if (error != nullptr)
      g_error_free(error);
    return {LocationPathStatus::invalid_encoding, {}};
  }
  if (error != nullptr)
    g_error_free(error);

  std::string input(native);
  g_free(native);
  if (input == "~" || input.starts_with("~/")) {
    if (home_directory.empty() || home_directory.front() != '/')
      return {LocationPathStatus::invalid_base, {}};
    input = std::string(home_directory) + input.substr(1);
  }

  gchar *canonical = g_canonicalize_filename(
      input.c_str(), std::string(current_directory).c_str());
  if (canonical == nullptr)
    return {LocationPathStatus::invalid_encoding, {}};
  std::string result(canonical);
  g_free(canonical);
  if (result.size() >= static_cast<size_t>(PATH_MAX))
    return {LocationPathStatus::too_long, {}};
  return {LocationPathStatus::valid, std::move(result)};
}

std::string location_path_to_utf8(std::string_view native_path) {
  GError *error = nullptr;
  gchar *utf8 = g_filename_to_utf8(
      native_path.data(), static_cast<glong>(native_path.size()), nullptr,
      nullptr, &error);
  if (utf8 != nullptr) {
    std::string result(utf8);
    g_free(utf8);
    if (error != nullptr)
      g_error_free(error);
    return result;
  }
  if (error != nullptr)
    g_error_free(error);

  const std::string terminated(native_path);
  gchar *display = g_filename_display_name(terminated.c_str());
  if (display == nullptr)
    return {};
  std::string result(display);
  g_free(display);
  return result;
}

} // namespace plainroot::filechooser::detail
