#include "portal_backend.hpp"

#include "browser_controller.hpp"
#include "portal_choices.hpp"
#include "portal_limits.hpp"
#include "portal_selection_mode.hpp"
#include "portal_response.hpp"
#include "save_files.hpp"
#include "trace.hpp"
#include "ui_context_factory.hpp"

#include <plainroot/i18n/i18n.h>
#include <plainroot/toolkit/toolkit.h>

#include <glib-unix.h>
#include <glib.h>
#include <systemd/sd-bus.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <new>
#include <poll.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plainroot::filechooser {
namespace {

constexpr char kDomain[] = "plainroot-filechooser";
constexpr char kBackendName[] =
    "org.freedesktop.impl.portal.desktop.plainroot";
constexpr char kDesktopPath[] = "/org/freedesktop/portal/desktop";
constexpr char kChooserInterface[] = "org.freedesktop.impl.portal.FileChooser";
constexpr char kRequestInterface[] = "org.freedesktop.impl.portal.Request";
constexpr char kRequestPrefix[] = "/org/freedesktop/portal/desktop/request/";
constexpr uint32_t kResponseSuccess = 0;
constexpr uint32_t kResponseCancelled = 1;
constexpr uint32_t kResponseError = 2;
constexpr size_t kMaximumFilters = 64U;
constexpr size_t kMaximumPatternsPerFilter = 256U;
constexpr size_t kMaximumPatternLength = 4096U;

class PortalBackend;

struct PortalRequest {
  PortalBackend *backend = nullptr;
  sd_bus_message *call = nullptr;
  sd_bus_slot *request_slot = nullptr;
  std::string handle;
  std::string app_id;
  std::string parent_window;
  std::string title;
  std::string accept_label;
  std::string start_path;
  std::string current_name;
  std::string current_file_path;
  std::vector<std::string> save_filenames;
  std::vector<BrowserFilter> filters;
  std::vector<BrowserChoice> choices;
  BrowserFilter current_filter;
  int32_t selected_filter_index = -1;
  bool has_current_filter = false;
  bool multiple = false;
  bool directory = false;
  bool modal = true;
  bool save_file = false;
  bool save_files = false;
  bool has_save_filenames = false;
  bool cancel_requested = false;
  bool finished = false;
  plainroot_ui_context *context = nullptr;
  std::unique_ptr<BrowserController> controller;
  plainroot_toolkit_window *window = nullptr;
};

struct PortalBusSource {
  GSource source;
  PortalBackend *backend;
  gpointer fd_tag;
  GIOCondition poll_events;
  gint64 deadline_usec;
};

int open_file_method(sd_bus_message *message, void *user_data,
                     sd_bus_error *ret_error);
int save_file_method(sd_bus_message *message, void *user_data,
                     sd_bus_error *ret_error);
int save_files_method(sd_bus_message *message, void *user_data,
                      sd_bus_error *ret_error);
int close_request_method(sd_bus_message *message, void *user_data,
                         sd_bus_error *ret_error);

const sd_bus_vtable kChooserVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("OpenFile", "osssa{sv}", "ua{sv}", open_file_method,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SaveFile", "osssa{sv}", "ua{sv}", save_file_method,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SaveFiles", "osssa{sv}", "ua{sv}", save_files_method,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

const sd_bus_vtable kRequestVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Close", "", "", close_request_method,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

bool parse_filter_patterns(sd_bus_message *message,
                           std::vector<BrowserFilterPattern> &patterns,
                           size_t &total_patterns) {
  int result = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY,
                                               "(us)");
  if (result < 0)
    return false;

  while ((result = sd_bus_message_enter_container(
              message, SD_BUS_TYPE_STRUCT, "us")) > 0) {
    uint32_t type = 0;
    const char *pattern = nullptr;
    result = sd_bus_message_read(message, "us", &type, &pattern);
    if (result < 0 || pattern == nullptr || (type != 0U && type != 1U) ||
        std::strlen(pattern) > kMaximumPatternLength ||
        patterns.size() >= kMaximumPatternsPerFilter ||
        !detail::another_filter_pattern_allowed(total_patterns) ||
        sd_bus_message_exit_container(message) < 0)
      return false;
    patterns.push_back({type, pattern});
    ++total_patterns;
  }
  return result >= 0 && sd_bus_message_exit_container(message) >= 0;
}

bool parse_filter(sd_bus_message *message, BrowserFilter &filter,
                  size_t &total_patterns) {
  int result = sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT,
                                               "sa(us)");
  if (result <= 0)
    return false;

