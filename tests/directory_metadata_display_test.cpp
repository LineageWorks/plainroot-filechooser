#include "../src/directory_metadata_display.hpp"

#include <cassert>
#include <string>
#include <vector>

using plainroot::filechooser::detail::DirectoryEntry;
using plainroot::filechooser::detail::DirectoryMetadataDisplay;
using plainroot::filechooser::detail::format_directory_metadata_displays;

namespace {

const DirectoryMetadataDisplay *find_field(
    const std::vector<DirectoryMetadataDisplay> &fields,
    const std::string &label) {
  for (const DirectoryMetadataDisplay &field : fields) {
    if (field.label_message == label)
      return &field;
  }
  return nullptr;
}

} // namespace

int main() {
  DirectoryEntry file{};
  file.name = "notes.md";
  file.kind = PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE;
  file.size_bytes = 4096;
  file.modified_unix_seconds = 1'762'358'400;
  file.metadata_available = true;
  file.mime_types = {"text/markdown"};

  const auto file_fields = format_directory_metadata_displays(
      file, "Folder", "Unknown type");
  assert(file_fields.size() == 3U);
  const auto *size = find_field(file_fields, "Size");
  assert(size != nullptr && !size->value.empty() && size->value_is_ltr);
  const auto *type = find_field(file_fields, "Type");
  assert(type != nullptr && type->value == "text/markdown" &&
         type->value_is_ltr);
  const auto *modified = find_field(file_fields, "Modified");
  assert(modified != nullptr && !modified->value.empty() &&
         modified->value_is_ltr);

  DirectoryEntry extension_only{};
  extension_only.name = "notes.md";
  extension_only.kind = PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE;
  const auto fallback_fields = format_directory_metadata_displays(
      extension_only, "Folder", "Unknown type");
  assert(fallback_fields.size() == 1U);
  const auto *fallback_type = find_field(fallback_fields, "Type");
  assert(fallback_type != nullptr && fallback_type->value == "md" &&
         fallback_type->value_is_ltr);

  DirectoryEntry extensionless{};
  extensionless.name = "README";
  extensionless.kind = PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE;
  const auto unknown_fields = format_directory_metadata_displays(
      extensionless, "Folder", "Unknown type");
  const auto *unknown_type = find_field(unknown_fields, "Type");
  assert(unknown_type != nullptr && unknown_type->value == "Unknown type" &&
         !unknown_type->value_is_ltr);

  DirectoryEntry directory{};
  directory.name = "documents";
  directory.kind = PLAINROOT_CORE_FS_ENTRY_DIRECTORY;
  directory.metadata_available = true;
  const auto folder_fields = format_directory_metadata_displays(
      directory, "Folder", "Unknown type");
  assert(folder_fields.size() == 2U);
  assert(find_field(folder_fields, "Size") == nullptr);
  const auto *folder_type = find_field(folder_fields, "Type");
  assert(folder_type != nullptr && folder_type->value == "Folder" &&
         !folder_type->value_is_ltr);
  assert(find_field(folder_fields, "Modified") != nullptr);

  const auto unavailable_fields = format_directory_metadata_displays(
      extension_only, "Folder", "Unknown type");
  assert(find_field(unavailable_fields, "Size") == nullptr);
  assert(find_field(unavailable_fields, "Modified") == nullptr);

  return 0;
}
