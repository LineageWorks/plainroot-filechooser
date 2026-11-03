#include "directory_loader.hpp"

#include "mime_database.hpp"
#include "recent_files.hpp"

#include <atomic>
#include <cerrno>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <poll.h>
#include <thread>
#include <utility>
#include <sys/stat.h>

#include <sys/eventfd.h>
#include <unistd.h>

namespace plainroot::filechooser::detail {

class DirectoryLoader::Impl {
public:
  using ResultHandler = std::function<void(DirectoryResult)>;

  Impl(plainroot_toolkit_application *application, ResultHandler result_handler)
      : application_(application), result_handler_(std::move(result_handler)),
        notification_fd_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
    if (notification_fd_ >= 0)
      worker_ = std::thread([this] { run(); });
  }

  ~Impl() { stop(); }

  bool ready() const {
    return notification_fd_ >= 0 && worker_.joinable() &&
           !startup_failed_.load(std::memory_order_acquire);
  }

  void request(std::string path, uint64_t generation, bool classify_mime) {
    bool failed = false;
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        return;
      latest_generation_.store(generation, std::memory_order_release);
      failed = startup_failed_.load(std::memory_order_relaxed);
      if (!failed) {
        std::erase_if(jobs_, [](const Job &job) {
          return job.operation == DirectoryResult::Operation::Listing;
        });
        jobs_.push_back({DirectoryResult::Operation::Listing,
                         std::move(path), generation, 0, classify_mime, {},
                         {}});
      }
    }
    if (failed) {
      DirectoryResult result{};
      result.generation = generation;
      result.path = std::move(path);
      result.result = PLAINROOT_CORE_FS_IO_ERROR;
      publish(std::move(result));
      return;
    }
    wake_worker();
  }

  void create_directory(std::string path, uint64_t generation,
                        uint64_t request_id) {
    bool failed = false;
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        return;
      failed = startup_failed_.load(std::memory_order_relaxed);
      if (!failed)
        jobs_.push_back({DirectoryResult::Operation::CreateDirectory,
                         std::move(path), generation, request_id, false, {},
                         {}});
    }
    if (failed) {
      DirectoryResult result{};
      result.operation = DirectoryResult::Operation::CreateDirectory;
      result.generation = generation;
      result.request_id = request_id;
      result.path = std::move(path);
      result.result = PLAINROOT_CORE_FS_IO_ERROR;
      publish(std::move(result));
      return;
    }
    wake_worker();
  }

  void inspect_symlink(std::string path, uint64_t generation) {
    bool failed = false;
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        return;
      failed = startup_failed_.load(std::memory_order_relaxed);
      if (!failed)
        jobs_.push_back({DirectoryResult::Operation::InspectSymlink,
                         std::move(path), generation, 0, false, {}, {}});
    }
    if (failed) {
      DirectoryResult result{};
      result.operation = DirectoryResult::Operation::InspectSymlink;
      result.generation = generation;
      result.path = std::move(path);
      result.result = PLAINROOT_CORE_FS_IO_ERROR;
      publish(std::move(result));
      return;
    }
    wake_worker();
  }

  void request_recent(std::string path, uint64_t generation,
                      bool classify_mime) {
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        return;
      latest_generation_.store(generation, std::memory_order_release);
      std::erase_if(jobs_, [](const Job &job) {
        return job.operation == DirectoryResult::Operation::Listing ||
               job.operation == DirectoryResult::Operation::RecentListing;
      });
      jobs_.push_back({DirectoryResult::Operation::RecentListing,
                       std::move(path), generation, 0, classify_mime, {}, {}});
    }
    wake_worker();
  }

  void record_recent(std::vector<std::string> paths, std::string xbel_path,
                     std::string application_id) {
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        return;
      for (auto &path : paths) {
        Job job{};
        job.operation = DirectoryResult::Operation::RecordRecent;
        job.path = std::move(path);
        job.secondary_path = xbel_path;
        job.application_id = application_id;
        jobs_.push_back(std::move(job));
      }
    }
    wake_worker();
  }

  void stop() {
    {
      std::lock_guard lock(mutex_);
      if (stopping_)
        return;
      stopping_ = true;
      latest_generation_.fetch_add(1, std::memory_order_acq_rel);
    }
    wake_worker();
    if (worker_.joinable())
      worker_.join();
    if (notification_fd_ >= 0) {
      close(notification_fd_);
      notification_fd_ = -1;
    }
  }