  const char *name = nullptr;
  result = sd_bus_message_read(message, "s", &name);
  if (result < 0 || name == nullptr ||
      !detail::portal_text_within_limit(name) ||
      !parse_filter_patterns(message, filter.patterns, total_patterns) ||
      sd_bus_message_exit_container(message) < 0)
    return false;
  filter.name = name;
  return true;
}

bool parse_filter_list(sd_bus_message *message,
                       std::vector<BrowserFilter> &filters,
                       size_t &total_patterns) {
  int result = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY,
                                               "(sa(us))");
  if (result < 0)
    return false;

  while (true) {
    BrowserFilter filter;
    char type = 0;
    const char *contents = nullptr;
    result = sd_bus_message_peek_type(message, &type, &contents);
    if (result == 0)
      break;
    if (result < 0 || type != SD_BUS_TYPE_STRUCT || contents == nullptr ||
        std::strcmp(contents, "sa(us)") != 0 ||
        filters.size() >= kMaximumFilters ||
        !parse_filter(message, filter, total_patterns))
      return false;
    filters.push_back(std::move(filter));
  }
  return sd_bus_message_exit_container(message) >= 0;
}

bool parse_current_filter(sd_bus_message *message, BrowserFilter &filter,
                          size_t &total_patterns) {
  return parse_filter(message, filter, total_patterns);
}

bool filters_equal(const BrowserFilter &left, const BrowserFilter &right) {
  if (left.name != right.name || left.patterns.size() != right.patterns.size())
    return false;
  for (size_t index = 0; index < left.patterns.size(); ++index) {
    if (left.patterns[index].type != right.patterns[index].type ||
        left.patterns[index].pattern != right.patterns[index].pattern)
      return false;
  }
  return true;
}

bool select_initial_filter(PortalRequest &request) {
  if (!request.has_current_filter) {
    request.selected_filter_index = request.filters.empty() ? -1 : 0;
    return true;
  }
  for (size_t index = 0; index < request.filters.size(); ++index) {
    if (filters_equal(request.filters[index], request.current_filter)) {
      request.selected_filter_index = static_cast<int32_t>(index);
      return true;
    }
  }
  if (request.filters.size() >= kMaximumFilters)
    return false;
  request.filters.push_back(request.current_filter);
  request.selected_filter_index =
      static_cast<int32_t>(request.filters.size() - 1U);
  return true;
}

std::string native_path_to_uri(const std::string &path) {
  if (path.empty() || path.front() != '/')
    return {};
  const auto *bytes = reinterpret_cast<const guint8 *>(path.data());
  gchar *escaped = g_uri_escape_bytes(bytes, path.size(), "/");
  if (escaped == nullptr)
    return {};
  std::string uri = "file://";
  uri += escaped;
  g_free(escaped);
  return uri;
}

