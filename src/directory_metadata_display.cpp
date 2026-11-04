#include "directory_metadata_display.hpp"

#include <glib.h>

#include <utility>

namespace plainroot::filechooser::detail {
namespace {

std::string type_value(const DirectoryEntry &entry,
                       const std::string &folder_label,
                       const std::string &unknown_type_label) {
  if (entry.kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY)
    return folder_label;
  if (!entry.mime_types.empty())
    return entry.mime_types.front();

  const size_t extension = entry.name.rfind('.');
  if (extension != std::string::npos && extension > 0U &&
      extension + 1U < entry.name.size())
    return entry.name.substr(extension + 1U);
  return unknown_type_label;
}

std::string size_value(const DirectoryEntry &entry) {
  gchar *formatted = g_format_size(entry.size_bytes);
  if (formatted == nullptr)
    return {};
  std::string value(formatted);
  g_free(formatted);
  return value;
}

std::string modified_value(const DirectoryEntry &entry) {
  GDateTime *date = g_date_time_new_from_unix_local(
      static_cast<gint64>(entry.modified_unix_seconds));
  if (date == nullptr)
    return {};
  gchar *formatted = g_date_time_format(date, "%Y-%m-%d %H:%M");
  g_date_time_unref(date);
  if (formatted == nullptr)
    return {};
  std::string value(formatted);
  g_free(formatted);
  return value;
}

} // namespace

std::vector<DirectoryMetadataDisplay> format_directory_metadata_displays(
    const DirectoryEntry &entry, const std::string &folder_label,
    const std::string &unknown_type_label) {
  std::vector<DirectoryMetadataDisplay> fields;
  fields.reserve(3U);
  if (entry.metadata_available &&
      entry.kind == PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE) {
    std::string value = size_value(entry);
    if (!value.empty())
      fields.push_back({"Size", std::move(value), true});
  }

  std::string type = type_value(entry, folder_label, unknown_type_label);
  const bool type_is_ltr = entry.kind != PLAINROOT_CORE_FS_ENTRY_DIRECTORY &&
                           type != unknown_type_label;
  fields.push_back({"Type", std::move(type), type_is_ltr});

  if (entry.metadata_available) {
    std::string value = modified_value(entry);
    if (!value.empty())
      fields.push_back({"Modified", std::move(value), true});
  }

  return fields;
}

} // namespace plainroot::filechooser::detail
