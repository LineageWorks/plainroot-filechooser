#include "browser_controller.hpp"

#include "directory_loader.hpp"
#include "storage_bridge.hpp"
#include "storage_locations_view.hpp"

#include <plainroot/i18n/i18n.h>

#include <glib.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fnmatch.h>
#include <limits.h>
#include <memory>
#include <cstring>
#include <sys/stat.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plainroot::filechooser {
namespace {

constexpr char kDomain[] = "plainroot-filechooser";
constexpr float kPagePadding = 16.0F;
constexpr float kContentSpacing = 16.0F;
constexpr float kExpandedControlsWidth = 840.0F;

std::string tr(const char *message) {
  const char *translated = plainroot_i18n_gettext_v1(kDomain, message);
  return translated == nullptr ? message : translated;
}

std::string display_path(const std::string &path) {
  gchar *display = g_filename_display_name(path.c_str());
  if (display == nullptr)
    return path;
  std::string result(display);
  g_free(display);
  return result;
}

std::string join_path(std::string_view directory, std::string_view name) {
  if (directory.empty() || directory == "/")
    return "/" + std::string(name);
  std::string result(directory);
  if (result.back() != '/')
    result.push_back('/');
  result.append(name);
  return result;
}

std::string parent_path(std::string_view path) {
  if (path.empty() || path == "/")
    return "/";
  size_t end = path.size();
  while (end > 1 && path[end - 1] == '/')
    --end;
  const size_t separator = path.rfind('/', end - 1);
  if (separator == std::string_view::npos)
    return ".";
  if (separator == 0)
    return "/";
  return std::string(path.substr(0, separator));
}

} // namespace

class BrowserController::Impl {
public:
  Impl(plainroot_toolkit_application *application,
       plainroot_ui_context *context, CompletionFn completion,
       void *completion_user_data)
      : application_(application), context_(context), completion_(completion),
        completion_user_data_(completion_user_data),
        loader_(application, [this](detail::DirectoryResult result) {
          apply_directory_result(std::move(result));
        }) {}

  ~Impl() { shutdown(); }

  bool ready() const { return loader_.ready(); }

  void shutdown() {
    if (storage_bridge_ != nullptr)
      storage_bridge_->stop();
    loader_.stop();
  }

  struct EntryAction {
    Impl *controller = nullptr;
    std::string path;
    plainroot_core_fs_entry_kind_v1 kind = PLAINROOT_CORE_FS_ENTRY_UNKNOWN;
  };

  bool create_ui() {
    auto *outer =
        plainroot_ui_vertical_scroll_area_create_v1(context_, 0, 0, 1.0F);
    auto *padding = plainroot_ui_padding_create_v1(
        context_, kPagePadding, kPagePadding, kPagePadding, kPagePadding, 1.0F);
    auto *page = plainroot_ui_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, kContentSpacing, 1.0F);
    const char *default_title =
        select_directory_ ? "Choose a folder"
                           : (save_file_ ? "Save file"
                                         : (allow_multiple_ ? "Choose files"
                                                            : "Choose a file"));
    const std::string title_text = requested_title_.empty()
                                       ? tr(default_title)
                                       : requested_title_;
    auto *title = plainroot_ui_label_create_v1(
        context_, title_text.c_str(), static_cast<uint32_t>(title_text.size()),
        PLAINROOT_UI_COLOR_ON_SURFACE_V3, 0.0F);
    if (title != nullptr)
      (void)plainroot_ui_label_set_typography_v1(
          title, PLAINROOT_UI_LABEL_HEADLINE_SMALL);

    storage_view_ = std::make_unique<detail::StorageLocationsView>(
        context_,
        detail::StorageLocationActions{
            [this](std::string id, bool read_only) {
              mount_volume(id, read_only);
            },
            [this](std::string path) { navigate(std::move(path)); },
            [this](std::string id) { unmount_mount(id); },
            [this](std::string id) { disconnect_network(id); },
            [this](std::string host, std::string username, uint16_t port,
                   std::string remote_path) {
              return connect_sftp(std::move(host), std::move(username), port,
                                  std::move(remote_path));
            },
            [this] { refresh_storage(); }, [this] { request_redraw(); }});
    auto *storage_view_widget = storage_view_->create();