bool parse_options(sd_bus_message *message, PortalRequest &request) {
  size_t total_filter_patterns = 0U;
  int result = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY,
                                               "{sv}");
  if (result < 0)
    return false;

  while ((result = sd_bus_message_enter_container(
              message, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
    const char *key = nullptr;
    result = sd_bus_message_read(message, "s", &key);
    if (result < 0 || key == nullptr)
      return false;

    char type = 0;
    const char *contents = nullptr;
    result = sd_bus_message_peek_type(message, &type, &contents);
    if (result <= 0 || type != SD_BUS_TYPE_VARIANT || contents == nullptr ||
        sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT,
                                       contents) < 0)
      return false;

    if (std::strcmp(key, "modal") == 0 &&
        std::strcmp(contents, "b") == 0) {
      int value = 1;
      result = sd_bus_message_read(message, "b", &value);
      request.modal = value != 0;
    } else if (!request.save_files && std::strcmp(key, "multiple") == 0 &&
        std::strcmp(contents, "b") == 0) {
      int value = 0;
      result = sd_bus_message_read(message, "b", &value);
      request.multiple = value != 0;
    } else if (!request.save_files && std::strcmp(key, "directory") == 0 &&
               std::strcmp(contents, "b") == 0) {
      int value = 0;
      result = sd_bus_message_read(message, "b", &value);
      request.directory = value != 0;
    } else if (std::strcmp(key, "accept_label") == 0 &&
               std::strcmp(contents, "s") == 0) {
      const char *value = nullptr;
      result = sd_bus_message_read(message, "s", &value);
      if (result >= 0 && value != nullptr &&
          detail::portal_text_within_limit(value))
        request.accept_label = value;
      else if (result >= 0)
        result = -EINVAL;
    } else if (std::strcmp(key, "current_name") == 0 &&
               std::strcmp(contents, "s") == 0) {
      const char *value = nullptr;
      result = sd_bus_message_read(message, "s", &value);
      if (result >= 0 && value != nullptr &&
          detail::portal_text_within_limit(value))
        request.current_name = value;
      else if (result >= 0)
        result = -EINVAL;
    } else if (std::strcmp(key, "current_folder") == 0 &&
               std::strcmp(contents, "ay") == 0) {
      const void *bytes = nullptr;
      size_t size = 0;
      result = sd_bus_message_read_array(message, SD_BUS_TYPE_BYTE, &bytes,
                                         &size);
      if (result >= 0 && size > 0U &&
          !detail::native_path_payload_valid(bytes, size))
        return false;
      if (result >= 0 && size > 0U) {
        const auto *path = static_cast<const char *>(bytes);
        request.start_path.assign(path, size - 1U);
      }
    } else if (std::strcmp(key, "current_file") == 0 &&
               std::strcmp(contents, "ay") == 0) {
      const void *bytes = nullptr;
      size_t size = 0;
      result = sd_bus_message_read_array(message, SD_BUS_TYPE_BYTE, &bytes,
                                         &size);
      if (result >= 0 && size > 0U &&
          !detail::native_path_payload_valid(bytes, size))
        return false;
      if (result >= 0 && size > 0U) {
        const auto *path = static_cast<const char *>(bytes);
        request.current_file_path.assign(path, size - 1U);
      }
    } else if (!request.save_files && std::strcmp(key, "filters") == 0 &&
               std::strcmp(contents, "a(sa(us))") == 0) {
      result = parse_filter_list(message, request.filters,
                                 total_filter_patterns)
                   ? 1
                   : -EINVAL;
    } else if (!request.save_files && std::strcmp(key, "current_filter") == 0 &&
               std::strcmp(contents, "(sa(us))") == 0) {
      request.has_current_filter =
          parse_current_filter(message, request.current_filter,
                               total_filter_patterns);
      result = request.has_current_filter ? 1 : -EINVAL;
    } else if (std::strcmp(key, "choices") == 0 &&
               std::strcmp(contents, "a(ssa(ss)s)") == 0) {
      result = parse_portal_choices(message, request.choices) ? 1 : -EINVAL;
    } else if (request.save_files && std::strcmp(key, "files") == 0 &&
               std::strcmp(contents, "aay") == 0) {
      if (request.has_save_filenames) {
        result = -EINVAL;
      } else {
        const bool parsed = detail::parse_portal_save_files(
            message, request.save_filenames);
        request.has_save_filenames = parsed;
        result = parsed ? 1 : -EINVAL;
      }
    } else {
      result = sd_bus_message_skip(message, contents);
    }
    if (result < 0 || sd_bus_message_exit_container(message) < 0 ||
        sd_bus_message_exit_container(message) < 0)
      return false;
  }
  return result >= 0 && sd_bus_message_exit_container(message) >= 0;
}

gboolean bus_source_prepare(GSource *source, gint *timeout_milliseconds);
gboolean bus_source_check(GSource *source);
gboolean bus_source_dispatch(GSource *source, GSourceFunc callback,
                             gpointer user_data);

GSourceFuncs kBusSourceFunctions = {
    .prepare = bus_source_prepare,
    .check = bus_source_check,
    .dispatch = bus_source_dispatch,
    .finalize = nullptr,
    .closure_callback = nullptr,
    .closure_marshal = nullptr,
};

class PortalBackend {
public:
  PortalBackend() = default;
  PortalBackend(const PortalBackend &) = delete;
  PortalBackend &operator=(const PortalBackend &) = delete;
  ~PortalBackend() { shutdown(); }

