#include "../src/directory_loader.hpp"

#include <plainroot/toolkit/toolkit.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct Results {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<plainroot::filechooser::detail::DirectoryResult> values;

  void append(plainroot::filechooser::detail::DirectoryResult result) {
    std::lock_guard lock(mutex);
    values.push_back(std::move(result));
    changed.notify_all();
  }

  bool wait_for_count(std::size_t count) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(5),
                            [this, count] { return values.size() >= count; });
  }

  plainroot::filechooser::detail::DirectoryResult at(std::size_t index) {
    std::lock_guard lock(mutex);
    return values.at(index);
  }
};

} // namespace

extern "C" plainroot_toolkit_result_v1 plainroot_toolkit_application_post_v1(
    plainroot_toolkit_application *,
    plainroot_toolkit_application_task_fn callback, void *user_data,
    plainroot_toolkit_application_task_destroy_fn destroy) {
  if (callback == nullptr)
    return PLAINROOT_TOOLKIT_RESULT_INVALID_ARGUMENT;
  callback(user_data);
  if (destroy != nullptr)
    destroy(user_data);
  return PLAINROOT_TOOLKIT_RESULT_OK;
}

int main() {
  namespace fs = std::filesystem;
  using plainroot::filechooser::detail::DirectoryLoader;
  using plainroot::filechooser::detail::DirectoryResult;

  const fs::path root = fs::temp_directory_path() /
                        ("plainroot-filechooser-link-test-" +
                         std::to_string(static_cast<unsigned long long>(
                             std::chrono::steady_clock::now()
                                 .time_since_epoch()
                                 .count())));
  std::error_code error;
  if (!fs::create_directories(root / "target-folder", error) || error)
    return 1;
  {
    std::ofstream file(root / "target-file.txt");
    if (!file)
      return 2;
    file << "test";
  }
  fs::create_directory_symlink(root / "target-folder", root / "folder-link",
                                error);
  if (error)
    return 3;
  fs::create_symlink(root / "target-file.txt", root / "file-link", error);
  if (error)
    return 4;
  fs::create_symlink(root / "missing-target", root / "dangling-link", error);
  if (error)
    return 5;

  Results results;
  auto *fake_application =
      reinterpret_cast<plainroot_toolkit_application *>(uintptr_t{1});
  DirectoryLoader loader(fake_application, [&results](DirectoryResult result) {
    results.append(std::move(result));
  });
  if (!loader.ready())
    return 6;

  loader.inspect_symlink((root / "folder-link").native(), 7);
  loader.inspect_symlink((root / "file-link").native(), 7);
  loader.inspect_symlink((root / "dangling-link").native(), 7);
  if (!results.wait_for_count(3))
    return 7;
  loader.stop();

  bool folder_ok = false;
  bool file_ok = false;
  bool dangling_rejected = false;
  for (std::size_t index = 0; index < 3; ++index) {
    const auto result = results.at(index);
    if (result.operation != DirectoryResult::Operation::InspectSymlink ||
        result.generation != 7 || result.selection_probe)
      return 8;
    if (result.path == (root / "folder-link").native())
      folder_ok = result.result == PLAINROOT_CORE_FS_OK &&
                  result.inspected_kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY;
    else if (result.path == (root / "file-link").native())
      file_ok = result.result == PLAINROOT_CORE_FS_OK &&
                result.inspected_kind ==
                    PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE;
    else if (result.path == (root / "dangling-link").native())
      dangling_rejected = result.result != PLAINROOT_CORE_FS_OK;
  }

  fs::remove_all(root, error);
  return folder_ok && file_ok && dangling_rejected ? 0 : 9;
}