private:
  struct Job {
    DirectoryResult::Operation operation =
        DirectoryResult::Operation::Listing;
    std::string path;
    uint64_t generation = 0;
    uint64_t request_id = 0;
    bool classify_mime = false;
    std::string secondary_path;
    std::string application_id;
  };

  struct Pending {
    Impl *owner = nullptr;
    Job job;
    plainroot_core_fs_async_request *request = nullptr;
    bool completed = false;
  };

  struct Delivery {
    ResultHandler handler;
    DirectoryResult result;
  };

  static void apply_delivery(void *user_data) {
    auto *delivery = static_cast<Delivery *>(user_data);
    delivery->handler(std::move(delivery->result));
  }

  static void destroy_delivery(void *user_data) {
    delete static_cast<Delivery *>(user_data);
  }

  void publish(DirectoryResult result) {
    auto *delivery =
        new (std::nothrow) Delivery{result_handler_, std::move(result)};
    if (delivery == nullptr)
      return;
    const auto posted = plainroot_toolkit_application_post_v1(
        application_, &Impl::apply_delivery, delivery, &Impl::destroy_delivery);
    if (posted != PLAINROOT_TOOLKIT_RESULT_OK)
      delete delivery;
  }

  static void completed(plainroot_core_fs_async_request *request,
                        plainroot_core_fs_result_v1 result, int32_t native_code,
                        void *user_data) {
    auto &pending = *static_cast<Pending *>(user_data);
    DirectoryResult delivery{};
    delivery.generation = pending.job.generation;
    delivery.request_id = pending.job.request_id;
    delivery.path = pending.job.path;
    delivery.result = result;
    delivery.native_code = native_code;
    delivery.operation = pending.job.operation;
    delivery.mime_classified = pending.job.classify_mime;
    delivery.mime_database_available =
        pending.owner->mime_database_ != nullptr &&
        pending.owner->mime_database_->available();
    if (result == PLAINROOT_CORE_FS_OK &&
        pending.job.operation == DirectoryResult::Operation::Listing) {
      plainroot_core_fs_listing *listing = nullptr;
      delivery.result =
          plainroot_core_fs_async_request_take_listing_v1(request, &listing);
      if (delivery.result == PLAINROOT_CORE_FS_OK && listing != nullptr) {
        const size_t count = plainroot_core_fs_listing_count_v1(listing);
        delivery.entries.reserve(count);
        for (size_t index = 0; index < count; ++index) {
          plainroot_core_fs_entry_v1 entry{};
          entry.struct_size = sizeof(entry);
          if (plainroot_core_fs_listing_get_v1(listing, index, &entry) !=
                  PLAINROOT_CORE_FS_OK ||
              (entry.name == nullptr && entry.name_size != 0U))
            continue;
          DirectoryEntry directory_entry{
              std::string(entry.name == nullptr ? "" : entry.name,
                          entry.name_size),
              entry.kind,
              {}};
          const bool regular_or_symlink =
              directory_entry.kind == PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE ||
              directory_entry.kind == PLAINROOT_CORE_FS_ENTRY_SYMLINK;
          if (pending.job.classify_mime && regular_or_symlink &&
              delivery.mime_database_available) {
            if (pending.owner->latest_generation_.load(
                    std::memory_order_acquire) != pending.job.generation)
              break;
            const std::string native_path =
                pending.job.path == "/"
                    ? "/" + directory_entry.name
                    : pending.job.path +
                          (pending.job.path.ends_with('/') ? "" : "/") +
                          directory_entry.name;
            directory_entry.mime_types =
                pending.owner->mime_database_->types_for_file(
                    native_path, directory_entry.name);
          }
          delivery.entries.push_back(std::move(directory_entry));
        }
      }
      if (listing != nullptr)
        plainroot_core_fs_listing_release_v1(listing);
    }
    if (result == PLAINROOT_CORE_FS_OK &&
        pending.job.operation == DirectoryResult::Operation::InspectSymlink) {
      plainroot_core_fs_metadata_v1 metadata{};
      metadata.struct_size = sizeof(metadata);
      delivery.result = plainroot_core_fs_async_request_get_metadata_v1(
          request, &metadata);
      if (delivery.result == PLAINROOT_CORE_FS_OK)
        delivery.inspected_kind = metadata.kind;
    }
    pending.owner->publish(std::move(delivery));
    pending.completed = true;
  }

  void wake_worker() {
    if (notification_fd_ < 0)
      return;
    const uint64_t value = 1;
    (void)write(notification_fd_, &value, sizeof(value));
  }

  void drain_notifications() {
    uint64_t value = 0;
    while (read(notification_fd_, &value, sizeof(value)) ==
           static_cast<ssize_t>(sizeof(value))) {
    }
  }

  std::vector<Job> take_jobs(bool *stopping) {
    std::lock_guard lock(mutex_);
    *stopping = stopping_;
    std::vector<Job> jobs;
    jobs.assign(std::make_move_iterator(jobs_.begin()),
                std::make_move_iterator(jobs_.end()));
    jobs_.clear();
    return jobs;
  }

  void submit_job(plainroot_core_fs_async_executor *executor, Job job,
                  std::vector<std::unique_ptr<Pending>> *pending) {
    if (job.operation == DirectoryResult::Operation::RecordRecent) {
      (void)record_recent_file(job.secondary_path, job.path,
                               job.application_id);
      return;
    }
    if (job.operation == DirectoryResult::Operation::RecentListing) {
      DirectoryResult result{};
      result.operation = job.operation;
      result.generation = job.generation;
      result.path = job.path;
      result.mime_classified = job.classify_mime;
      result.mime_database_available = mime_database_ != nullptr &&
                                       mime_database_->available();
      std::vector<RecentFile> recent;
      result.result = load_recent_files(job.path, recent)
                          ? PLAINROOT_CORE_FS_OK
                          : PLAINROOT_CORE_FS_IO_ERROR;
      for (auto &file : recent) {
        DirectoryEntry entry{};
        entry.name = std::move(file.name);
        entry.path = std::move(file.path);
        entry.sort_timestamp = file.visited_unix_seconds;
        struct stat metadata {};
        if (stat(entry.path.c_str(), &metadata) != 0 ||
            (!S_ISDIR(metadata.st_mode) && !S_ISREG(metadata.st_mode)))
          continue;
        entry.kind = S_ISDIR(metadata.st_mode)
                         ? PLAINROOT_CORE_FS_ENTRY_DIRECTORY
                         : PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE;
        if (job.classify_mime && result.mime_database_available &&
            entry.kind == PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE)
          entry.mime_types = mime_database_->types_for_file(entry.path,
                                                            entry.name);
        result.entries.push_back(std::move(entry));
      }
      if (latest_generation_.load(std::memory_order_acquire) == job.generation)
        publish(std::move(result));
      return;
    }
    auto next = std::make_unique<Pending>();
    next->owner = this;
    next->job = std::move(job);
    plainroot_core_fs_path_v1 path{};
    path.struct_size = sizeof(path);
    path.data = next->job.path.data();
    path.size = next->job.path.size();
    plainroot_core_fs_result_v1 submitted = PLAINROOT_CORE_FS_INVALID_ARGUMENT;
    if (next->job.operation == DirectoryResult::Operation::CreateDirectory) {
      submitted = plainroot_core_fs_async_create_directory_v1(
          executor, &path, &Impl::completed, next.get(), &next->request);
    } else if (next->job.operation ==
               DirectoryResult::Operation::InspectSymlink) {
      submitted = plainroot_core_fs_async_stat_v1(
          executor, &path, PLAINROOT_CORE_FS_STAT_FOLLOW_SYMLINKS,
          &Impl::completed, next.get(), &next->request);
    } else {
      submitted = plainroot_core_fs_async_list_directory_v1(
          executor, &path, &Impl::completed, next.get(), &next->request);
    }
    if (submitted == PLAINROOT_CORE_FS_OK) {
      pending->push_back(std::move(next));
    } else {
      DirectoryResult result{};
      result.operation = next->job.operation;
      result.generation = next->job.generation;
      result.request_id = next->job.request_id;
      result.path = next->job.path;
      result.result = submitted;
      publish(std::move(result));
    }
  }

  static void
  release_completed(plainroot_core_fs_async_executor *executor,
                    std::vector<std::unique_ptr<Pending>> *pending) {
    size_t dispatched = 0;
    (void)plainroot_core_fs_async_executor_dispatch_v1(executor, 0,
                                                       &dispatched);
    auto &requests = *pending;
    for (auto iterator = requests.begin(); iterator != requests.end();) {
      if (!(*iterator)->completed) {
        ++iterator;
        continue;
      }
      plainroot_core_fs_async_request_release_v1((*iterator)->request);
      iterator = requests.erase(iterator);
    }
  }

  void run() {
    try {
      mime_database_ = std::make_unique<MimeDatabase>();
    } catch (...) {
      mime_database_.reset();
    }

    plainroot_core_fs_async_executor_options_v1 options{};
    options.struct_size = sizeof(options);
    options.worker_count = 1;
    options.maximum_queued_requests = 8;
    options.maximum_pending_requests = 8;
    plainroot_core_fs_async_executor *executor = nullptr;
    const auto created = plainroot_core_fs_async_executor_create_v1(
        &options, nullptr, nullptr, &executor, nullptr);
    if (created != PLAINROOT_CORE_FS_OK || executor == nullptr) {
      std::deque<Job> failed_jobs;
      {
        std::lock_guard lock(mutex_);
        startup_failed_.store(true, std::memory_order_release);
        failed_jobs.swap(jobs_);
      }
      for (const Job &job : failed_jobs) {
        DirectoryResult result{};
        result.operation = job.operation;
        result.generation = job.generation;
        result.request_id = job.request_id;
        result.path = job.path;
        result.result = PLAINROOT_CORE_FS_IO_ERROR;
        publish(std::move(result));
      }
      return;
    }

    std::vector<std::unique_ptr<Pending>> pending;
    const int executor_fd =
        plainroot_core_fs_async_executor_notification_fd_v1(executor);
    bool stopping = false;
    while (!stopping) {
      pollfd descriptors[2] = {{notification_fd_, POLLIN, 0},
                               {executor_fd, POLLIN, 0}};
      int poll_result;
      do {
        poll_result = poll(descriptors, 2, -1);
      } while (poll_result < 0 && errno == EINTR);
      if (poll_result < 0)
        break;

      if ((descriptors[0].revents & POLLIN) != 0) {
        drain_notifications();
        auto jobs = take_jobs(&stopping);
        if (stopping) {
          for (Job &job : jobs) {
            if (job.operation == DirectoryResult::Operation::RecordRecent)
              submit_job(executor, std::move(job), &pending);
          }
          break;
        }
        for (Job &job : jobs) {
          if (job.operation == DirectoryResult::Operation::Listing) {
            for (const auto &existing : pending) {
              if (existing->job.operation ==
                  DirectoryResult::Operation::Listing)
                (void)plainroot_core_fs_async_request_cancel_v1(
                    existing->request);
            }
          }
          submit_job(executor, std::move(job), &pending);
        }
      }

      if ((descriptors[1].revents & POLLIN) != 0)
        release_completed(executor, &pending);
    }

    for (const auto &request : pending)
      (void)plainroot_core_fs_async_request_cancel_v1(request->request);
    (void)plainroot_core_fs_async_executor_shutdown_v1(executor);
    while (!pending.empty()) {
      release_completed(executor, &pending);
      if (pending.empty())
        break;
      pollfd descriptor{executor_fd, POLLIN, 0};
      (void)poll(&descriptor, 1, 50);
    }
    (void)plainroot_core_fs_async_executor_release_v1(executor);
  }

  plainroot_toolkit_application *application_ = nullptr;
  ResultHandler result_handler_;
  int notification_fd_ = -1;
  std::thread worker_;
  std::mutex mutex_;
  std::deque<Job> jobs_;
  std::unique_ptr<MimeDatabase> mime_database_;
  std::atomic<uint64_t> latest_generation_{0};
  bool stopping_ = false;
  std::atomic<bool> startup_failed_{false};
};