  int run() {
    std::array<char, 512> error{};
    plainroot_i18n_options_v1 i18n_options{};
    i18n_options.struct_size = sizeof(i18n_options);
    i18n_options.fallback_language_tag = "en";
    if (plainroot_i18n_initialize_v1(&i18n_options, error.data(),
                                     error.size()) !=
            PLAINROOT_I18N_RESULT_OK ||
        plainroot_i18n_bind_domain_v1(kDomain, nullptr, error.data(),
                                      error.size()) !=
            PLAINROOT_I18N_RESULT_OK) {
      std::fprintf(stderr, "plainroot-filechooser-portal: %s\n",
                   error[0] == '\0' ? "unable to initialize localization"
                                    : error.data());
      return 1;
    }

    plainroot_toolkit_application_options_v1 app_options{};
    app_options.struct_size = sizeof(app_options);
    application_ = plainroot_toolkit_application_create_v1(
        &app_options, error.data(), error.size());
    if (application_ == nullptr) {
      std::fprintf(stderr, "plainroot-filechooser-portal: %s\n",
                   error.data());
      return 1;
    }

    int result = sd_bus_open_user(&bus_);
    if (result < 0) {
      report_bus_error("unable to open the session bus", result);
      return 1;
    }
    result = sd_bus_request_name(bus_, kBackendName, 0);
    if (result < 0) {
      report_bus_error("unable to acquire the portal backend name", result);
      return 1;
    }
    result = sd_bus_add_object_vtable(bus_, &chooser_slot_, kDesktopPath,
                                      kChooserInterface, kChooserVtable, this);
    if (result < 0) {
      report_bus_error("unable to export the FileChooser interface", result);
      return 1;
    }
    if (!attach_bus_source()) {
      std::fputs("plainroot-filechooser-portal: unable to monitor the session bus\n",
                 stderr);
      return 1;
    }
    detail::trace("backend-ready");

    while (!stopping_) {
      result = process_bus();
      if (result < 0) {
        report_bus_error("session bus dispatch failed", result);
        return 1;
      }
      create_pending_requests();
      discard_finished_requests();
      if (has_open_windows()) {
        app_running_ = true;
        detail::trace("toolkit-loop-begin");
        result = plainroot_toolkit_application_run_v1(
            application_, error.data(), error.size());
        app_running_ = false;
        detail::trace("toolkit-loop-end result=%d", result);
        if (result != PLAINROOT_TOOLKIT_RESULT_OK) {
          std::fprintf(stderr, "plainroot-filechooser-portal: %s\n",
                       error[0] == '\0' ? "chooser window failed"
                                        : error.data());
          for (const auto &request : requests_)
            if (!request->finished)
              finish(*request, kResponseError, {}, false);
        }
        discard_closed_requests();
        continue;
      }
      result = sd_bus_wait(bus_, UINT64_MAX);
      if (result < 0 && result != -EINTR) {
        report_bus_error("session bus wait failed", result);
        return 1;
      }
    }
    return 0;
  }

  int open_file(sd_bus_message *message) {
    return begin_file_request(message, false, false);
  }

  int save_file(sd_bus_message *message) {
    return begin_file_request(message, true, false);
  }

  int save_files(sd_bus_message *message) {
    return begin_file_request(message, false, true);
  }