    auto *location_row = plainroot_ui_adaptive_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, PLAINROOT_UI_AXIS_HORIZONTAL,
        kExpandedControlsWidth, 8.0F, 0.0F);
    auto *location_actions = plainroot_ui_box_create_v1(
        context_, PLAINROOT_UI_AXIS_HORIZONTAL, 8.0F, 0.0F);
    auto *up = make_button("Up", &Impl::up_activated);
    auto *home = make_button("Home", &Impl::home_activated);
    const std::string initial_path = tr("Loading folder…");
    path_label_ =
        plainroot_ui_label_create_v1(context_, initial_path.c_str(),
                                     static_cast<uint32_t>(initial_path.size()),
                                     PLAINROOT_UI_COLOR_ON_SURFACE_V3, 1.0F);
    if (path_label_ != nullptr)
      (void)plainroot_ui_widget_set_text_direction_v1(path_label_,
                                                      PLAINROOT_UI_TEXT_LTR);

    auto *controls = plainroot_ui_adaptive_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, PLAINROOT_UI_AXIS_HORIZONTAL,
        kExpandedControlsWidth, 12.0F, 0.0F);
    auto *filename_row = save_file_ ? plainroot_ui_adaptive_box_create_v1(
                                          context_, PLAINROOT_UI_AXIS_VERTICAL,
                                          PLAINROOT_UI_AXIS_HORIZONTAL,
                                          kExpandedControlsWidth, 8.0F, 0.0F)
                                    : nullptr;
    auto *filename_label = save_file_ ? make_label("File name", 0.0F) : nullptr;
    const std::string empty;
    const std::string search_hint = tr("Search files");
    search_field_ = plainroot_ui_search_field_create_v1(
        context_, empty.data(), 0, search_hint.c_str(),
        static_cast<uint32_t>(search_hint.size()), 512, 1.0F,
        &Impl::search_changed, this);
    const std::string hidden_label = tr("Show hidden files");
    hidden_switch_ = plainroot_ui_switch_create_v1(
        context_, hidden_label.c_str(),
        static_cast<uint32_t>(hidden_label.size()), 0, 0.0F,
        &Impl::hidden_changed, this);
    if (filters_.size() > 1U) {
      const std::string filter_hint = tr("Filter files");
      filter_dropdown_ = plainroot_ui_dropdown_create_v1(
          context_, filter_hint.c_str(),
          static_cast<uint32_t>(filter_hint.size()), 1.0F,
          &Impl::filter_changed, this);
    }
    if (save_file_) {
      const std::string hint = tr("File name");
      filename_field_ = plainroot_ui_text_field_create_v1(
          context_, initial_filename_.data(),
          static_cast<uint32_t>(initial_filename_.size()), hint.c_str(),
          static_cast<uint32_t>(hint.size()), NAME_MAX, 1.0F,
          &Impl::filename_changed, this);
      if (filename_field_ != nullptr)
        (void)plainroot_ui_widget_set_text_direction_v1(
            filename_field_, PLAINROOT_UI_TEXT_LTR);
    }

    list_scroll_ =
        plainroot_ui_vertical_scroll_area_create_v1(context_, 0, 0, 1.0F);
    entries_list_ = plainroot_ui_list_create_v1(context_, 1.0F);
    status_label_ = make_label("Loading folder…", 0.0F);
    auto *footer = plainroot_ui_adaptive_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, PLAINROOT_UI_AXIS_HORIZONTAL,
        kExpandedControlsWidth, 12.0F, 0.0F);
    const std::string initial_selection_summary =
        allow_multiple_ ? selection_summary()
                        : tr(select_directory_
                                 ? "The current folder will be returned."
                                 : (save_file_
                                        ? "Enter a name for the file to save."
                                        : "No file selected"));
    selected_label_ = plainroot_ui_label_create_v1(
        context_, initial_selection_summary.data(),
        static_cast<uint32_t>(initial_selection_summary.size()),
        PLAINROOT_UI_COLOR_ON_SURFACE_V3, 1.0F);
    if (selected_label_ != nullptr)
      (void)plainroot_ui_widget_set_text_direction_v1(selected_label_,
          allow_multiple_ ? PLAINROOT_UI_TEXT_DIRECTION_INHERIT
                          : PLAINROOT_UI_TEXT_LTR);
    auto *select = make_action_button(
        accept_label_.empty()
            ? tr(select_directory_ ? "Select folder"
                                   : (save_file_ ? "Save" : "Open"))
            : accept_label_,
        &Impl::accept_activated);
    if (select != nullptr)
      (void)plainroot_ui_button_set_style_v1(select,
                                             PLAINROOT_UI_BUTTON_STYLE_FILLED);

    if (outer == nullptr || padding == nullptr || page == nullptr ||
        title == nullptr || location_row == nullptr ||
        location_actions == nullptr || up == nullptr || home == nullptr ||
        storage_view_widget == nullptr || path_label_ == nullptr ||
        controls == nullptr || search_field_ == nullptr ||
        hidden_switch_ == nullptr ||
        (filters_.size() > 1U && filter_dropdown_ == nullptr) ||
        list_scroll_ == nullptr ||
        entries_list_ == nullptr || status_label_ == nullptr ||
        footer == nullptr || selected_label_ == nullptr || select == nullptr ||
        (save_file_ && (filename_row == nullptr || filename_label == nullptr ||
                        filename_field_ == nullptr)))
      return false;

    if (filter_dropdown_ != nullptr) {
      for (size_t index = 0; index < filters_.size(); ++index) {
        const BrowserFilter &filter = filters_[index];
        auto *option = plainroot_ui_list_box_option_create_v1(
            context_, filter.name.c_str(),
            static_cast<uint32_t>(filter.name.size()),
            static_cast<int32_t>(index), 0.0F);
        if (!add(filter_dropdown_, option))
          return false;
      }
      initializing_filter_dropdown_ = true;
      const auto result = plainroot_ui_dropdown_set_value_v1(
          filter_dropdown_, selected_filter_index_);
      initializing_filter_dropdown_ = false;
      if (result != PLAINROOT_UI_RESULT_OK)
        return false;
    }

    if (!add(outer, padding) || !add(padding, page) || !add(page, title) ||
        !add(page, location_row) || !add(location_row, location_actions) ||
        !add(location_actions, up) || !add(location_actions, home) ||
        (save_file_ &&
         (!add(page, filename_row) || !add(filename_row, filename_label) ||
          !add(filename_row, filename_field_))) ||
        !add(location_row, path_label_) || !add(page, controls) ||
        !add(controls, search_field_) || !add(controls, hidden_switch_) ||
        (filter_dropdown_ != nullptr && !add(controls, filter_dropdown_)) ||
        !add(page, storage_view_widget) || !add(page, list_scroll_) ||
        !add(list_scroll_, entries_list_) || !add(page, status_label_) ||
        !add(page, footer) || !add(footer, selected_label_) ||
        !add(footer, select) ||
        plainroot_ui_context_set_root_v1(context_, outer) !=
            PLAINROOT_UI_RESULT_OK)
      return false;

    ui_ready_ = true;
    storage_bridge_ = std::make_unique<detail::StorageBridge>(
        application_,
        [this](detail::StorageCatalog catalog) {
          apply_storage_catalog(std::move(catalog));
        },
        [this](detail::StorageOperationUpdate update) {
          apply_storage_operation(update);
        },
        [this](detail::NetworkOperationUpdate update) {
          apply_network_operation(std::move(update));
        },
        [this](detail::StorageServiceError error) {
          apply_storage_error(error);
        });
    if (!storage_bridge_->ready())
      set_storage_status(tr("Storage locations are unavailable."));
    else
      refresh_storage();
    return true;
  }

  void set_window(plainroot_toolkit_window *window) { window_ = window; }

  void set_title(std::string title) { requested_title_ = std::move(title); }

  void set_select_directory(bool select_directory) {
    select_directory_ = select_directory;
  }

  void set_allow_multiple(bool allow_multiple) {
    allow_multiple_ = allow_multiple;
  }

  void set_filters(std::vector<BrowserFilter> filters,
                   int32_t selected_filter) {
    filters_ = std::move(filters);
    if (filters_.empty()) {
      selected_filter_index_ = -1;
    } else if (selected_filter >= 0 &&
               static_cast<size_t>(selected_filter) < filters_.size()) {
      selected_filter_index_ = selected_filter;
    } else {
      selected_filter_index_ = 0;
    }
  }

  void set_save_file(bool save_file) { save_file_ = save_file; }

  void set_initial_filename(std::string filename) {
    initial_filename_ = std::move(filename);
    if (filename_field_ != nullptr)
      set_label(filename_field_, initial_filename_);
  }

  void set_accept_label(std::string label) { accept_label_ = std::move(label); }

  void cancel() {
    if (!finished_ && completion_ != nullptr) {
      finished_ = true;
      completion_(completion_user_data_, false, {}, nullptr);
    }
  }

  void navigate(std::string path) {
    if (path.empty() || path.front() != '/') {
      set_status(tr("Choose an absolute directory path."));
      return;
    }
    current_path_ = std::move(path);
    selected_path_.clear();
    selected_paths_.clear();
    ++generation_;
    set_label(path_label_, display_path(current_path_));
    if (allow_multiple_) {
      update_selection_summary();
    } else {
      set_label(selected_label_,
                tr(select_directory_
                       ? "The current folder will be returned."
                       : (save_file_ ? "Enter a name for the file to save."
                                     : "No file selected")));
    }
    set_status(tr("Loading folder…"));
    loader_.request(current_path_, generation_);
    schedule_render();
    request_redraw();
  }