DirectoryLoader::DirectoryLoader(plainroot_toolkit_application *application,
                                 ResultHandler result_handler)
    : impl_(std::make_unique<Impl>(application, std::move(result_handler))) {}

DirectoryLoader::~DirectoryLoader() = default;

bool DirectoryLoader::ready() const { return impl_ && impl_->ready(); }

void DirectoryLoader::request(std::string path, uint64_t generation,
                              bool classify_mime) {
  if (impl_)
    impl_->request(std::move(path), generation, classify_mime);
}

void DirectoryLoader::create_directory(std::string path, uint64_t generation,
                                       uint64_t request_id) {
  if (impl_)
    impl_->create_directory(std::move(path), generation, request_id);
}

void DirectoryLoader::inspect_symlink(std::string path,
                                      uint64_t generation) {
  if (impl_)
    impl_->inspect_symlink(std::move(path), generation);
}

void DirectoryLoader::request_recent(std::string path, uint64_t generation,
                                     bool classify_mime) {
  if (impl_)
    impl_->request_recent(std::move(path), generation, classify_mime);
}

void DirectoryLoader::record_recent(std::vector<std::string> paths,
                                    std::string xbel_path,
                                    std::string application_id) {
  if (impl_)
    impl_->record_recent(std::move(paths), std::move(xbel_path),
                         std::move(application_id));
}

void DirectoryLoader::stop() {
  if (impl_)
    impl_->stop();
}

} // namespace plainroot::filechooser::detail