  int begin_file_request(sd_bus_message *message, bool save_file_request,
                         bool save_files_request) {
    detail::trace("request-received kind=%s",
                  save_files_request
                      ? "save-files"
                      : (save_file_request ? "save-file" : "open-file"));
    const char *handle = nullptr;
    const char *app_id = nullptr;
    const char *parent_window = nullptr;
    const char *title = nullptr;
    int result = sd_bus_message_read(message, "osss", &handle, &app_id,
                                     &parent_window, &title);
    if (result < 0 || handle == nullptr || app_id == nullptr ||
        parent_window == nullptr || title == nullptr ||
        !detail::portal_text_within_limit(handle) ||
        !detail::portal_text_within_limit(app_id) ||
        !detail::portal_text_within_limit(parent_window) ||
        !detail::portal_text_within_limit(title))
      return reply_error(
          message, SD_BUS_ERROR_INVALID_ARGS,
          save_files_request
              ? "Malformed FileChooser.SaveFiles request"
              : (save_file_request ? "Malformed FileChooser.SaveFile request"
                                   : "Malformed FileChooser.OpenFile request"));
    if (!sd_bus_object_path_is_valid(handle) ||
        std::string_view(handle).rfind(kRequestPrefix, 0) != 0)
      return reply_error(message, SD_BUS_ERROR_INVALID_ARGS,
                         "Invalid portal request handle");

    std::unique_ptr<PortalRequest> request;
    try {
      request = std::make_unique<PortalRequest>();
      request->backend = this;
      request->handle = handle;
      request->app_id = app_id;
      request->parent_window = parent_window;
      request->title = title;
      request->save_file = save_file_request;
      request->save_files = save_files_request;
    } catch (const std::bad_alloc &) {
      return reply_error(message, SD_BUS_ERROR_NO_MEMORY,
                         "Unable to allocate chooser request");
    }
    if (!parse_options(message, *request))
      return reply_error(
          message, SD_BUS_ERROR_INVALID_ARGS,
          save_files_request
              ? "Malformed FileChooser.SaveFiles options"
              : (save_file_request ? "Malformed FileChooser.SaveFile options"
                                   : "Malformed FileChooser.OpenFile options"));
    detail::trace("request-parent-modal parent-present=%d modal=%d",
                  request->parent_window.empty() ? 0 : 1,
                  request->modal ? 1 : 0);
    if (!select_initial_filter(*request))
      return reply_error(message, SD_BUS_ERROR_INVALID_ARGS,
                         "Too many file filters in request");
    if (!detail::portal_selection_mode_valid(
            save_file_request, request->multiple, request->directory))
      return reply_error(message, SD_BUS_ERROR_INVALID_ARGS,
                         request->multiple
                             ? "Multiple selection cannot be combined with SaveFile"
                             : "SaveFile does not support directory selection");
    if (save_files_request && !request->has_save_filenames)
      return reply_error(message, SD_BUS_ERROR_INVALID_ARGS,
                         "SaveFiles requires at least one valid file name");
    if (save_files_request)
      request->directory = true;
    if (save_file_request && !request->current_file_path.empty()) {
      gchar *folder = g_path_get_dirname(request->current_file_path.c_str());
      gchar *name = g_path_get_basename(request->current_file_path.c_str());
      if (folder == nullptr || name == nullptr) {
        g_free(folder);
        g_free(name);
        return reply_error(message, SD_BUS_ERROR_NO_MEMORY,
                           "Unable to read the suggested file path");
      }
      request->start_path = folder;
      if (request->current_name.empty() && g_utf8_validate(name, -1, nullptr))
        request->current_name = name;
      g_free(folder);
      g_free(name);
    }
    if (request->title.empty()) {
      const char *fallback = plainroot_i18n_gettext_v1(
          kDomain, save_files_request
                       ? "Save files"
                       : (save_file_request
                              ? "Save file"
                              : (request->directory
                                     ? "Choose a folder"
                                     : (request->multiple ? "Choose files"
                                                          : "Choose a file"))));
      const char *english_fallback =
          save_files_request
              ? "Save files"
              : (save_file_request
                     ? "Save file"
                     : (request->directory
                            ? "Choose a folder"
                            : (request->multiple ? "Choose files"
                                                 : "Choose a file")));
      request->title = fallback == nullptr ? english_fallback : fallback;
    }
    if (request->start_path.empty()) {
      const char *home = g_get_home_dir();
      request->start_path = home != nullptr && home[0] == '/' ? home : "/";
    }
    for (const auto &existing : requests_)
      if (existing->handle == request->handle)
        return reply_error(message, SD_BUS_ERROR_FAILED,
                           "A chooser request already uses this handle");

    request->call = sd_bus_message_ref(message);
    PortalRequest *raw = request.get();
    result = sd_bus_add_object_vtable(bus_, &raw->request_slot,
                                      raw->handle.c_str(), kRequestInterface,
                                      kRequestVtable, raw);
    if (result < 0) {
      sd_bus_message_unref(request->call);
      request->call = nullptr;
      return reply_error(message, SD_BUS_ERROR_FAILED,
                         "Unable to register the portal request handle");
    }
    requests_.push_back(std::move(request));
    detail::trace("request-queued kind=%s",
                  save_files_request
                      ? "save-files"
                      : (save_file_request ? "save-file" : "open-file"));
    if (app_running_)
      schedule_pending_requests();
    return 1;
  }

  void request_close(PortalRequest &request) {
    detail::trace("request-close");
    request.cancel_requested = true;
    if (app_running_)
      schedule_pending_requests();
  }

  void finish(PortalRequest &request, uint32_t response,
              const std::vector<std::string> &native_paths,
              bool close_window,
              const BrowserFilter *current_filter = nullptr,
              const std::vector<BrowserChoiceSelection> *choices = nullptr,
              bool writable = true) {
    if (request.finished)
      return;
    detail::trace("finish-begin response=%u", response);
    const auto request_window_close = [&] {
      if (!close_window || request.window == nullptr)
        return;
      const auto result =
          plainroot_toolkit_window_request_close(request.window);
      detail::trace("window-close-request result=%u",
                    static_cast<unsigned>(result));
    };
    std::vector<std::string> save_paths;
    const std::vector<std::string> *result_paths = &native_paths;
    if (response == kResponseSuccess && request.save_files) {
      if (native_paths.size() != 1U) {
        response = kResponseError;
      } else {
        try {
          auto resolved = detail::resolve_save_files(native_paths.front(),
                                                     request.save_filenames);
          if (resolved.status != detail::SaveFilesStatus::success) {
            response = kResponseError;
          } else {
            save_paths = std::move(resolved.paths);
            result_paths = &save_paths;
          }
        } catch (const std::bad_alloc &) {
          response = kResponseError;
        }
      }
    }
    if (response == kResponseSuccess) {
      if (result_paths->empty()) {
        response = kResponseError;
      }
      std::vector<std::string> uris;
      try {
        if (response == kResponseSuccess) {
          uris.reserve(result_paths->size());
          for (const std::string &native_path : *result_paths) {
            std::string uri = native_path_to_uri(native_path);
            if (uri.empty()) {
              response = kResponseError;
              break;
            }
            uris.push_back(std::move(uri));
          }
        }
      } catch (const std::bad_alloc &) {
        response = kResponseError;
      }
      if (response == kResponseSuccess &&
          uris.size() == result_paths->size() && !uris.empty()) {
        send_reply(request, response, uris, current_filter, choices, writable);
        if (request.controller != nullptr)
          request.controller->record_recent_paths(
              *result_paths, request.save_file || request.save_files);
        request.finished = true;
        unregister_request(request);
        request_window_close();
        return;
      }
    }
    send_reply(request, response, {}, nullptr, nullptr);
    request.finished = true;
    unregister_request(request);
    request_window_close();
  }

