#include "directory_metadata_display.hpp"

#include <glib.h>

namespace plainroot::filechooser::detail {
namespace {

bool is_size_sort(DirectorySortOrder order) {
  return order == DirectorySortOrder::size_ascending ||
         order == DirectorySortOrder::size_descending;
}

bool is_type_sort(DirectorySortOrder order) {
  return order == DirectorySortOrder::type_ascending ||
         order == DirectorySortOrder::type_descending;
}

bool is_modified_sort(DirectorySortOrder order) {
  return order == DirectorySortOrder::modified_newest_first ||
         order == DirectorySortOrder::modified_oldest_first;
}

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

DirectoryMetadataDisplay format_directory_metadata_display(
    const DirectoryEntry &entry, DirectorySortOrder order,
    const std::string &folder_label, const std::string &unknown_type_label) {
  if (is_size_sort(order)) {
    if (entry.kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY)
      return {"Type", folder_label};
    if (entry.metadata_available)
      return {"Size", size_value(entry)};
  } else if (is_type_sort(order)) {
    return {"Type", type_value(entry, folder_label, unknown_type_label)};
  } else if (is_modified_sort(order) && entry.metadata_available) {
    return {"Modified", modified_value(entry)};
  }
  return {};
}

} // namespace plainroot::filechooser::detail
