#include "../src/directory_loader.hpp"

#include <plainroot/toolkit/toolkit.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Results {
  std::thread::id owner = std::this_thread::get_id();
  bool delivered_on_owner_thread = true;
  std::vector<plainroot::filechooser::detail::DirectoryResult> values;

  void append(plainroot::filechooser::detail::DirectoryResult result) {
    delivered_on_owner_thread &= std::this_thread::get_id() == owner;
    values.push_back(std::move(result));
  }
};

struct PostedTask {
  plainroot_toolkit_application_task_fn callback = nullptr;
  void *user_data = nullptr;
  plainroot_toolkit_application_task_destroy_fn destroy = nullptr;
};

struct TaskQueue {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<PostedTask> tasks;

  void push(PostedTask task) {
    std::lock_guard lock(mutex);
    tasks.push_back(task);
    changed.notify_all();
  }

  bool dispatch_one() {
    PostedTask task{};
    {
      std::unique_lock lock(mutex);
      if (!changed.wait_for(lock, std::chrono::seconds(5),
                            [this] { return !tasks.empty(); }))
        return false;
      task = tasks.front();
      tasks.pop_front();
    }
    task.callback(task.user_data);
    if (task.destroy != nullptr)
      task.destroy(task.user_data);
    return true;
  }

  void clear() {
    std::deque<PostedTask> abandoned;
    {
      std::lock_guard lock(mutex);
      abandoned.swap(tasks);
    }
    for (const PostedTask &task : abandoned) {
      if (task.destroy != nullptr)
        task.destroy(task.user_data);
    }
  }
};

TaskQueue task_queue;

} // namespace

extern "C" plainroot_toolkit_result_v1 plainroot_toolkit_application_post_v1(
    plainroot_toolkit_application *,
    plainroot_toolkit_application_task_fn callback, void *user_data,
    plainroot_toolkit_application_task_destroy_fn destroy) {
  if (callback == nullptr)
    return PLAINROOT_TOOLKIT_RESULT_INVALID_ARGUMENT;
  task_queue.push({callback, user_data, destroy});
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
  loader.inspect_location((root / "target-folder").native(), 8, 101);
  loader.inspect_location((root / "target-file.txt").native(), 8, 102);
  loader.inspect_location((root / "missing-location").native(), 8, 103);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
  while (results.values.size() < 6 &&
         std::chrono::steady_clock::now() < deadline) {
    if (!task_queue.dispatch_one())
      break;
  }
  if (results.values.size() != 6 || !results.delivered_on_owner_thread) {
    loader.stop();
    task_queue.clear();
    fs::remove_all(root, error);
    return 7;
  }
  loader.stop();

  bool folder_ok = false;
  bool file_ok = false;
  bool dangling_rejected = false;
  bool location_folder_ok = false;
  bool location_file_ok = false;
  bool missing_location_rejected = false;
  for (std::size_t index = 0; index < results.values.size(); ++index) {
    const auto &result = results.values[index];
    if (result.operation == DirectoryResult::Operation::InspectSymlink &&
        (result.generation != 7 || result.selection_probe))
      return 8;
    if (result.operation == DirectoryResult::Operation::InspectLocation &&
        result.generation != 8)
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
    else if (result.path == (root / "target-folder").native())
      location_folder_ok =
          result.operation == DirectoryResult::Operation::InspectLocation &&
          result.request_id == 101 && result.result == PLAINROOT_CORE_FS_OK &&
          result.inspected_kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY;
    else if (result.path == (root / "target-file.txt").native())
      location_file_ok =
          result.operation == DirectoryResult::Operation::InspectLocation &&
          result.request_id == 102 && result.result == PLAINROOT_CORE_FS_OK &&
          result.inspected_kind == PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE;
    else if (result.path == (root / "missing-location").native())
      missing_location_rejected =
          result.operation == DirectoryResult::Operation::InspectLocation &&
          result.request_id == 103 && result.result != PLAINROOT_CORE_FS_OK;
  }

  fs::remove_all(root, error);
  return folder_ok && file_ok && dangling_rejected && location_folder_ok &&
                 location_file_ok && missing_location_rejected
             ? 0
             : 9;
}