  void bus_failed() {
    stopping_ = true;
    if (app_running_)
      plainroot_toolkit_application_quit(application_);
  }

  sd_bus *bus() const { return bus_; }

private:
  static int reply_error(sd_bus_message *message, const char *name,
                         const char *description) {
    return sd_bus_reply_method_errorf(message, name, "%s", description);
  }

  int process_bus() {
    int result = 0;
    do {
      result = sd_bus_process(bus_, nullptr);
    } while (result > 0 || result == -EINTR);
    return result;
  }

  bool attach_bus_source() {
    const int fd = sd_bus_get_fd(bus_);
    if (fd < 0)
      return false;
    auto *source = reinterpret_cast<PortalBusSource *>(
        g_source_new(&kBusSourceFunctions, sizeof(PortalBusSource)));
    if (source == nullptr)
      return false;
    source->backend = this;
    source->poll_events =
        static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL);
    source->deadline_usec = -1;
    source->fd_tag = g_source_add_unix_fd(reinterpret_cast<GSource *>(source),
                                          fd, source->poll_events);
    g_source_set_name(reinterpret_cast<GSource *>(source),
                      "plainroot-filechooser-portal-sd-bus");
    bus_source_ = reinterpret_cast<GSource *>(source);
    return g_source_attach(bus_source_, g_main_context_default()) != 0U;
  }

  void schedule_pending_requests() {
    if (pending_idle_scheduled_)
      return;
    pending_idle_scheduled_ = true;
    g_idle_add_full(G_PRIORITY_DEFAULT, dispatch_pending_idle, this, nullptr);
  }

  static gboolean dispatch_pending_idle(gpointer user_data) {
    auto *self = static_cast<PortalBackend *>(user_data);
    self->pending_idle_scheduled_ = false;
    self->create_pending_requests();
    return G_SOURCE_REMOVE;
  }

  void create_pending_requests() {
    for (const auto &owned : requests_) {
      PortalRequest &request = *owned;
      if (request.finished)
        continue;
      if (request.cancel_requested) {
        if (request.controller != nullptr)
          request.controller->cancel();
        else
          finish(request, kResponseCancelled, {}, false);
        if (!request.finished)
          finish(request, kResponseCancelled, {}, true);
        continue;
      }
      if (request.window != nullptr)
        continue;
      create_window(request);
    }
  }

  void create_window(PortalRequest &request) {
    detail::trace("window-create-begin parent-present=%d modal=%d",
                  request.parent_window.empty() ? 0 : 1,
                  request.modal ? 1 : 0);
    const char *language = plainroot_i18n_language_tag_v1();
    if (language == nullptr)
      language = "en";
    request.context = make_ui_context(language);
    if (request.context == nullptr) {
      finish(request, kResponseError, {}, false);
      return;
    }
    request.controller = std::make_unique<BrowserController>(
        application_, request.context, browser_completed, &request);
    request.controller->set_title(request.title);
    request.controller->set_recent_application_id(request.app_id);
    request.controller->set_select_directory(request.directory);
    request.controller->set_allow_multiple(request.multiple);
    request.controller->set_allow_read_only(
        !request.save_file && !request.save_files && !request.directory);
    request.controller->set_filters(request.filters,
                                    request.selected_filter_index);
    request.controller->set_choices(request.choices);
    request.controller->set_save_file(request.save_file);
    request.controller->set_save_files(request.save_filenames);
    request.controller->set_initial_filename(request.current_name);
    request.controller->set_accept_label(request.accept_label);
    if (!request.controller->ready() || !request.controller->create_ui()) {
      finish(request, kResponseError, {}, false);
      return;
    }

    plainroot_toolkit_window_options_v1 options{};
    options.struct_size = sizeof(options);
    options.title = request.title.empty()
                        ? (request.save_files
                               ? "Save files"
                               : (request.save_file
                                      ? "Save file"
                                      : (request.directory
                                             ? "Choose a folder"
                                             : (request.multiple
                                                    ? "Choose files"
                                                    : "Choose a file"))))
                        : request.title.c_str();
    options.app_id = "org.plainroot.FileChooser";
    options.initial_width = 1000;
    options.initial_height = 720;
    options.minimum_width = 320;
    options.minimum_height = 320;
    options.maximum_width = 1600;
    options.maximum_height = 1200;
    options.content_max_width = 1280.0F;
    options.parent_window_identifier = request.parent_window.c_str();
    options.modal = request.modal ? 1U : 0U;
    options.ui_context = request.context;
    options.clear_color[0] = 0.055F;
    options.clear_color[1] = 0.065F;
    options.clear_color[2] = 0.085F;
    options.clear_color[3] = 1.0F;
    std::array<char, 512> error{};
    const auto result = plainroot_toolkit_window_create_v1(
        application_, &options, window_close_requested, &request,
        &request.window, error.data(), error.size());
    if (result != PLAINROOT_TOOLKIT_RESULT_OK) {
      std::fprintf(stderr, "plainroot-filechooser-portal: %s\n",
                   error[0] == '\0' ? "unable to create chooser window"
                                    : error.data());
      finish(request, kResponseError, {}, false);
      return;
    }
    request.controller->set_window(request.window);
    request.controller->navigate(request.start_path);
    detail::trace("window-create-end");
  }

  static void browser_completed(
      void *user_data, bool accepted, bool writable,
      const std::vector<std::string> &native_paths,
      const BrowserFilter *current_filter,
      const std::vector<BrowserChoiceSelection> &choices) {
    detail::trace("browser-completed accepted=%u paths=%zu choices=%zu",
                  accepted ? 1U : 0U, native_paths.size(), choices.size());
    auto &request = *static_cast<PortalRequest *>(user_data);
    request.backend->finish(request,
                            accepted ? kResponseSuccess : kResponseCancelled,
                            native_paths, true,
                            accepted && !request.save_files ? current_filter
                                                            : nullptr,
                            accepted ? &choices : nullptr, writable);
  }

  static int window_close_requested(void *user_data,
                                    plainroot_toolkit_window *) {
    detail::trace("window-close-requested");
    auto &request = *static_cast<PortalRequest *>(user_data);
    request.backend->finish(request, kResponseCancelled, {}, false);
    return 1;
  }

  void send_reply(PortalRequest &request, uint32_t response,
                  const std::vector<std::string> &uris,
                  const BrowserFilter *current_filter,
                  const std::vector<BrowserChoiceSelection> *choices,
                  bool writable = true) {
    if (request.call == nullptr)
      return;
    const int result = send_portal_response(
        bus_, request.call, response, uris, current_filter, choices,
        writable);
    detail::trace("response-sent response=%u uris=%zu result=%d", response,
                  uris.size(), result);
    if (result < 0)
      report_bus_error("unable to send chooser response", result);
    sd_bus_message_unref(request.call);
    request.call = nullptr;
  }

  void unregister_request(PortalRequest &request) {
    request.request_slot = sd_bus_slot_unref(request.request_slot);
  }

  bool has_open_windows() const {
    return std::any_of(requests_.begin(), requests_.end(),
                       [](const auto &request) {
                         return request->window != nullptr &&
                                !request->finished;
                       });
  }

  void discard_finished_requests() {
    for (auto iterator = requests_.begin(); iterator != requests_.end();) {
      if (!(*iterator)->finished) {
        ++iterator;
        continue;
      }
      release_request(**iterator);
      iterator = requests_.erase(iterator);
    }
  }

  void discard_closed_requests() {
    for (const auto &request : requests_) {
      if (request->window != nullptr && !request->finished) {
        plainroot_toolkit_window_state_v1 state{};
        state.struct_size = sizeof(state);
        if (plainroot_toolkit_window_get_state(request->window, &state) ==
                PLAINROOT_TOOLKIT_RESULT_OK &&
            (state.flags & PLAINROOT_TOOLKIT_WINDOW_CLOSED) != 0U)
          finish(*request, kResponseCancelled, {}, false);
      }
    }
    discard_finished_requests();
  }

  void release_request(PortalRequest &request) {
    if (request.controller != nullptr)
      request.controller->shutdown();
    if (request.window != nullptr) {
      (void)plainroot_toolkit_window_destroy(request.window);
      request.window = nullptr;
    }
    request.controller.reset();
    plainroot_ui_context_destroy(request.context);
    request.context = nullptr;
    unregister_request(request);
    if (request.call != nullptr) {
      sd_bus_message_unref(request.call);
      request.call = nullptr;
    }
  }

  void report_bus_error(const char *operation, int result) const {
    std::fprintf(stderr, "plainroot-filechooser-portal: %s: %s\n", operation,
                 std::strerror(-result));
  }

  void shutdown() {
    if (bus_source_ != nullptr) {
      g_source_destroy(bus_source_);
      g_source_unref(bus_source_);
      bus_source_ = nullptr;
    }
    for (const auto &request : requests_)
      release_request(*request);
    requests_.clear();
    chooser_slot_ = sd_bus_slot_unref(chooser_slot_);
    if (bus_ != nullptr)
      bus_ = sd_bus_flush_close_unref(bus_);
    if (application_ != nullptr) {
      plainroot_toolkit_application_destroy(application_);
      application_ = nullptr;
    }
  }

  sd_bus *bus_ = nullptr;
  sd_bus_slot *chooser_slot_ = nullptr;
  GSource *bus_source_ = nullptr;
  plainroot_toolkit_application *application_ = nullptr;
  std::list<std::unique_ptr<PortalRequest>> requests_;
  bool app_running_ = false;
  bool stopping_ = false;
  bool pending_idle_scheduled_ = false;
};

