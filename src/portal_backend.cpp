#include "portal_backend.hpp"

#include "browser_controller.hpp"
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
  bool multiple = false;
  bool directory = false;
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
  gint64 deadline_usec;
};

int open_file_method(sd_bus_message *message, void *user_data,
                     sd_bus_error *ret_error);
int unsupported_method(sd_bus_message *message, void *, sd_bus_error *);
int close_request_method(sd_bus_message *message, void *user_data,
                         sd_bus_error *ret_error);

const sd_bus_vtable kChooserVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("OpenFile", "osssa{sv}", "ua{sv}", open_file_method,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SaveFile", "osssa{sv}", "ua{sv}", unsupported_method,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SaveFiles", "osssa{sv}", "ua{sv}", unsupported_method,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

const sd_bus_vtable kRequestVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Close", "", "", close_request_method,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

bool parse_options(sd_bus_message *message, PortalRequest &request) {
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

    if (std::strcmp(key, "multiple") == 0 &&
        std::strcmp(contents, "b") == 0) {
      int value = 0;
      result = sd_bus_message_read(message, "b", &value);
      request.multiple = value != 0;
    } else if (std::strcmp(key, "directory") == 0 &&
               std::strcmp(contents, "b") == 0) {
      int value = 0;
      result = sd_bus_message_read(message, "b", &value);
      request.directory = value != 0;
    } else if (std::strcmp(key, "accept_label") == 0 &&
               std::strcmp(contents, "s") == 0) {
      const char *value = nullptr;
      result = sd_bus_message_read(message, "s", &value);
      if (result >= 0 && value != nullptr)
        request.accept_label = value;
    } else if (std::strcmp(key, "current_folder") == 0 &&
               std::strcmp(contents, "ay") == 0) {
      const void *bytes = nullptr;
      size_t size = 0;
      result = sd_bus_message_read_array(message, SD_BUS_TYPE_BYTE, &bytes,
                                         &size);
      if (result >= 0 && size > 0U && bytes != nullptr) {
        const auto *path = static_cast<const char *>(bytes);
        if (path[size - 1U] != '\0' || path[0] != '/' ||
            std::memchr(path, '\0', size - 1U) != nullptr)
          return false;
        request.start_path.assign(path, size - 1U);
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

bool append_uris(sd_bus_message *reply, const char *uri) {
  int result = sd_bus_message_open_container(reply, SD_BUS_TYPE_DICT_ENTRY,
                                              "sv");
  if (result >= 0)
    result = sd_bus_message_append(reply, "s", "uris");
  if (result >= 0)
    result = sd_bus_message_open_container(reply, SD_BUS_TYPE_VARIANT, "as");
  if (result >= 0)
    result = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "s");
  if (result >= 0)
    result = sd_bus_message_append(reply, "s", uri);
  if (result >= 0)
    result = sd_bus_message_close_container(reply);
  if (result >= 0)
    result = sd_bus_message_close_container(reply);
  if (result >= 0)
    result = sd_bus_message_close_container(reply);
  return result >= 0;
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
        result = plainroot_toolkit_application_run_v1(
            application_, error.data(), error.size());
        app_running_ = false;
        if (result != PLAINROOT_TOOLKIT_RESULT_OK) {
          std::fprintf(stderr, "plainroot-filechooser-portal: %s\n",
                       error[0] == '\0' ? "chooser window failed"
                                        : error.data());
          for (const auto &request : requests_)
            if (!request->finished)
              finish(*request, kResponseError, nullptr, false);
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
    const char *handle = nullptr;
    const char *app_id = nullptr;
    const char *parent_window = nullptr;
    const char *title = nullptr;
    int result = sd_bus_message_read(message, "osss", &handle, &app_id,
                                     &parent_window, &title);
    if (result < 0 || handle == nullptr || app_id == nullptr ||
        parent_window == nullptr || title == nullptr)
      return reply_error(message, SD_BUS_ERROR_INVALID_ARGS,
                         "Malformed FileChooser.OpenFile request");
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
    } catch (const std::bad_alloc &) {
      return reply_error(message, SD_BUS_ERROR_NO_MEMORY,
                         "Unable to allocate chooser request");
    }
    if (!parse_options(message, *request))
      return reply_error(message, SD_BUS_ERROR_INVALID_ARGS,
                         "Malformed FileChooser.OpenFile options");
    if (request->multiple)
      return reply_error(message, SD_BUS_ERROR_NOT_SUPPORTED,
                         "This backend does not support multiple-file OpenFile requests");
    if (request->title.empty()) {
      const char *fallback = plainroot_i18n_gettext_v1(
          kDomain, request->directory ? "Choose a folder" : "Choose a file");
      request->title = fallback == nullptr ? "Choose a file" : fallback;
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
    if (app_running_)
      schedule_pending_requests();
    return 1;
  }

  void request_close(PortalRequest &request) {
    request.cancel_requested = true;
    if (app_running_)
      schedule_pending_requests();
  }

  void finish(PortalRequest &request, uint32_t response,
              const char *native_path, bool close_window) {
    if (request.finished)
      return;
    if (response == kResponseSuccess) {
      GError *error = nullptr;
      gchar *uri = g_filename_to_uri(native_path, nullptr, &error);
      if (uri == nullptr) {
        if (error != nullptr)
          g_error_free(error);
        response = kResponseError;
      } else {
        send_reply(request, response, uri);
        g_free(uri);
        request.finished = true;
        unregister_request(request);
        if (close_window && request.window != nullptr)
          (void)plainroot_toolkit_window_request_close(request.window);
        return;
      }
    }
    send_reply(request, response, nullptr);
    request.finished = true;
    unregister_request(request);
    if (close_window && request.window != nullptr)
      (void)plainroot_toolkit_window_request_close(request.window);
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
    source->deadline_usec = -1;
    source->fd_tag = g_source_add_unix_fd(
        reinterpret_cast<GSource *>(source), fd,
        static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL));
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
          finish(request, kResponseCancelled, nullptr, false);
        if (!request.finished)
          finish(request, kResponseCancelled, nullptr, true);
        continue;
      }
      if (request.window != nullptr)
        continue;
      create_window(request);
    }
  }

  void create_window(PortalRequest &request) {
    const char *language = plainroot_i18n_language_tag_v1();
    if (language == nullptr)
      language = "en";
    request.context = make_ui_context(language);
    if (request.context == nullptr) {
      finish(request, kResponseError, nullptr, false);
      return;
    }
    request.controller = std::make_unique<BrowserController>(
        application_, request.context, browser_completed, &request);
    request.controller->set_title(request.title);
    request.controller->set_select_directory(request.directory);
    request.controller->set_accept_label(request.accept_label);
    if (!request.controller->ready() || !request.controller->create_ui()) {
      finish(request, kResponseError, nullptr, false);
      return;
    }

    plainroot_toolkit_window_options_v1 options{};
    options.struct_size = sizeof(options);
    options.title = request.title.empty() ? "Choose a file" : request.title.c_str();
    options.app_id = "org.plainroot.FileChooser";
    options.initial_width = 1000;
    options.initial_height = 720;
    options.minimum_width = 320;
    options.minimum_height = 320;
    options.maximum_width = 1600;
    options.maximum_height = 1200;
    options.content_max_width = 1280.0F;
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
      finish(request, kResponseError, nullptr, false);
      return;
    }
    request.controller->set_window(request.window);
    request.controller->navigate(request.start_path);
  }

  static void browser_completed(void *user_data, bool accepted,
                                const char *native_path) {
    auto &request = *static_cast<PortalRequest *>(user_data);
    request.backend->finish(request,
                            accepted ? kResponseSuccess : kResponseCancelled,
                            native_path, true);
  }

  static int window_close_requested(void *user_data,
                                    plainroot_toolkit_window *) {
    auto &request = *static_cast<PortalRequest *>(user_data);
    request.backend->finish(request, kResponseCancelled, nullptr, false);
    return 1;
  }

  void send_reply(PortalRequest &request, uint32_t response,
                  const char *uri) {
    if (request.call == nullptr)
      return;
    sd_bus_message *reply = nullptr;
    int result = sd_bus_message_new_method_return(request.call, &reply);
    if (result >= 0)
      result = sd_bus_message_append(reply, "u", response);
    if (result >= 0)
      result = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "{sv}");
    if (result >= 0 && uri != nullptr && !append_uris(reply, uri))
      result = -EINVAL;
    if (result >= 0)
      result = sd_bus_message_close_container(reply);
    if (result >= 0)
      result = sd_bus_send(bus_, reply, nullptr);
    sd_bus_message_unref(reply);
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
          finish(*request, kResponseCancelled, nullptr, false);
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

int unsupported_method(sd_bus_message *message, void *, sd_bus_error *) {
  return sd_bus_reply_method_errorf(
      message, SD_BUS_ERROR_NOT_SUPPORTED,
      "This FileChooser operation is not implemented yet");
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
  g_source_modify_unix_fd(source, watch->fd_tag, conditions);
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