private:
  using ButtonCallback = void (*)(void *, plainroot_ui_widget *);

  static bool add(plainroot_ui_widget *parent, plainroot_ui_widget *child) {
    return parent != nullptr && child != nullptr &&
           plainroot_ui_widget_add_child_v1(parent, child) ==
               PLAINROOT_UI_RESULT_OK;
  }

  plainroot_ui_widget *make_label(const char *message, float flex) {
    const std::string text = tr(message);
    return plainroot_ui_label_create_v1(context_, text.c_str(),
                                        static_cast<uint32_t>(text.size()),
                                        PLAINROOT_UI_COLOR_ON_SURFACE_V3, flex);
  }

  plainroot_ui_widget *make_button(const char *message,
                                   ButtonCallback callback) {
    const std::string text = tr(message);
    return plainroot_ui_button_create_v1(context_, text.c_str(),
                                         static_cast<uint32_t>(text.size()),
                                         0.0F, callback, this);
  }

  plainroot_ui_widget *make_action_button(const std::string &label,
                                         ButtonCallback callback) {
    return plainroot_ui_button_create_v1(
        context_, label.c_str(), static_cast<uint32_t>(label.size()), 0.0F,
        callback, this);
  }

  static void up_activated(void *user_data, plainroot_ui_widget *) {
    auto &self = *static_cast<Impl *>(user_data);
    self.navigate(parent_path(self.current_path_));
  }

  static void home_activated(void *user_data, plainroot_ui_widget *) {
    auto &self = *static_cast<Impl *>(user_data);
    const char *home = std::getenv("HOME");
    self.navigate(home != nullptr && home[0] == '/' ? home : "/");
  }

  static void search_changed(void *user_data, plainroot_ui_widget *widget) {
    auto &self = *static_cast<Impl *>(user_data);
    const char *value = nullptr;
    uint32_t length = 0;
    if (plainroot_ui_text_field_get_text_v1(widget, &value, &length) !=
            PLAINROOT_UI_RESULT_OK ||
        (value == nullptr && length != 0U))
      return;
    self.search_query_.assign(value == nullptr ? "" : value, length);
    self.schedule_render();
  }

  static void hidden_changed(void *user_data, plainroot_ui_widget *widget) {
    auto &self = *static_cast<Impl *>(user_data);
    uint32_t checked = 0;
    if (plainroot_ui_switch_get_checked_v1(widget, &checked) !=
        PLAINROOT_UI_RESULT_OK)
      return;
    self.show_hidden_ = checked != 0;
    self.schedule_render();
  }

  static void filter_changed(void *user_data, plainroot_ui_widget *widget) {
    auto &self = *static_cast<Impl *>(user_data);
    if (self.initializing_filter_dropdown_)
      return;
    int32_t selected = -1;
    uint32_t has_value = 0;
    if (plainroot_ui_dropdown_get_value_v1(widget, &selected, &has_value) !=
            PLAINROOT_UI_RESULT_OK ||
        has_value == 0 || selected < 0 ||
        static_cast<size_t>(selected) >= self.filters_.size())
      return;
    self.selected_filter_index_ = selected;
    self.schedule_render();
    self.request_redraw();
  }

  static void filename_changed(void *user_data, plainroot_ui_widget *widget) {
    auto &self = *static_cast<Impl *>(user_data);
    const char *value = nullptr;
    uint32_t length = 0;
    if (plainroot_ui_text_field_get_text_v1(widget, &value, &length) !=
            PLAINROOT_UI_RESULT_OK ||
        (value == nullptr && length != 0U))
      return;
    self.initial_filename_.assign(value == nullptr ? "" : value, length);
  }

  static void entry_activated(void *user_data, plainroot_ui_widget *) {
    auto &action = *static_cast<EntryAction *>(user_data);
    if (action.kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY) {
      action.controller->navigate(action.path);
      return;
    }
    if (action.controller->select_directory_) {
      action.controller->set_status(
          tr("Choose the current folder with Select folder."));
      action.controller->request_redraw();
      return;
    }
    if (action.controller->save_file_) {
      const std::string filename = parent_path(action.path) == "/"
                                       ? action.path.substr(1)
                                       : action.path.substr(
                                             action.path.find_last_of('/') + 1U);
      set_label(action.controller->filename_field_, filename);
      action.controller->initial_filename_ = filename;
      action.controller->set_status(tr("File name copied from the selection."));
      action.controller->request_redraw();
      return;
    }
    if (action.controller->allow_multiple_)
      return;
    action.controller->selected_path_ = action.path;
    set_label(action.controller->selected_label_, display_path(action.path));
    action.controller->set_status(
        tr("File selected. Choose Open to return it."));
    action.controller->request_redraw();
  }

  static void selection_changed(void *user_data, plainroot_ui_widget *widget) {
    auto &action = *static_cast<EntryAction *>(user_data);
    if (action.controller == nullptr || widget == nullptr)
      return;
    uint32_t checked = 0;
    if (plainroot_ui_checkbox_get_checked_v1(widget, &checked) !=
        PLAINROOT_UI_RESULT_OK)
      return;
    auto &selected = action.controller->selected_paths_;
    const auto found = std::find(selected.begin(), selected.end(), action.path);
    if (checked != 0U) {
      if (found == selected.end())
        selected.push_back(action.path);
    } else if (found != selected.end()) {
      selected.erase(found);
    }
    action.controller->update_selection_summary();
    action.controller->request_redraw();
  }

  static void accept_activated(void *user_data, plainroot_ui_widget *) {
    auto &self = *static_cast<Impl *>(user_data);
    if (self.save_file_) {
      self.accept_save_file();
      return;
    }
    if (self.allow_multiple_) {
      if (self.selected_paths_.empty()) {
        self.set_status(tr("Select at least one file."));
        self.request_redraw();
        return;
      }
      self.complete_selection(self.selected_paths_);
      return;
    }
    if (!self.select_directory_ && self.selected_path_.empty()) {
      self.set_status(tr("Select a file first."));
      self.request_redraw();
      return;
    }
    const std::string &selected_path =
        self.select_directory_ ? self.current_path_ : self.selected_path_;
    self.complete_selection({selected_path});
  }

  static void overwrite_primary(void *user_data,
                                plainroot_toolkit_window *) {
    static_cast<Impl *>(user_data)->overwrite_confirmed_ = true;
  }

  static void overwrite_secondary(void *, plainroot_toolkit_window *) {}

  static void overwrite_dismissed(
      void *user_data, plainroot_toolkit_window *,
      plainroot_toolkit_dialog_dismiss_reason_v1) {
    auto &self = *static_cast<Impl *>(user_data);
    self.overwrite_dialog_open_ = false;
    const bool confirmed = self.overwrite_confirmed_;
    self.overwrite_confirmed_ = false;
    const std::string path = std::move(self.pending_save_path_);
    self.pending_save_path_.clear();
    if (confirmed)
      self.complete_selection({path});
  }

  void accept_save_file() {
    if (initial_filename_.empty()) {
      set_status(tr("Enter a file name."));
      request_redraw();
      return;
    }
    if (initial_filename_ == "." || initial_filename_ == ".." ||
        initial_filename_.find('/') != std::string::npos) {
      set_status(tr("Enter a file name without a folder path."));
      request_redraw();
      return;
    }
    if (initial_filename_.size() > NAME_MAX) {
      set_status(tr("The file name is too long."));
      request_redraw();
      return;
    }

    const std::string path = join_path(current_path_, initial_filename_);
    struct stat info {};
    if (lstat(path.c_str(), &info) != 0) {
      if (errno == ENOENT) {
        complete_selection({path});
        return;
      }
      gchar *detail = g_strdup_printf(
          tr("Unable to check the target file: %s").c_str(),
          g_strerror(errno));
      set_status(detail == nullptr ? tr("Unable to check the target file.")
                                   : detail);
      g_free(detail);
      request_redraw();
      return;
    }
    if (S_ISDIR(info.st_mode)) {
      set_status(tr("A folder with this name already exists."));
      request_redraw();
      return;
    }
    show_overwrite_confirmation(path);
  }

  void show_overwrite_confirmation(const std::string &path) {
    if (window_ == nullptr || overwrite_dialog_open_)
      return;
    pending_save_path_ = path;
    overwrite_confirmed_ = false;
    overwrite_dialog_open_ = true;

    gchar *message = g_strdup_printf(
        tr("A file named “%s” already exists. Replace it?").c_str(),
        display_path(initial_filename_).c_str());
    if (message == nullptr) {
      overwrite_dialog_open_ = false;
      set_status(tr("Unable to show the overwrite confirmation."));
      request_redraw();
      return;
    }
    const std::string title = tr("Replace existing file?");
    const std::string replace = tr("Replace");
    const std::string cancel = tr("Cancel");
    plainroot_toolkit_dialog_options_v1 options{};
    options.struct_size = sizeof(options);
    options.title_utf8 = title.c_str();
    options.title_length = static_cast<uint32_t>(title.size());
    options.message_utf8 = message;
    options.message_length = static_cast<uint32_t>(std::strlen(message));
    options.primary_action_utf8 = replace.c_str();
    options.primary_action_length = static_cast<uint32_t>(replace.size());
    options.secondary_action_utf8 = cancel.c_str();
    options.secondary_action_length = static_cast<uint32_t>(cancel.size());
    options.text_direction =
        plainroot_i18n_get_layout_direction_v1() ==
                PLAINROOT_I18N_LAYOUT_DIRECTION_RTL
            ? PLAINROOT_UI_TEXT_RTL
            : PLAINROOT_UI_TEXT_LTR;
    options.dismiss_on_escape = 1;
    options.dismiss_on_backdrop = 1;
    options.width = 440.0F;
    options.primary_action = overwrite_primary;
    options.secondary_action = overwrite_secondary;
    options.dismissed = overwrite_dismissed;
    options.user_data = this;
    const auto result =
        plainroot_toolkit_window_show_confirmation_dialog_v1(window_, &options);
    g_free(message);
    if (result != PLAINROOT_TOOLKIT_RESULT_OK) {
      overwrite_dialog_open_ = false;
      pending_save_path_.clear();
      set_status(tr("Unable to show the overwrite confirmation."));
      request_redraw();
    }
  }

  std::string selection_summary() const {
    char formatted_count[64]{};
    size_t formatted_length = 0;
    const char *language_tag = plainroot_i18n_language_tag_v1();
    if (language_tag == nullptr)
      language_tag = "en";
    const auto number_result = plainroot_i18n_format_number_for_tag_v1(
        language_tag, static_cast<double>(selected_paths_.size()), 1, 0, 0, 0,
        formatted_count, sizeof(formatted_count), &formatted_length);
    (void)formatted_length;
    const std::string count = number_result == PLAINROOT_I18N_RESULT_OK
                                  ? formatted_count
                                  : std::to_string(selected_paths_.size());
    gchar *summary =
        g_strdup_printf(tr("Selected files: %s").c_str(), count.c_str());
    if (summary == nullptr)
      return tr("Selected files");
    std::string result(summary);
    g_free(summary);
    return result;
  }

  void update_selection_summary() {
    if (allow_multiple_ && selected_label_ != nullptr)
      set_label(selected_label_, selection_summary());
  }

  void complete_selection(const std::vector<std::string> &paths) {
    if (finished_)
      return;
    if (paths.empty())
      return;
    finished_ = true;
    if (completion_ != nullptr) {
      const BrowserFilter *current_filter =
          selected_filter_index_ >= 0 &&
                  static_cast<size_t>(selected_filter_index_) < filters_.size()
              ? &filters_[static_cast<size_t>(selected_filter_index_)]
              : nullptr;
      completion_(completion_user_data_, true, paths, current_filter);
      return;
    }
    for (const std::string &path : paths) {
      (void)std::fwrite(path.data(), 1, path.size(), stdout);
      (void)std::fputc('\n', stdout);
    }
    (void)std::fflush(stdout);
    plainroot_toolkit_application_quit(application_);
  }

  static void render_task(void *user_data) {
    static_cast<Impl *>(user_data)->render_entries();
  }

  void schedule_render() {
    if (!ui_ready_)
      return;
    if (plainroot_toolkit_application_post_v1(application_, &Impl::render_task,
                                              this, nullptr) !=
        PLAINROOT_TOOLKIT_RESULT_OK)
      return;
  }

  void apply_directory_result(detail::DirectoryResult result) {
    if (result.generation != generation_)
      return;
    if (result.result == PLAINROOT_CORE_FS_OK) {
      entries_ = std::move(result.entries);
      status_error_.clear();
      set_status(entries_.empty() ? tr("This folder is empty.") : "");
    } else {
      entries_.clear();
      if (result.native_code == 0) {
        status_error_ = tr("Unable to read this folder.");
      } else {
        gchar *detail =
            g_strdup_printf(tr("Unable to read this folder: %s").c_str(),
                            g_strerror(result.native_code));
        status_error_ =
            detail == nullptr ? tr("Unable to read this folder.") : detail;
        g_free(detail);
      }
      set_status(status_error_);
    }
    schedule_render();
    request_redraw();
  }

  void apply_storage_catalog(detail::StorageCatalog catalog) {
    const bool complete = catalog.complete;
    const bool empty =
        catalog.removable_volumes.empty() && catalog.network_mounts.empty();
    storage_catalog_complete_ = complete;
    storage_view_->set_catalog(std::move(catalog));
    if (!complete) {
      set_storage_status(tr(
          "Storage information is incomplete; removable-volume actions are unavailable."));
    } else if (empty) {
      set_storage_status(
          tr("No removable volumes or mounted network locations."));
    } else {
      set_storage_status(tr("Storage locations are up to date."));
    }
    request_redraw();
  }

  void apply_storage_error(detail::StorageServiceError error) {
    const char *message = "Unable to read storage locations.";
    if (error.result == PLAINROOT_STORAGE_NOT_READY)
      message = "Storage service is unavailable.";
    else if (error.result == PLAINROOT_STORAGE_PERMISSION_DENIED)
      message = "Storage access was denied.";
    else if (error.result == PLAINROOT_STORAGE_BUSY)
      message = "Storage service is busy.";
    set_storage_status(tr(message));
    request_redraw();
  }

  void refresh_storage() {
    const auto result = storage_bridge_ == nullptr
                            ? PLAINROOT_STORAGE_NOT_READY
                            : storage_bridge_->request_snapshot();
    if (result == PLAINROOT_STORAGE_OK)
      set_storage_status(tr("Checking storage locations…"));
    else
      apply_storage_error({result, 0});
    request_redraw();
  }

  void apply_storage_operation(detail::StorageOperationUpdate update) {
    if (update.type == PLAINROOT_STORAGE_CLIENT_OPERATION_REQUEST_COMPLETED) {
      if (update.result != PLAINROOT_STORAGE_OK) {
        storage_operation_pending_ = false;
        set_storage_status(tr("Unable to start the storage operation."));
      } else {
        set_storage_status(tr("Storage operation accepted."));
      }
      request_redraw();
      return;
    }

    if (update.state == PLAINROOT_STORAGE_OPERATION_SUCCEEDED) {
      storage_operation_pending_ = false;
      set_storage_status(tr("Storage operation completed."));
      if (storage_bridge_ != nullptr)
        (void)storage_bridge_->request_snapshot();
    } else if (update.state == PLAINROOT_STORAGE_OPERATION_FAILED) {
      storage_operation_pending_ = false;
      set_storage_status(tr(storage_operation_error_message(update.error)));
      if (storage_bridge_ != nullptr)
        (void)storage_bridge_->request_snapshot();
    } else if (update.state == PLAINROOT_STORAGE_OPERATION_CANCELLED) {
      storage_operation_pending_ = false;
      set_storage_status(tr("Storage operation was cancelled."));
    } else if (update.state == PLAINROOT_STORAGE_OPERATION_INDETERMINATE) {
      storage_operation_pending_ = false;
      set_storage_status(tr("Storage operation outcome is uncertain."));
      if (storage_bridge_ != nullptr)
        (void)storage_bridge_->request_snapshot();
    } else {
      const bool unmount =
          update.operation_kind == PLAINROOT_STORAGE_OPERATION_UNMOUNT;
      set_storage_status(
          tr(unmount ? "Unmounting location…" : "Mounting volume…"));
    }
    request_redraw();
  }

  void apply_network_operation(detail::NetworkOperationUpdate update) {
    if (update.type == PLAINROOT_STORAGE_NETWORK_EVENT_ERROR) {
      storage_operation_pending_ = false;
      set_storage_status(tr(update.result == PLAINROOT_STORAGE_NOT_READY
                                ? "Network service is unavailable."
                                : "Unable to start the network operation."));
      request_redraw();
      return;
    }

    if (update.type == PLAINROOT_STORAGE_NETWORK_EVENT_REQUEST_COMPLETED) {
      if (update.request_kind != PLAINROOT_STORAGE_NETWORK_REQUEST_CONNECT &&
          update.request_kind !=
              PLAINROOT_STORAGE_NETWORK_REQUEST_DISCONNECT)
        return;
      if (update.result != PLAINROOT_STORAGE_OK) {
        storage_operation_pending_ = false;
        set_storage_status(tr(update.result == PLAINROOT_STORAGE_NOT_READY
                                  ? "Network service is unavailable."
                                  : "Unable to start the network operation."));
      } else {
        const bool disconnect =
            update.request_kind == PLAINROOT_STORAGE_NETWORK_REQUEST_DISCONNECT;
        set_storage_status(tr(disconnect ? "Disconnecting network location…"
                                         : "Connecting to SFTP location…"));
      }
      request_redraw();
      return;
    }

    if (update.type != PLAINROOT_STORAGE_NETWORK_EVENT_OPERATION_CHANGED)
      return;
    if (update.state == PLAINROOT_STORAGE_NETWORK_OPERATION_SUCCEEDED) {
      storage_operation_pending_ = false;
      const bool disconnect =
          update.operation_kind == PLAINROOT_STORAGE_NETWORK_OPERATION_DISCONNECT;
      set_storage_status(tr(disconnect ? "Network location disconnected."
                                       : "Network location connected."));
      if (storage_bridge_ != nullptr)
        (void)storage_bridge_->request_snapshot();
    } else if (update.state == PLAINROOT_STORAGE_NETWORK_OPERATION_FAILED) {
      storage_operation_pending_ = false;
      set_storage_status(tr(network_operation_error_message(update.error)));
      if (storage_bridge_ != nullptr)
        (void)storage_bridge_->request_snapshot();
    } else if (update.state ==
               PLAINROOT_STORAGE_NETWORK_OPERATION_CANCELLED) {
      storage_operation_pending_ = false;
      set_storage_status(tr("Network connection was cancelled."));
    } else if (update.state ==
               PLAINROOT_STORAGE_NETWORK_OPERATION_INDETERMINATE) {
      storage_operation_pending_ = false;
      set_storage_status(tr("Network operation outcome is uncertain."));
      if (storage_bridge_ != nullptr)
        (void)storage_bridge_->request_snapshot();
    } else if (update.operation_kind ==
               PLAINROOT_STORAGE_NETWORK_OPERATION_DISCONNECT) {
      set_storage_status(tr("Disconnecting network location…"));
    } else {
      set_storage_status(tr("Connecting to SFTP location…"));
    }
    request_redraw();
  }

  static const char *network_operation_error_message(uint32_t error) {
    switch (error) {
    case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_INVALID_REQUEST:
      return "Check the server address and try again.";
    case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_DEPENDENCY_UNAVAILABLE:
      return "SFTP support is unavailable.";
    case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_MOUNTPOINT_UNAVAILABLE:
      return "A mount location is unavailable.";
    case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_SYSTEMD_FAILED:
      return "Unable to start the network mount service.";
    case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_CONNECTION_FAILED:
      return "Could not connect to the SFTP server.";
    case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_UNMOUNT_FAILED:
      return "Could not disconnect the network location.";
    case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_CANCELLED:
      return "Network connection was cancelled.";
    case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_OUTCOME_UNKNOWN:
      return "Network operation outcome is uncertain.";
    default:
      return "The network operation failed.";
    }
  }

  static const char *storage_operation_error_message(uint32_t error) {
    switch (error) {
    case PLAINROOT_STORAGE_OPERATION_ERROR_AUTHORIZATION_DENIED:
      return "Storage authorization was denied.";
    case PLAINROOT_STORAGE_OPERATION_ERROR_INCOMPLETE_SNAPSHOT:
      return "Storage information is incomplete; refresh and try again.";
    case PLAINROOT_STORAGE_OPERATION_ERROR_STALE_ENTITY:
      return "This storage device changed. Refresh and try again.";
    case PLAINROOT_STORAGE_OPERATION_ERROR_UNSUPPORTED_FILESYSTEM:
      return "This filesystem is not supported for mounting.";
    case PLAINROOT_STORAGE_OPERATION_ERROR_ALREADY_MOUNTED:
      return "This volume is already mounted.";
    case PLAINROOT_STORAGE_OPERATION_ERROR_MOUNT_NOT_OWNED:
      return "This location is managed by another service.";
    case PLAINROOT_STORAGE_OPERATION_ERROR_MOUNTPOINT_UNAVAILABLE:
      return "A mount location is unavailable.";
    case PLAINROOT_STORAGE_OPERATION_ERROR_AUTHORIZATION_UNAVAILABLE:
      return "Storage authorization is unavailable.";
    default:
      return "The storage operation failed.";
    }
  }

  void mount_volume(const std::string &id, bool read_only) {
    if (!storage_catalog_complete_) {
      set_storage_status(tr(
          "Storage information is incomplete; removable-volume actions are unavailable."));
      request_redraw();
      return;
    }
    if (storage_operation_pending_) {
      set_storage_status(tr("A storage operation is already in progress."));
      request_redraw();
      return;
    }
    const auto result = storage_bridge_ == nullptr
                            ? PLAINROOT_STORAGE_NOT_READY
                            : storage_bridge_->mount_volume(id, read_only);
    if (result == PLAINROOT_STORAGE_OK) {
      storage_operation_pending_ = true;
      set_storage_status(tr("Mounting volume…"));
    } else {
      apply_storage_error({result, 0});
    }
    request_redraw();
  }

  void unmount_mount(const std::string &id) {
    if (!storage_catalog_complete_) {
      set_storage_status(tr(
          "Storage information is incomplete; removable-volume actions are unavailable."));
      request_redraw();
      return;
    }
    if (storage_operation_pending_) {
      set_storage_status(tr("A storage operation is already in progress."));
      return;
    }
    const auto result = storage_bridge_ == nullptr
                            ? PLAINROOT_STORAGE_NOT_READY
                            : storage_bridge_->unmount_mount(id);
    if (result == PLAINROOT_STORAGE_OK) {
      storage_operation_pending_ = true;
      set_storage_status(tr("Unmounting location…"));
    } else {
      apply_storage_error({result, 0});
    }
    request_redraw();
  }

  plainroot_storage_result_v1 connect_sftp(std::string host,
                                           std::string username, uint16_t port,
                                           std::string remote_path) {
    if (storage_operation_pending_) {
      set_storage_status(tr("A storage operation is already in progress."));
      request_redraw();
      return PLAINROOT_STORAGE_BUSY;
    }
    const auto result = storage_bridge_ == nullptr
                            ? PLAINROOT_STORAGE_NOT_READY
                            : storage_bridge_->connect_sftp(host, username, port,
                                                            remote_path);
    if (result == PLAINROOT_STORAGE_OK) {
      storage_operation_pending_ = true;
      set_storage_status(tr("Connecting to SFTP location…"));
    } else {
      set_storage_status(tr(result == PLAINROOT_STORAGE_NOT_READY
                                ? "Network service is unavailable."
                                : "Unable to start the network operation."));
    }
    request_redraw();
    return result;
  }

  void disconnect_network(const std::string &id) {
    if (storage_operation_pending_) {
      set_storage_status(tr("A storage operation is already in progress."));
      request_redraw();
      return;
    }
    const auto result = storage_bridge_ == nullptr
                            ? PLAINROOT_STORAGE_NOT_READY
                            : storage_bridge_->disconnect_network(id);
    if (result == PLAINROOT_STORAGE_OK) {
      storage_operation_pending_ = true;
      set_storage_status(tr("Disconnecting network location…"));
    } else {
      set_storage_status(tr(result == PLAINROOT_STORAGE_NOT_READY
                                ? "Network service is unavailable."
                                : "Unable to start the network operation."));
    }
    request_redraw();
  }

  void render_entries() {
    for (plainroot_ui_widget *row : rows_) {
      (void)plainroot_ui_widget_remove_child_v1(entries_list_, row);
      (void)plainroot_ui_widget_destroy_subtree_v1(row);
    }
    rows_.clear();
    actions_.clear();

    std::string folded_query;
    if (!search_query_.empty()) {
      gchar *folded = g_utf8_casefold(search_query_.c_str(), -1);
      if (folded != nullptr) {
        folded_query.assign(folded);
        g_free(folded);
      }
    }

    std::vector<const detail::DirectoryEntry *> visible;
    visible.reserve(entries_.size());
    for (const detail::DirectoryEntry &entry : entries_) {
      if (!show_hidden_ && !entry.name.empty() && entry.name.front() == '.')
        continue;
      if (!matches_selected_filter(entry))
        continue;
      if (!folded_query.empty()) {
        const std::string shown = display_path(entry.name);
        gchar *folded = g_utf8_casefold(shown.c_str(), -1);
        const bool matches =
            folded != nullptr && std::string_view(folded).find(folded_query) !=
                                     std::string_view::npos;
        g_free(folded);
        if (!matches)
          continue;
      }
      visible.push_back(&entry);
    }
    std::stable_sort(visible.begin(), visible.end(),
                     [](const detail::DirectoryEntry *left,
                        const detail::DirectoryEntry *right) {
                       const bool left_directory =
                           left->kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY;
                       const bool right_directory =
                           right->kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY;
                       return left_directory && !right_directory;
                     });

    for (const detail::DirectoryEntry *entry : visible) {
      std::string label = display_path(entry->name);
      if (entry->kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY)
        label += "/";
      auto action = std::make_unique<EntryAction>();
      action->controller = this;
      action->path = join_path(current_path_, entry->name);
      action->kind = entry->kind;
      auto *row = plainroot_ui_list_item_create_v1(context_, 1.0F);
      plainroot_ui_widget *control = nullptr;
      if (allow_multiple_ &&
          entry->kind != PLAINROOT_CORE_FS_ENTRY_DIRECTORY) {
        const bool selected =
            std::find(selected_paths_.begin(), selected_paths_.end(),
                      action->path) != selected_paths_.end();
        control = plainroot_ui_checkbox_create_v1(
            context_, label.c_str(), static_cast<uint32_t>(label.size()),
            selected ? 1U : 0U, 1.0F, &Impl::selection_changed, action.get());
      } else {
        control = plainroot_ui_button_create_v1(
            context_, label.c_str(), static_cast<uint32_t>(label.size()), 1.0F,
            &Impl::entry_activated, action.get());
        if (control != nullptr &&
            plainroot_ui_button_set_style_v1(
                control, PLAINROOT_UI_BUTTON_STYLE_TEXT) !=
                PLAINROOT_UI_RESULT_OK)
          control = nullptr;
      }
      if (control == nullptr || row == nullptr || !add(row, control) ||
          !add(entries_list_, row))
        continue;
      rows_.push_back(row);
      actions_.push_back(std::move(action));
    }

    if (status_error_.empty() && selected_filter_has_unhandled_mime()) {
      set_status(tr("MIME type filters are not applied yet."));
    } else if (visible.empty() && status_error_.empty()) {
      set_status(search_query_.empty() ? tr("This folder is empty.")
                                       : tr("No matching files."));
    } else if (!visible.empty() && status_error_.empty()) {
      set_status("");
    }
    request_redraw();
  }

  bool matches_selected_filter(
      const detail::DirectoryEntry &entry) const {
    if (entry.kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY ||
        selected_filter_index_ < 0 ||
        static_cast<size_t>(selected_filter_index_) >= filters_.size())
      return true;

    const BrowserFilter &filter =
        filters_[static_cast<size_t>(selected_filter_index_)];
    bool has_glob = false;
    bool has_unhandled_mime = false;
    for (const BrowserFilterPattern &pattern : filter.patterns) {
      if (pattern.type == 0U) {
        has_glob = true;
        if (fnmatch(pattern.pattern.c_str(), entry.name.c_str(), 0) == 0)
          return true;
      } else if (pattern.type == 1U) {
        has_unhandled_mime = true;
      }
    }

    // Keep MIME patterns permissive until the chooser has a MIME database
    // implementation. The status message makes this limitation visible.
    return has_unhandled_mime || !has_glob;
  }

  bool selected_filter_has_unhandled_mime() const {
    if (selected_filter_index_ < 0 ||
        static_cast<size_t>(selected_filter_index_) >= filters_.size())
      return false;
    const BrowserFilter &filter =
        filters_[static_cast<size_t>(selected_filter_index_)];
    return std::any_of(filter.patterns.begin(), filter.patterns.end(),
                       [](const BrowserFilterPattern &pattern) {
                         return pattern.type == 1U;
                       });
  }

  static void set_label(plainroot_ui_widget *widget, const std::string &value) {
    if (widget == nullptr)
      return;
    (void)plainroot_ui_widget_set_text_v1(widget, value.c_str(),
                                          static_cast<uint32_t>(value.size()));
  }

  void set_status(const std::string &text) { set_label(status_label_, text); }

  void set_storage_status(const std::string &text) {
    if (storage_view_ != nullptr)
      storage_view_->set_status(text);
  }

  void request_redraw() {
    if (window_ != nullptr)
      (void)plainroot_toolkit_window_request_redraw(window_);
  }

  plainroot_toolkit_application *application_ = nullptr;
  plainroot_ui_context *context_ = nullptr;
  plainroot_toolkit_window *window_ = nullptr;
  CompletionFn completion_ = nullptr;
  void *completion_user_data_ = nullptr;
  std::string requested_title_;
  std::string accept_label_;
  detail::DirectoryLoader loader_;
  std::unique_ptr<detail::StorageLocationsView> storage_view_;
  bool storage_catalog_complete_ = false;
  bool storage_operation_pending_ = false;
  std::unique_ptr<detail::StorageBridge> storage_bridge_;
  bool ui_ready_ = false;
  bool select_directory_ = false;
  bool allow_multiple_ = false;
  bool save_file_ = false;
  bool finished_ = false;
  bool show_hidden_ = false;
  bool initializing_filter_dropdown_ = false;
  bool overwrite_dialog_open_ = false;
  bool overwrite_confirmed_ = false;
  int32_t selected_filter_index_ = -1;
  uint64_t generation_ = 0;
  std::string current_path_;
  std::string search_query_;
  std::string initial_filename_;
  std::string pending_save_path_;
  std::string selected_path_;
  std::vector<std::string> selected_paths_;
  std::vector<BrowserFilter> filters_;
  std::string status_error_;
  std::vector<detail::DirectoryEntry> entries_;
  std::vector<plainroot_ui_widget *> rows_;
  std::vector<std::unique_ptr<EntryAction>> actions_;
  plainroot_ui_widget *path_label_ = nullptr;
  plainroot_ui_widget *filename_field_ = nullptr;
  plainroot_ui_widget *search_field_ = nullptr;
  plainroot_ui_widget *hidden_switch_ = nullptr;
  plainroot_ui_widget *filter_dropdown_ = nullptr;
  plainroot_ui_widget *list_scroll_ = nullptr;
  plainroot_ui_widget *entries_list_ = nullptr;
  plainroot_ui_widget *status_label_ = nullptr;
  plainroot_ui_widget *selected_label_ = nullptr;
};