int open_file_method(sd_bus_message *message, void *user_data, sd_bus_error *) {
  return static_cast<PortalBackend *>(user_data)->open_file(message);
}

int save_file_method(sd_bus_message *message, void *user_data, sd_bus_error *) {
  return static_cast<PortalBackend *>(user_data)->save_file(message);
}

int save_files_method(sd_bus_message *message, void *user_data, sd_bus_error *) {
  return static_cast<PortalBackend *>(user_data)->save_files(message);
}

int close_request_method(sd_bus_message *message, void *user_data,
                         sd_bus_error *) {
  auto &request = *static_cast<PortalRequest *>(user_data);
  const int result = sd_bus_reply_method_return(message, "");
  if (result < 0)
    return result;
  request.backend->request_close(request);
  return 1;
}

gboolean bus_source_prepare(GSource *source, gint *timeout_milliseconds) {
  auto *watch = reinterpret_cast<PortalBusSource *>(source);
  const int events = sd_bus_get_events(watch->backend->bus());
  uint64_t deadline = UINT64_MAX;
  const int timeout_result =
      events < 0 ? events : sd_bus_get_timeout(watch->backend->bus(), &deadline);
  if (events < 0 || timeout_result < 0) {
    *timeout_milliseconds = 0;
    return TRUE;
  }
  GIOCondition conditions = static_cast<GIOCondition>(
      G_IO_ERR | G_IO_HUP | G_IO_NVAL);
  if ((events & POLLIN) != 0)
    conditions = static_cast<GIOCondition>(conditions | G_IO_IN);
  if ((events & POLLOUT) != 0)
    conditions = static_cast<GIOCondition>(conditions | G_IO_OUT);
  if (conditions != watch->poll_events) {
    g_source_modify_unix_fd(source, watch->fd_tag, conditions);
    watch->poll_events = conditions;
  }
  watch->deadline_usec = -1;
  if (deadline == UINT64_MAX) {
    *timeout_milliseconds = -1;
    return FALSE;
  }
  const gint64 now = g_get_monotonic_time();
  watch->deadline_usec = deadline > static_cast<uint64_t>(G_MAXINT64)
                             ? G_MAXINT64
                             : static_cast<gint64>(deadline);
  if (watch->deadline_usec <= now) {
    *timeout_milliseconds = 0;
    return TRUE;
  }
  const uint64_t remaining =
      static_cast<uint64_t>(watch->deadline_usec - now);
  const uint64_t milliseconds = (remaining + 999U) / 1000U;
  *timeout_milliseconds = milliseconds > static_cast<uint64_t>(G_MAXINT)
                              ? G_MAXINT
                              : static_cast<gint>(milliseconds);
  return FALSE;
}

gboolean bus_source_check(GSource *source) {
  auto *watch = reinterpret_cast<PortalBusSource *>(source);
  const GIOCondition condition = g_source_query_unix_fd(source, watch->fd_tag);
  if ((condition & (G_IO_IN | G_IO_OUT | G_IO_PRI | G_IO_ERR | G_IO_HUP |
                    G_IO_NVAL)) != 0)
    return TRUE;
  return watch->deadline_usec >= 0 &&
         g_get_monotonic_time() >= watch->deadline_usec;
}

gboolean bus_source_dispatch(GSource *source, GSourceFunc, gpointer) {
  auto *watch = reinterpret_cast<PortalBusSource *>(source);
  PortalBackend *backend = watch->backend;
  int result = 0;
  do {
    result = sd_bus_process(backend->bus(), nullptr);
  } while (result > 0 || result == -EINTR);
  if (result < 0)
    backend->bus_failed();
  return G_SOURCE_CONTINUE;
}

} // namespace

int run_portal_backend() {
  PortalBackend backend;
  return backend.run();
}

} // namespace plainroot::filechooser
