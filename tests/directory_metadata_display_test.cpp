#include "../src/directory_metadata_display.hpp"

#include <cassert>
#include <string>

using plainroot::filechooser::detail::DirectoryEntry;
using plainroot::filechooser::detail::DirectorySortOrder;
using plainroot::filechooser::detail::format_directory_metadata_display;

int main() {
  DirectoryEntry file{};
  file.name = "notes.md";
  file.kind = PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE;
  file.size_bytes = 4096;
  file.modified_unix_seconds = 1'762'358'400;
  file.metadata_available = true;
  file.mime_types = {"text/markdown"};

  const auto size = format_directory_metadata_display(
      file, DirectorySortOrder::size_ascending, "Folder", "Unknown type");
  assert(size.label_message == "Size");
  assert(!size.value.empty());

  const auto type = format_directory_metadata_display(
      file, DirectorySortOrder::type_ascending, "Folder", "Unknown type");
  assert(type.label_message == "Type");
  assert(type.value == "text/markdown");

  const auto modified = format_directory_metadata_display(
      file, DirectorySortOrder::modified_newest_first, "Folder",
      "Unknown type");
  assert(modified.label_message == "Modified");
  assert(!modified.value.empty());

  DirectoryEntry extension_only{};
  extension_only.name = "notes.md";
  extension_only.kind = PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE;
  const auto fallback_type = format_directory_metadata_display(
      extension_only, DirectorySortOrder::type_ascending, "Folder",
      "Unknown type");
  assert(fallback_type.value == "md");

  DirectoryEntry directory{};
  directory.name = "documents";
  directory.kind = PLAINROOT_CORE_FS_ENTRY_DIRECTORY;
  directory.metadata_available = true;
  const auto folder = format_directory_metadata_display(
      directory, DirectorySortOrder::size_ascending, "Folder",
      "Unknown type");
  assert(folder.label_message == "Type");
  assert(folder.value == "Folder");

  const auto unavailable = format_directory_metadata_display(
      extension_only, DirectorySortOrder::size_ascending, "Folder",
      "Unknown type");
  assert(unavailable.label_message.empty());
  assert(unavailable.value.empty());

  return 0;
}