BrowserController::BrowserController(plainroot_toolkit_application *application,
                                     plainroot_ui_context *context,
                                     CompletionFn completion,
                                     void *completion_user_data)
    : impl_(std::make_unique<Impl>(application, context, completion,
                                  completion_user_data)) {}

BrowserController::~BrowserController() = default;

bool BrowserController::ready() const { return impl_ && impl_->ready(); }

void BrowserController::shutdown() {
  if (impl_)
    impl_->shutdown();
}

bool BrowserController::create_ui() { return impl_ && impl_->create_ui(); }

void BrowserController::set_window(plainroot_toolkit_window *window) {
  if (impl_)
    impl_->set_window(window);
}

void BrowserController::set_title(std::string title) {
  if (impl_)
    impl_->set_title(std::move(title));
}

void BrowserController::set_select_directory(bool select_directory) {
  if (impl_)
    impl_->set_select_directory(select_directory);
}

void BrowserController::set_allow_multiple(bool allow_multiple) {
  if (impl_)
    impl_->set_allow_multiple(allow_multiple);
}

void BrowserController::set_filters(std::vector<BrowserFilter> filters,
                                    int32_t selected_filter) {
  if (impl_)
    impl_->set_filters(std::move(filters), selected_filter);
}

void BrowserController::set_save_file(bool save_file) {
  if (impl_)
    impl_->set_save_file(save_file);
}

void BrowserController::set_initial_filename(std::string filename) {
  if (impl_)
    impl_->set_initial_filename(std::move(filename));
}

void BrowserController::set_accept_label(std::string label) {
  if (impl_)
    impl_->set_accept_label(std::move(label));
}

void BrowserController::navigate(std::string path) {
  if (impl_)
    impl_->navigate(std::move(path));
}

void BrowserController::cancel() {
  if (impl_)
    impl_->cancel();
}

} // namespace plainroot::filechooser
