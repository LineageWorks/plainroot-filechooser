#include "browser_controller.hpp"

#include "bookmarks.hpp"
#include "directory_breadcrumbs.hpp"
#include "directory_loader.hpp"
#include "directory_sort.hpp"
#include "mime_database.hpp"
#include "open_filename.hpp"
#include "portal_selection_mode.hpp"
#include "recent_files.hpp"
#include "storage_bridge.hpp"
#include "storage_locations_view.hpp"
#include "trace.hpp"

#include <plainroot/i18n/i18n.h>
#include <plainroot/ui/style.h>

#include <glib.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fnmatch.h>
#include <limits.h>
#include <limits>
#include <memory>
#include <cstring>
#include <sys/stat.h>
#include <string>
#include <string_view>
#include <unordered_map>
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

  bool uses_filename_field() const {
    return save_file_ || (!select_directory_ && !allow_multiple_);
  }

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

  struct BreadcrumbAction {
    Impl *controller = nullptr;
    std::string path;
  };

  struct ChoiceAction {
    Impl *controller = nullptr;
    size_t index = 0;
  };

  struct Place {
    std::string label;
    std::string path;
  };

  bool create_ui() {
    initialize_places();
    auto *root = plainroot_ui_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, 0.0F, 1.0F);
    auto *outer =
        plainroot_ui_vertical_scroll_area_create_v1(context_, 0, 0, 1.0F);
    auto *padding = plainroot_ui_padding_create_v1(
        context_, kPagePadding, kPagePadding, kPagePadding, kPagePadding, 1.0F);
    auto *page = plainroot_ui_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, kContentSpacing, 1.0F);
    const char *default_title =
        !save_files_.empty()
            ? "Save files"
            : (select_directory_ ? "Choose a folder"
                                 : (save_file_ ? "Save file"
                                               : (allow_multiple_
                                                      ? "Choose files"
                                                      : "Choose a file")));
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
    auto *location_actions = plainroot_ui_adaptive_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, PLAINROOT_UI_AXIS_HORIZONTAL,
        420.0F, 8.0F, 0.0F);
    auto *up = make_button("Up", &Impl::up_activated);
    auto *home = make_button("Home", &Impl::home_activated);
    recent_button_ = make_button("Recent", &Impl::recent_activated);
    const std::string places_hint = tr("Places");
    places_dropdown_ = plainroot_ui_dropdown_create_v1(
        context_, places_hint.c_str(),
        static_cast<uint32_t>(places_hint.size()), 0.0F,
        &Impl::place_changed, this);
    bookmark_button_ =
        make_button("Bookmark folder", &Impl::bookmark_activated);
    new_folder_button_ =
        make_button("New folder", &Impl::new_folder_activated);
    breadcrumb_scroll_ =
        plainroot_ui_horizontal_scroll_area_create_v1(context_, 0.0F, 1.0F);
    breadcrumb_strip_ = plainroot_ui_box_create_v1(
        context_, PLAINROOT_UI_AXIS_HORIZONTAL, 4.0F, 0.0F);
    if (breadcrumb_scroll_ != nullptr)
      (void)plainroot_ui_widget_set_text_direction_v1(breadcrumb_scroll_,
                                                      PLAINROOT_UI_TEXT_LTR);
    if (breadcrumb_strip_ != nullptr)
      (void)plainroot_ui_widget_set_text_direction_v1(breadcrumb_strip_,
                                                      PLAINROOT_UI_TEXT_LTR);

    plainroot_ui_widget *save_files_panel = nullptr;
    if (!save_files_.empty()) {
      save_files_panel = plainroot_ui_box_create_v1(
          context_, PLAINROOT_UI_AXIS_VERTICAL, 8.0F, 0.0F);
      auto *files_heading = make_label("Files to save", 0.0F);
      auto *files_note = make_label(
          "Existing or repeated names receive numbered variants.", 0.0F);
      auto *files_list = plainroot_ui_box_create_v1(
          context_, PLAINROOT_UI_AXIS_VERTICAL, 4.0F, 0.0F);
      if (save_files_panel == nullptr || files_heading == nullptr ||
          files_note == nullptr || files_list == nullptr ||
          !add(save_files_panel, files_heading) ||
          !add(save_files_panel, files_note) ||
          !add(save_files_panel, files_list))
        return false;
      for (const std::string &filename : save_files_) {
        const std::string display_name = display_path(filename);
        auto *filename_label = plainroot_ui_label_create_v1(
            context_, display_name.c_str(),
            static_cast<uint32_t>(display_name.size()),
            PLAINROOT_UI_COLOR_ON_SURFACE_V3, 0.0F);
        if (filename_label == nullptr ||
            plainroot_ui_widget_set_text_direction_v1(
                filename_label, PLAINROOT_UI_TEXT_LTR) !=
                PLAINROOT_UI_RESULT_OK ||
            !add(files_list, filename_label))
          return false;
      }
    }

    auto *controls = plainroot_ui_adaptive_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, PLAINROOT_UI_AXIS_HORIZONTAL,
        kExpandedControlsWidth, 12.0F, 0.0F);
    const bool show_filename_entry = uses_filename_field();
    auto *filename_row = show_filename_entry
                             ? plainroot_ui_adaptive_box_create_v1(
                                   context_, PLAINROOT_UI_AXIS_VERTICAL,
                                   PLAINROOT_UI_AXIS_HORIZONTAL,
                                   kExpandedControlsWidth, 8.0F, 0.0F)
                             : nullptr;
    auto *filename_label =
        show_filename_entry ? make_label("File name", 0.0F) : nullptr;
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
    const std::string sort_hint = tr("Sort by name");
    sort_dropdown_ = plainroot_ui_dropdown_create_v1(
        context_, sort_hint.c_str(), static_cast<uint32_t>(sort_hint.size()),
        0.0F, &Impl::sort_changed, this);
    if (show_filename_entry) {
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

    auto *new_folder_actions = plainroot_ui_box_create_v1(
        context_, PLAINROOT_UI_AXIS_HORIZONTAL, 8.0F, 0.0F);
    new_folder_form_ = plainroot_ui_adaptive_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, PLAINROOT_UI_AXIS_HORIZONTAL,
        560.0F, 8.0F, 0.0F);
    const std::string folder_name_hint = tr("Folder name");
    new_folder_field_ = plainroot_ui_text_field_create_v1(
        context_, "", 0, folder_name_hint.c_str(),
        static_cast<uint32_t>(folder_name_hint.size()), NAME_MAX, 1.0F,
        nullptr, nullptr);
    new_folder_create_button_ =
        make_button("Create", &Impl::create_folder_activated);
    new_folder_cancel_button_ =
        make_button("Cancel", &Impl::cancel_create_folder_activated);
    if (new_folder_create_button_ != nullptr)
      (void)plainroot_ui_button_set_style_v1(
          new_folder_create_button_, PLAINROOT_UI_BUTTON_STYLE_FILLED);
    if (new_folder_field_ != nullptr)
      (void)plainroot_ui_widget_set_text_direction_v1(
          new_folder_field_, PLAINROOT_UI_TEXT_LTR);
    if (new_folder_form_ != nullptr)
      (void)plainroot_ui_widget_set_visible_v1(new_folder_form_, 0U);

    list_scroll_ =
        plainroot_ui_vertical_scroll_area_create_v1(context_, 0, 0, 1.0F);
    entries_list_ = plainroot_ui_list_create_v1(context_, 1.0F);
    status_label_ = make_label("Loading folder…", 0.0F);
    auto *footer = plainroot_ui_adaptive_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, PLAINROOT_UI_AXIS_HORIZONTAL,
        kExpandedControlsWidth, 12.0F, 0.0F);
    auto *footer_padding = plainroot_ui_padding_create_v1(
        context_, kPagePadding, 8.0F, kPagePadding, kPagePadding, 0.0F);
    const std::string initial_selection_summary =
        allow_multiple_ ? selection_summary()
                        : tr(!save_files_.empty()
                                 ? "The selected folder will be used to save these files."
                                 : (select_directory_
                                        ? "The current folder will be returned."
                                        : (save_file_
                                               ? "Enter a name for the file to save."
                                               : "No file selected")));
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
            ? tr(!save_files_.empty()
                     ? "Save"
                     : (select_directory_ ? "Select folder"
                                          : (save_file_ ? "Save" : "Open")))
            : accept_label_,
        &Impl::accept_activated);
    if (select != nullptr)
      (void)plainroot_ui_button_set_style_v1(select,
                                             PLAINROOT_UI_BUTTON_STYLE_FILLED);

    if (root == nullptr || outer == nullptr || padding == nullptr ||
        page == nullptr || footer_padding == nullptr ||
        title == nullptr || location_row == nullptr ||
        location_actions == nullptr || up == nullptr || home == nullptr ||
        recent_button_ == nullptr ||
        places_dropdown_ == nullptr || bookmark_button_ == nullptr ||
        new_folder_button_ == nullptr || new_folder_form_ == nullptr ||
        new_folder_actions == nullptr || new_folder_field_ == nullptr ||
        new_folder_create_button_ == nullptr ||
        new_folder_cancel_button_ == nullptr ||
        storage_view_widget == nullptr || breadcrumb_scroll_ == nullptr ||
        breadcrumb_strip_ == nullptr || controls == nullptr ||
        search_field_ == nullptr || hidden_switch_ == nullptr ||
        sort_dropdown_ == nullptr ||
        (filters_.size() > 1U && filter_dropdown_ == nullptr) ||
        list_scroll_ == nullptr || entries_list_ == nullptr ||
        status_label_ == nullptr || footer == nullptr ||
        selected_label_ == nullptr || select == nullptr ||
        (!save_files_.empty() && save_files_panel == nullptr) ||
        (show_filename_entry &&
         (filename_row == nullptr || filename_label == nullptr ||
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

    const std::string ascending_label = tr("Name (ascending)");
    auto *ascending_option = plainroot_ui_list_box_option_create_v1(
        context_, ascending_label.c_str(),
        static_cast<uint32_t>(ascending_label.size()), 0, 0.0F);
    const std::string descending_label = tr("Name (descending)");
    auto *descending_option = plainroot_ui_list_box_option_create_v1(
        context_, descending_label.c_str(),
        static_cast<uint32_t>(descending_label.size()), 1, 0.0F);
    const std::string recent_sort_label = tr("Recently used");
    recent_sort_option_ = plainroot_ui_list_box_option_create_v1(
        context_, recent_sort_label.c_str(),
        static_cast<uint32_t>(recent_sort_label.size()), 2, 0.0F);
    if (!add(sort_dropdown_, ascending_option) ||
        !add(sort_dropdown_, descending_option) ||
        !add(sort_dropdown_, recent_sort_option_))
      return false;
    (void)plainroot_ui_widget_set_visible_v1(recent_sort_option_, 0U);
    initializing_sort_dropdown_ = true;
    const auto sort_result =
        plainroot_ui_dropdown_set_value_v1(sort_dropdown_, 0);
    initializing_sort_dropdown_ = false;
    if (sort_result != PLAINROOT_UI_RESULT_OK)
      return false;

    if (!populate_place_options())
      return false;
    refresh_bookmark_button();

    plainroot_ui_widget *choices_panel = nullptr;
    choice_widgets_.assign(choices_.size(), nullptr);
    if (!choices_.empty() || allow_read_only_) {
      choices_panel = plainroot_ui_box_create_v1(
          context_, PLAINROOT_UI_AXIS_VERTICAL, 8.0F, 0.0F);
      if (choices_panel == nullptr)
        return false;
      for (size_t index = 0; index < choices_.size(); ++index) {
        const BrowserChoice &choice = choices_[index];
        auto action = std::make_unique<ChoiceAction>();
        action->controller = this;
        action->index = index;
        plainroot_ui_widget *control = nullptr;
        plainroot_ui_widget *choice_row = nullptr;
        if (choice.options.empty()) {
          control = plainroot_ui_checkbox_create_v1(
              context_, choice.label.c_str(),
              static_cast<uint32_t>(choice.label.size()),
              choice_selections_[index].value == "true" ? 1U : 0U, 0.0F,
              &Impl::choice_changed, action.get());
        } else {
          choice_row = plainroot_ui_adaptive_box_create_v1(
              context_, PLAINROOT_UI_AXIS_VERTICAL,
              PLAINROOT_UI_AXIS_HORIZONTAL, kExpandedControlsWidth, 8.0F,
              0.0F);
          auto *choice_label = plainroot_ui_label_create_v1(
              context_, choice.label.c_str(),
              static_cast<uint32_t>(choice.label.size()),
              PLAINROOT_UI_COLOR_ON_SURFACE_V3, 0.0F);
          control = plainroot_ui_dropdown_create_v1(
              context_, choice.label.c_str(),
              static_cast<uint32_t>(choice.label.size()), 1.0F,
              &Impl::choice_changed, action.get());
          if (control != nullptr)
            (void)plainroot_ui_widget_set_accessible_name_v1(
                control, choice.label.c_str(),
                static_cast<uint32_t>(choice.label.size()));
          if (choice_row == nullptr || choice_label == nullptr ||
              control == nullptr)
            return false;
          for (size_t option_index = 0;
               option_index < choice.options.size(); ++option_index) {
            const BrowserChoiceOption &option = choice.options[option_index];
            auto *widget_option = plainroot_ui_list_box_option_create_v1(
                context_, option.label.c_str(),
                static_cast<uint32_t>(option.label.size()),
                static_cast<int32_t>(option_index), 0.0F);
            if (!add(control, widget_option))
              return false;
          }
          const auto selected = std::find_if(
              choice.options.begin(), choice.options.end(),
              [this, index](const BrowserChoiceOption &option) {
                return option.id == choice_selections_[index].value;
              });
          if (selected == choice.options.end())
            return false;
          const int32_t selected_index = static_cast<int32_t>(
              std::distance(choice.options.begin(), selected));
          initializing_choice_controls_ = true;
          const auto set_result =
              plainroot_ui_dropdown_set_value_v1(control, selected_index);
          initializing_choice_controls_ = false;
          if (set_result != PLAINROOT_UI_RESULT_OK ||
              !add(choice_row, choice_label) || !add(choice_row, control))
            return false;
        }
        if (control == nullptr ||
            !add(choices_panel,
                 choice_row == nullptr ? control : choice_row))
          return false;
        choice_widgets_[index] = control;
        choice_actions_.push_back(std::move(action));
      }
      if (allow_read_only_) {
        const std::string label = tr("Open files read-only");
        read_only_checkbox_ = plainroot_ui_checkbox_create_v1(
            context_, label.c_str(), static_cast<uint32_t>(label.size()), 0U,
            0.0F, &Impl::read_only_changed, this);
        if (read_only_checkbox_ == nullptr ||
            !add(choices_panel, read_only_checkbox_))
          return false;
      }
    }

    if (!add(root, outer) || !add(root, footer_padding) ||
        !add(outer, padding) || !add(padding, page) || !add(page, title) ||
        !add(page, location_row) ||
        (save_files_panel != nullptr && !add(page, save_files_panel)) ||
        !add(location_row, location_actions) ||
        !add(location_actions, up) || !add(location_actions, home) ||
        !add(location_actions, recent_button_) ||
        !add(location_actions, places_dropdown_) ||
        !add(location_actions, bookmark_button_) ||
        !add(location_actions, new_folder_button_) ||
        !add(page, new_folder_form_) ||
        !add(new_folder_form_, new_folder_field_) ||
        !add(new_folder_form_, new_folder_actions) ||
        !add(new_folder_actions, new_folder_create_button_) ||
        !add(new_folder_actions, new_folder_cancel_button_) ||
        (show_filename_entry &&
         (!add(page, filename_row) || !add(filename_row, filename_label) ||
          !add(filename_row, filename_field_))) ||
        !add(location_row, breadcrumb_scroll_) ||
        !add(breadcrumb_scroll_, breadcrumb_strip_) || !add(page, controls) ||
        !add(controls, search_field_) || !add(controls, hidden_switch_) ||
        !add(controls, sort_dropdown_) ||
        (filter_dropdown_ != nullptr && !add(controls, filter_dropdown_)) ||
        (choices_panel != nullptr && !add(page, choices_panel)) ||
        !add(page, storage_view_widget) || !add(page, list_scroll_) ||
        !add(list_scroll_, entries_list_) || !add(page, status_label_) ||
        !add(footer_padding, footer) || !add(footer, selected_label_) ||
        !add(footer, select) ||
        plainroot_ui_context_set_root_v1(context_, root) !=
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

  void set_choices(std::vector<BrowserChoice> choices) {
    choices_ = std::move(choices);
    choice_selections_.clear();
    choice_selections_.reserve(choices_.size());
    for (const BrowserChoice &choice : choices_) {
      std::string value = choice.initial_selection;
      if (choice.options.empty()) {
        if (value.empty())
          value = "false";
      } else if (value.empty()) {
        value = choice.options.front().id;
      }
      choice_selections_.push_back({choice.id, std::move(value)});
    }
  }

  void set_allow_read_only(bool allow_read_only) {
    allow_read_only_ = allow_read_only;
  }

  void set_save_file(bool save_file) { save_file_ = save_file; }

  void set_save_files(std::vector<std::string> filenames) {
    save_files_ = std::move(filenames);
  }

  void set_initial_filename(std::string filename) {
    initial_filename_ = std::move(filename);
    if (filename_field_ != nullptr)
      set_label(filename_field_, initial_filename_);
  }

  void set_accept_label(std::string label) { accept_label_ = std::move(label); }

  void cancel() {
    if (!finished_ && completion_ != nullptr) {
      finished_ = true;
      completion_(completion_user_data_, false, writable_, {}, nullptr, {});
    }
  }

  void navigate(std::string path) {
    if (path.empty() || path.front() != '/') {
      set_status(tr("Choose an absolute directory path."));
      return;
    }
    ++folder_create_request_id_;
    folder_creation_pending_ = false;
    set_folder_creation_controls_enabled(true);
    if (new_folder_field_ != nullptr)
      (void)plainroot_ui_widget_set_text_v1(new_folder_field_, "", 0U);
    if (new_folder_form_ != nullptr)
      (void)plainroot_ui_widget_set_visible_v1(new_folder_form_, 0U);
    current_path_ = std::move(path);
    recent_mode_ = false;
    sort_order_ = detail::DirectorySortOrder::name_ascending;
    if (recent_sort_option_ != nullptr)
      (void)plainroot_ui_widget_set_visible_v1(recent_sort_option_, 0U);
    initializing_sort_dropdown_ = true;
    (void)plainroot_ui_dropdown_set_value_v1(sort_dropdown_, 0);
    initializing_sort_dropdown_ = false;
    refresh_bookmark_button();
    new_folder_created_ = false;
    render_breadcrumbs();
    if (!save_file_ && filename_field_ != nullptr) {
      initial_filename_.clear();
      set_label(filename_field_, initial_filename_);
    }
    request_directory_load(false);
  }

private:
  void request_directory_load(bool preserve_selection) {
    if (!preserve_selection) {
      selected_path_.clear();
      selected_paths_.clear();
    }
    ++generation_;
    entries_.clear();
    directory_loading_ = true;
    entries_mime_classified_ = false;
    mime_database_available_ = false;
    if (!preserve_selection) {
      if (allow_multiple_) {
        update_selection_summary();
      } else {
        set_label(selected_label_,
                  tr(!save_files_.empty()
                         ? "The selected folder will be used to save these files."
                         : (select_directory_
                                ? "The current folder will be returned."
                                : (save_file_
                                       ? "Enter a name for the file to save."
                                       : "No file selected"))));
      }
    }
    if (recent_mode_) {
      set_status(tr("Loading recent files…"));
      const gchar *data_directory = g_get_user_data_dir();
      const std::string xbel = std::string(data_directory == nullptr ? "" : data_directory) + "/recently-used.xbel";
      loader_.request_recent(xbel, generation_, selected_filter_has_mime());
    } else {
      set_status(tr("Loading folder…"));
      loader_.request(current_path_, generation_, selected_filter_has_mime());
    }
    schedule_render();
    request_redraw();
  }

  void inspect_symlink(const std::string &path) {
    set_status(tr("Checking link…"));
    loader_.inspect_symlink(path, generation_);
    request_redraw();
  }

  void inspect_symlink_selection(const std::string &path) {
    const uint64_t request_id = ++symlink_selection_request_id_;
    pending_symlink_selections_.emplace(request_id, path);
    loader_.inspect_symlink(path, generation_, true, request_id);
    set_status(tr("Checking link…"));
    request_redraw();
  }

  void set_folder_creation_controls_enabled(bool enabled) {
    const uint32_t value = enabled ? 1U : 0U;
    if (new_folder_button_ != nullptr)
      (void)plainroot_ui_widget_set_enabled_v1(new_folder_button_, value);
    if (new_folder_field_ != nullptr)
      (void)plainroot_ui_widget_set_enabled_v1(new_folder_field_, value);
    if (new_folder_create_button_ != nullptr)
      (void)plainroot_ui_widget_set_enabled_v1(new_folder_create_button_, value);
    if (new_folder_cancel_button_ != nullptr)
      (void)plainroot_ui_widget_set_enabled_v1(new_folder_cancel_button_, value);
  }

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
    if (self.recent_mode_) {
      self.navigate(self.current_path_);
      return;
    }
    self.navigate(parent_path(self.current_path_));
  }

  static void home_activated(void *user_data, plainroot_ui_widget *) {
    auto &self = *static_cast<Impl *>(user_data);
    const char *home = std::getenv("HOME");
    self.navigate(home != nullptr && home[0] == '/' ? home : "/");
  }

  static void recent_activated(void *user_data, plainroot_ui_widget *) {
    auto &self = *static_cast<Impl *>(user_data);
    self.recent_mode_ = true;
    self.sort_order_ = detail::DirectorySortOrder::recent_first;
    if (self.recent_sort_option_ != nullptr)
      (void)plainroot_ui_widget_set_visible_v1(self.recent_sort_option_, 1U);
    self.initializing_sort_dropdown_ = true;
    (void)plainroot_ui_dropdown_set_value_v1(self.sort_dropdown_, 2);
    self.initializing_sort_dropdown_ = false;
    self.render_breadcrumbs();
    self.request_directory_load(false);
  }

  void initialize_places() {
    bookmarks_available_ = detail::read_gtk_bookmarks(bookmarks_);
    rebuild_places_model();
  }

  void rebuild_places_model() {
    places_.clear();
    const struct {
      const char *label;
      GUserDirectory directory;
    } user_directories[] = {
        {"Desktop", G_USER_DIRECTORY_DESKTOP},
        {"Documents", G_USER_DIRECTORY_DOCUMENTS},
        {"Downloads", G_USER_DIRECTORY_DOWNLOAD},
        {"Music", G_USER_DIRECTORY_MUSIC},
        {"Pictures", G_USER_DIRECTORY_PICTURES},
        {"Videos", G_USER_DIRECTORY_VIDEOS},
    };

    for (const auto &entry : user_directories) {
      const char *path = g_get_user_special_dir(entry.directory);
      if (path == nullptr || path[0] != '/' ||
          !g_file_test(path, G_FILE_TEST_IS_DIR))
        continue;
      const bool already_added = std::any_of(
          places_.begin(), places_.end(), [path](const Place &place) {
            return place.path == path;
          });
      if (!already_added)
        places_.push_back({tr(entry.label), path});
    }

    for (const detail::Bookmark &bookmark : bookmarks_) {
      if (bookmark.path.empty())
        continue;
      const bool already_added = std::any_of(
          places_.begin(), places_.end(), [&bookmark](const Place &place) {
            return detail::canonical_local_path(place.path) == bookmark.path;
          });
      if (!already_added)
        places_.push_back(
            {detail::bookmark_display_name(bookmark), bookmark.path});
    }
  }

  bool populate_place_options() {
    if (places_dropdown_ == nullptr)
      return false;
    for (plainroot_ui_widget *option : place_option_widgets_) {
      (void)plainroot_ui_widget_remove_child_v1(places_dropdown_, option);
      (void)plainroot_ui_widget_destroy_subtree_v1(option);
    }
    place_option_widgets_.clear();

    const auto add_option = [this](const std::string &label, int32_t value) {
      auto *option = plainroot_ui_list_box_option_create_v1(
          context_, label.c_str(), static_cast<uint32_t>(label.size()), value,
          0.0F);
      if (option == nullptr || !add(places_dropdown_, option))
        return false;
      place_option_widgets_.push_back(option);
      return true;
    };

    if (places_.empty()) {
      if (!add_option(tr("No places available"), 0) ||
          plainroot_ui_widget_set_enabled_v1(places_dropdown_, 0U) !=
              PLAINROOT_UI_RESULT_OK)
        return false;
    } else {
      for (size_t index = 0; index < places_.size(); ++index) {
        if (index > static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
            !add_option(places_[index].label, static_cast<int32_t>(index)))
          return false;
      }
      if (plainroot_ui_widget_set_enabled_v1(places_dropdown_, 1U) !=
          PLAINROOT_UI_RESULT_OK)
        return false;
    }

    size_t selected_index = 0;
    if (!current_path_.empty()) {
      const std::string current =
          detail::canonical_local_path(current_path_);
      const auto current_place = std::find_if(
          places_.begin(), places_.end(), [&current](const Place &place) {
            return detail::canonical_local_path(place.path) == current;
          });
      if (current_place != places_.end())
        selected_index = static_cast<size_t>(
            std::distance(places_.begin(), current_place));
    }
    initializing_places_dropdown_ = true;
    const auto result = plainroot_ui_dropdown_set_value_v1(
        places_dropdown_, static_cast<int32_t>(selected_index));
    initializing_places_dropdown_ = false;
    return result == PLAINROOT_UI_RESULT_OK;
  }

  bool current_path_is_bookmarked() const {
    if (current_path_.empty())
      return false;
    const std::string current =
        detail::canonical_local_path(current_path_);
    return !current.empty() &&
           std::any_of(bookmarks_.begin(), bookmarks_.end(),
                       [&current](const detail::Bookmark &bookmark) {
                         return bookmark.path == current;
                       });
  }

  void refresh_bookmark_button() {
    if (bookmark_button_ == nullptr)
      return;
    const bool bookmarked = current_path_is_bookmarked();
    const std::string label =
        tr(bookmarked ? "Remove bookmark" : "Bookmark folder");
    set_label(bookmark_button_, label);
    (void)plainroot_ui_widget_set_accessible_name_v1(
        bookmark_button_, label.c_str(), static_cast<uint32_t>(label.size()));
    const bool enabled = bookmarks_available_ && !current_path_.empty();
    (void)plainroot_ui_widget_set_enabled_v1(bookmark_button_,
                                             enabled ? 1U : 0U);
  }

  static void bookmark_activated(void *user_data, plainroot_ui_widget *) {
    auto &self = *static_cast<Impl *>(user_data);
    if (!self.bookmarks_available_ || self.current_path_.empty()) {
      self.set_status(tr("Unable to save bookmarks."));
      self.request_redraw();
      return;
    }

    const std::string current =
        detail::canonical_local_path(self.current_path_);
    if (current.empty()) {
      self.set_status(tr("Unable to save bookmarks."));
      self.request_redraw();
      return;
    }

    std::vector<detail::Bookmark> updated = self.bookmarks_;
    const size_t original_size = updated.size();
    std::erase_if(updated, [&current](const detail::Bookmark &bookmark) {
      return bookmark.path == current;
    });
    const bool removing = original_size != updated.size();
    if (!removing) {
      detail::Bookmark bookmark;
      if (!detail::make_local_bookmark(current, bookmark)) {
        self.set_status(tr("Unable to save bookmarks."));
        self.request_redraw();
        return;
      }
      updated.push_back(std::move(bookmark));
    }

    if (!detail::write_gtk_bookmarks(updated)) {
      self.set_status(tr("Unable to save bookmarks."));
      self.request_redraw();
      return;
    }

    self.bookmarks_ = std::move(updated);
    self.rebuild_places_model();
    if (!self.populate_place_options()) {
      self.set_status(tr("Unable to save bookmarks."));
      self.request_redraw();
      return;
    }
    self.refresh_bookmark_button();
    self.set_status(tr(removing ? "Bookmark removed." : "Bookmark added."));
    self.request_redraw();
  }

  static void place_changed(void *user_data, plainroot_ui_widget *widget) {
    auto &self = *static_cast<Impl *>(user_data);
    if (self.initializing_places_dropdown_)
      return;
    int32_t selected = -1;
    uint32_t has_value = 0;
    if (plainroot_ui_dropdown_get_value_v1(widget, &selected, &has_value) !=
            PLAINROOT_UI_RESULT_OK ||
        has_value == 0U || selected < 0 ||
        static_cast<size_t>(selected) >= self.places_.size())
      return;
    self.navigate(self.places_[static_cast<size_t>(selected)].path);
  }

  static void new_folder_activated(void *user_data, plainroot_ui_widget *) {
    auto &self = *static_cast<Impl *>(user_data);
    if (self.folder_creation_pending_)
      return;
    uint8_t visible = 0;
    if (self.new_folder_form_ == nullptr ||
        plainroot_ui_widget_get_visible_v1(self.new_folder_form_, &visible) !=
            PLAINROOT_UI_RESULT_OK)
      return;
    if (plainroot_ui_widget_set_visible_v1(self.new_folder_form_,
                                           visible == 0U ? 1U : 0U) !=
        PLAINROOT_UI_RESULT_OK)
      return;
    if (visible == 0U)
      self.set_status("");
    self.request_redraw();
  }

  static void cancel_create_folder_activated(void *user_data,
                                             plainroot_ui_widget *) {
    auto &self = *static_cast<Impl *>(user_data);
    if (self.folder_creation_pending_)
      return;
    ++self.folder_create_request_id_;
    self.folder_creation_pending_ = false;
    if (self.new_folder_field_ != nullptr)
      (void)plainroot_ui_widget_set_text_v1(self.new_folder_field_, "", 0U);
    if (self.new_folder_form_ != nullptr)
      (void)plainroot_ui_widget_set_visible_v1(self.new_folder_form_, 0U);
    self.set_status("");
    self.request_redraw();
  }

  static void create_folder_activated(void *user_data, plainroot_ui_widget *) {
    auto &self = *static_cast<Impl *>(user_data);
    if (self.folder_creation_pending_)
      return;
    const char *value = nullptr;
    uint32_t length = 0;
    if (self.new_folder_field_ == nullptr ||
        plainroot_ui_text_field_get_text_v1(self.new_folder_field_, &value,
                                            &length) !=
            PLAINROOT_UI_RESULT_OK ||
        (value == nullptr && length != 0U)) {
      self.set_status(tr("Unable to create the folder."));
      self.request_redraw();
      return;
    }

    const std::string name(value == nullptr ? "" : value, length);
    if (name.empty()) {
      self.set_status(tr("Enter a folder name."));
      self.request_redraw();
      return;
    }
    if (name == "." || name == ".." || name.find('/') != std::string::npos) {
      self.set_status(
          tr("Use a folder name without slashes or dot components."));
      self.request_redraw();
      return;
    }
    if (name.size() > NAME_MAX) {
      self.set_status(tr("Folder name is too long."));
      self.request_redraw();
      return;
    }

    const std::string target = join_path(self.current_path_, name);
    self.folder_creation_pending_ = true;
    self.set_folder_creation_controls_enabled(false);
    const uint64_t request_id = ++self.folder_create_request_id_;
    self.loader_.create_directory(target, self.generation_, request_id);
    self.set_status(tr("Creating folder…"));
    self.request_redraw();
  }

  static const char *create_folder_error_message(
      plainroot_core_fs_result_v1 result) {
    if (result == PLAINROOT_CORE_FS_ALREADY_EXISTS)
      return "An item with this name already exists.";
    if (result == PLAINROOT_CORE_FS_PERMISSION_DENIED)
      return "You do not have permission to create a folder here.";
    if (result == PLAINROOT_CORE_FS_READ_ONLY)
      return "This location is read-only.";
    if (result == PLAINROOT_CORE_FS_NO_SPACE)
      return "There is not enough space to create a folder.";
    if (result == PLAINROOT_CORE_FS_NOT_FOUND ||
        result == PLAINROOT_CORE_FS_NOT_DIRECTORY)
      return "The current folder is no longer available.";
    return "Unable to create the folder.";
  }

  void apply_create_folder_result(detail::DirectoryResult result) {
    if (result.generation != generation_ ||
        result.request_id != folder_create_request_id_ ||
        !folder_creation_pending_)
      return;
    folder_creation_pending_ = false;
    set_folder_creation_controls_enabled(true);
    if (result.result != PLAINROOT_CORE_FS_OK) {
      set_status(tr(create_folder_error_message(result.result)));
      request_redraw();
      return;
    }

    (void)plainroot_ui_widget_set_text_v1(new_folder_field_, "", 0U);
    (void)plainroot_ui_widget_set_visible_v1(new_folder_form_, 0U);
    new_folder_created_ = true;
    request_directory_load(true);
    set_status(tr("Folder created."));
    request_redraw();
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
    if (self.selected_filter_has_mime() && !self.entries_mime_classified_) {
      self.request_directory_load(true);
      return;
    }
    self.schedule_render();
    self.request_redraw();
  }

  static void sort_changed(void *user_data, plainroot_ui_widget *widget) {
    auto &self = *static_cast<Impl *>(user_data);
    if (self.initializing_sort_dropdown_)
      return;
    int32_t selected = -1;
    uint32_t has_value = 0;
    if (plainroot_ui_dropdown_get_value_v1(widget, &selected, &has_value) !=
            PLAINROOT_UI_RESULT_OK ||
        has_value == 0 || selected < 0 || selected > 2 ||
        (selected == 2 && !self.recent_mode_))
      return;
    self.sort_order_ = selected == 0
                           ? detail::DirectorySortOrder::name_ascending
                           : (selected == 1
                                  ? detail::DirectorySortOrder::name_descending
                                  : detail::DirectorySortOrder::recent_first);
    self.schedule_render();
  }

  static void choice_changed(void *user_data, plainroot_ui_widget *) {
    auto &action = *static_cast<ChoiceAction *>(user_data);
    Impl &self = *action.controller;
    if (self.initializing_choice_controls_ ||
        action.index >= self.choice_selections_.size())
      return;
    std::string value;
    if (self.read_choice_value(action.index, value))
      self.choice_selections_[action.index].value = std::move(value);
  }

  static void read_only_changed(void *user_data, plainroot_ui_widget *widget) {
    auto &self = *static_cast<Impl *>(user_data);
    uint32_t checked = 0U;
    if (plainroot_ui_checkbox_get_checked_v1(widget, &checked) ==
        PLAINROOT_UI_RESULT_OK)
      self.writable_ = checked == 0U;
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
    if (!self.save_file_ && self.uses_filename_field()) {
      self.selected_path_.clear();
      if (self.initial_filename_.empty()) {
        set_label(self.selected_label_, tr("No file selected"));
        self.set_status("");
      } else {
        set_label(self.selected_label_,
                  join_path(display_path(self.current_path_),
                            self.initial_filename_));
        self.set_status(tr("File name entered. Choose Open to check it."));
      }
      self.request_redraw();
    }
  }

  static void entry_activated(void *user_data, plainroot_ui_widget *) {
    auto &action = *static_cast<EntryAction *>(user_data);
    if (action.kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY) {
      action.controller->navigate(action.path);
      return;
    }
    if (action.kind == PLAINROOT_CORE_FS_ENTRY_SYMLINK) {
      action.controller->inspect_symlink(action.path);
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
    if (action.controller->filename_field_ != nullptr) {
      const std::string filename =
          parent_path(action.path) == "/"
              ? action.path.substr(1)
              : action.path.substr(action.path.find_last_of('/') + 1U);
      action.controller->initial_filename_ = display_path(filename);
      set_label(action.controller->filename_field_,
                action.controller->initial_filename_);
    }
    action.controller->selected_path_ = action.path;
    set_label(action.controller->selected_label_, display_path(action.path));
    action.controller->set_status(
        tr("File selected. Choose Open to return it."));
    action.controller->request_redraw();
  }

  static void breadcrumb_activated(void *user_data, plainroot_ui_widget *) {
    const auto &action = *static_cast<BreadcrumbAction *>(user_data);
    auto *controller = action.controller;
    std::string path = action.path;
    controller->navigate(std::move(path));
  }

  static void selection_changed(void *user_data, plainroot_ui_widget *widget) {
    auto &action = *static_cast<EntryAction *>(user_data);
    if (action.controller == nullptr || widget == nullptr)
      return;
    if (action.controller->setting_symlink_selection_)
      return;
    uint32_t checked = 0;
    if (plainroot_ui_checkbox_get_checked_v1(widget, &checked) !=
        PLAINROOT_UI_RESULT_OK)
      return;
    if (action.kind == PLAINROOT_CORE_FS_ENTRY_SYMLINK && checked != 0U) {
      action.controller->setting_symlink_selection_ = true;
      (void)plainroot_ui_checkbox_set_checked_v1(widget, 0U);
      action.controller->setting_symlink_selection_ = false;
      (void)plainroot_ui_widget_set_enabled_v1(widget, 0U);
      action.controller->inspect_symlink_selection(action.path);
      return;
    }
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
    detail::trace(
        "accept-activated save=%u save-files=%u multiple=%u directory=%u",
        self.save_file_ ? 1U : 0U, self.save_files_.empty() ? 0U : 1U,
        self.allow_multiple_ ? 1U : 0U,
        self.select_directory_ ? 1U : 0U);
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
    if (!self.select_directory_) {
      self.accept_open_file();
      return;
    }
    self.complete_selection({self.current_path_});
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

  void accept_open_file() {
    if (!selected_path_.empty()) {
      complete_selection({selected_path_});
      return;
    }

    const auto resolution =
        detail::resolve_open_filename(current_path_, initial_filename_);
    switch (resolution.status) {
    case detail::OpenFilenameStatus::valid:
      complete_selection({resolution.path});
      return;
    case detail::OpenFilenameStatus::directory:
      navigate(resolution.path);
      return;
    case detail::OpenFilenameStatus::empty:
      set_status(tr("Enter a file name or select a file."));
      break;
    case detail::OpenFilenameStatus::invalid_component:
      set_status(tr("Enter a file name without a folder path."));
      break;
    case detail::OpenFilenameStatus::too_long:
      set_status(tr("The file name is too long."));
      break;
    case detail::OpenFilenameStatus::invalid_encoding:
      set_status(tr("This file name cannot be represented on this system."));
      break;
    case detail::OpenFilenameStatus::not_found:
      set_status(tr("No file with that name exists in this folder."));
      break;
    case detail::OpenFilenameStatus::io_error:
      set_status(tr("Unable to check the selected file."));
      break;
    }
    request_redraw();
  }

  void render_breadcrumbs() {
    if (recent_mode_) {
      for (plainroot_ui_widget *widget : breadcrumb_widgets_) {
        (void)plainroot_ui_widget_remove_child_v1(breadcrumb_strip_, widget);
        (void)plainroot_ui_widget_destroy_subtree_v1(widget);
      }
      breadcrumb_widgets_.clear();
      breadcrumb_actions_.clear();
      auto *label = make_label("Recent", 0.0F);
      if (label != nullptr && add(breadcrumb_strip_, label))
        breadcrumb_widgets_.push_back(label);
      return;
    }
    if (breadcrumb_strip_ == nullptr)
      return;

    for (plainroot_ui_widget *child : breadcrumb_widgets_) {
      (void)plainroot_ui_widget_remove_child_v1(breadcrumb_strip_, child);
      (void)plainroot_ui_widget_destroy_subtree_v1(child);
    }
    breadcrumb_widgets_.clear();
    breadcrumb_actions_.clear();

    const auto breadcrumbs = detail::directory_breadcrumbs(current_path_);
    if (breadcrumbs.empty())
      return;
    breadcrumb_widgets_.reserve(breadcrumbs.size() * 2U - 1U);
    breadcrumb_actions_.reserve(breadcrumbs.size() - 1U);

    const auto add_widget = [this](plainroot_ui_widget *widget) {
      if (widget == nullptr ||
          plainroot_ui_widget_add_child_v1(breadcrumb_strip_, widget) !=
              PLAINROOT_UI_RESULT_OK) {
        if (widget != nullptr)
          (void)plainroot_ui_widget_destroy_subtree_v1(widget);
        return false;
      }
      breadcrumb_widgets_.push_back(widget);
      return true;
    };
    const auto add_separator = [this, &add_widget]() {
      constexpr std::string_view separator = "›";
      auto *widget = plainroot_ui_label_create_v1(
          context_, separator.data(), static_cast<uint32_t>(separator.size()),
          PLAINROOT_UI_COLOR_ON_SURFACE_VARIANT_V3, 0.0F);
      if (widget != nullptr)
        (void)plainroot_ui_widget_set_text_direction_v1(widget,
                                                        PLAINROOT_UI_TEXT_LTR);
      return add_widget(widget);
    };
    const auto add_ancestor = [this, &add_widget](const std::string &label,
                                                  const std::string &path) {
      auto action = std::make_unique<BreadcrumbAction>();
      action->controller = this;
      action->path = path;
      auto *button = plainroot_ui_button_create_v1(
          context_, label.c_str(), static_cast<uint32_t>(label.size()), 0.0F,
          &Impl::breadcrumb_activated, action.get());
      if (button == nullptr || plainroot_ui_button_set_style_v1(
                                   button, PLAINROOT_UI_BUTTON_STYLE_TEXT) !=
                                   PLAINROOT_UI_RESULT_OK) {
        if (button != nullptr)
          (void)plainroot_ui_widget_destroy_subtree_v1(button);
        return false;
      }
      (void)plainroot_ui_widget_set_text_direction_v1(button,
                                                      PLAINROOT_UI_TEXT_LTR);
      if (!add_widget(button))
        return false;
      breadcrumb_actions_.push_back(std::move(action));
      return true;
    };
    const auto add_current = [this, &add_widget](const std::string &label) {
      auto *widget = plainroot_ui_label_create_v1(
          context_, label.c_str(), static_cast<uint32_t>(label.size()),
          PLAINROOT_UI_COLOR_ON_SURFACE_V3, 0.0F);
      if (widget != nullptr)
        (void)plainroot_ui_widget_set_text_direction_v1(widget,
                                                        PLAINROOT_UI_TEXT_LTR);
      return add_widget(widget);
    };

    for (size_t index = 0; index < breadcrumbs.size(); ++index) {
      if (index != 0U && !add_separator())
        break;
      const detail::DirectoryBreadcrumb &breadcrumb = breadcrumbs[index];
      const std::string label = display_path(breadcrumb.name);
      const bool added = breadcrumb.current
                             ? add_current(label)
                             : add_ancestor(label, breadcrumb.path);
      if (!added)
        break;
    }

    // The active folder stays visible when a deep path overflows the viewport.
    // Layout clamps this pending offset to the new content extent.
    (void)plainroot_ui_scroll_area_set_offset_v1(
        breadcrumb_scroll_, std::numeric_limits<float>::max(), 0.0F);
    request_redraw();
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
    const char *data_directory = g_get_user_data_dir();
    loader_.record_recent(
        paths, std::string(data_directory == nullptr ? "" : data_directory) +
                   "/recently-used.xbel",
        "Plainroot FileChooser");
    detail::trace("selection-complete paths=%zu", paths.size());
    finished_ = true;
    if (completion_ != nullptr) {
      const BrowserFilter *current_filter =
          selected_filter_index_ >= 0 &&
                  static_cast<size_t>(selected_filter_index_) < filters_.size()
              ? &filters_[static_cast<size_t>(selected_filter_index_)]
              : nullptr;
      std::vector<BrowserChoiceSelection> selected_choices;
      if (!collect_choice_selections(selected_choices)) {
        set_status(tr("Unable to read the selected options."));
        request_redraw();
        finished_ = false;
        return;
      }
      completion_(completion_user_data_, true, writable_, paths, current_filter,
                  selected_choices);
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
    if (result.operation ==
        detail::DirectoryResult::Operation::CreateDirectory) {
      apply_create_folder_result(std::move(result));
      return;
    }
    if (result.operation ==
        detail::DirectoryResult::Operation::InspectSymlink) {
      if (result.selection_probe) {
        const auto pending =
            pending_symlink_selections_.find(result.request_id);
        if (pending == pending_symlink_selections_.end())
          return;
        const std::string selected_path = std::move(pending->second);
        pending_symlink_selections_.erase(pending);
        if (result.generation != generation_)
          return;
        const bool correct_target_kind =
            detail::symlink_target_matches_selection(select_directory_,
                                                     result.inspected_kind);
        if (result.result == PLAINROOT_CORE_FS_OK && correct_target_kind) {
          const auto found = std::find(selected_paths_.begin(),
                                       selected_paths_.end(), selected_path);
          if (found == selected_paths_.end())
            selected_paths_.push_back(selected_path);
          update_selection_summary();
          status_error_.clear();
          set_status("");
        } else if (result.result != PLAINROOT_CORE_FS_OK) {
          set_status(tr("This item is no longer available."));
        } else {
          set_status(tr("This item cannot be selected here."));
        }
        schedule_render();
        request_redraw();
        return;
      }
      if (result.generation != generation_)
        return;
      if (result.result == PLAINROOT_CORE_FS_OK &&
          (result.inspected_kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY ||
           result.inspected_kind == PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE)) {
        EntryAction action{this, std::move(result.path),
                           result.inspected_kind};
        entry_activated(&action, nullptr);
      } else {
        set_status(tr("This item is no longer available."));
      }
      request_redraw();
      return;
    }
    if (result.generation != generation_)
      return;
    directory_loading_ = false;
    if (result.result == PLAINROOT_CORE_FS_OK) {
      entries_ = std::move(result.entries);
      entries_mime_classified_ = result.mime_classified;
      mime_database_available_ = result.mime_database_available;
      status_error_.clear();
      if (new_folder_created_) {
        set_status(tr("Folder created."));
        new_folder_created_ = false;
      } else {
        set_status(entries_.empty()
                       ? tr(result.operation ==
                                    detail::DirectoryResult::Operation::RecentListing
                                ? "No recent files."
                                : "This folder is empty.")
                       : "");
      }
    } else {
      new_folder_created_ = false;
      entries_.clear();
      if (result.native_code == 0) {
        const bool recent = result.operation ==
                            detail::DirectoryResult::Operation::RecentListing;
        status_error_ = tr(recent ? "Unable to read recent files."
                                  : "Unable to read this folder.");
      } else {
        gchar *detail =
            g_strdup_printf(tr(result.operation ==
                                        detail::DirectoryResult::Operation::RecentListing
                                    ? "Unable to read recent files: %s"
                                    : "Unable to read this folder: %s").c_str(),
                            g_strerror(result.native_code));
        status_error_ =
            detail == nullptr
                ? tr(result.operation ==
                             detail::DirectoryResult::Operation::RecentListing
                         ? "Unable to read recent files."
                         : "Unable to read this folder.")
                : detail;
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
    case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_MOUNT_FAILED:
      return "The network location could not be mounted. Check the server, host key, login, and FUSE availability.";
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
        const std::string shown = display_path(
            entry.path.empty() ? entry.name : entry.name + " " + entry.path);
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
    visible = detail::sort_directory_entries(visible, sort_order_);

    for (const detail::DirectoryEntry *entry : visible) {
      std::string label = display_path(entry->name);
      if (entry->kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY)
        label += "/";
      auto action = std::make_unique<EntryAction>();
      action->controller = this;
      action->path = entry->path.empty() ? join_path(current_path_, entry->name)
                                         : entry->path;
      action->kind = entry->kind;
      auto *row = plainroot_ui_list_item_create_v1(context_, 1.0F);
      auto *recent_content = entry->path.empty()
                                 ? nullptr
                                 : plainroot_ui_box_create_v1(
                                       context_, PLAINROOT_UI_AXIS_VERTICAL,
                                       2.0F, 1.0F);
      plainroot_ui_widget *control = nullptr;
      plainroot_ui_widget *entry_content = nullptr;
      const bool is_symlink =
          entry->kind == PLAINROOT_CORE_FS_ENTRY_SYMLINK;
      const bool selectable = detail::entry_is_multi_selectable(
          allow_multiple_, select_directory_, entry->kind);
      if (selectable) {
        const bool selected =
            std::find(selected_paths_.begin(), selected_paths_.end(),
                      action->path) != selected_paths_.end();
        control = plainroot_ui_checkbox_create_v1(
            context_, label.c_str(), static_cast<uint32_t>(label.size()),
            selected ? 1U : 0U, 1.0F, &Impl::selection_changed, action.get());
        entry_content = control;
        if (control != nullptr &&
            (select_directory_ || (allow_multiple_ && is_symlink))) {
          const std::string open_label = tr("Open");
          auto *open_button = plainroot_ui_button_create_v1(
              context_, open_label.c_str(),
              static_cast<uint32_t>(open_label.size()), 1.0F,
              &Impl::entry_activated, action.get());
          auto *selection_row = plainroot_ui_box_create_v1(
              context_, PLAINROOT_UI_AXIS_HORIZONTAL, 2.0F, 1.0F);
          if (open_button == nullptr || selection_row == nullptr ||
              plainroot_ui_button_set_style_v1(
                  open_button, PLAINROOT_UI_BUTTON_STYLE_TEXT) !=
                  PLAINROOT_UI_RESULT_OK ||
              !add(selection_row, control) || !add(selection_row, open_button)) {
            entry_content = nullptr;
          } else {
            entry_content = selection_row;
          }
        }
      } else {
        control = plainroot_ui_button_create_v1(
            context_, label.c_str(), static_cast<uint32_t>(label.size()), 1.0F,
            &Impl::entry_activated, action.get());
        if (control != nullptr &&
            plainroot_ui_button_set_style_v1(
                control, PLAINROOT_UI_BUTTON_STYLE_TEXT) !=
                PLAINROOT_UI_RESULT_OK)
          control = nullptr;
        entry_content = control;
      }
      bool row_ready = entry_content != nullptr && row != nullptr;
      if (row_ready && recent_content != nullptr) {
        auto *location = plainroot_ui_label_create_v1(
            context_, entry->path.c_str(),
            static_cast<uint32_t>(entry->path.size()),
            PLAINROOT_UI_COLOR_ON_SURFACE_VARIANT_V3, 0.0F);
        if (location != nullptr)
          (void)plainroot_ui_widget_set_text_direction_v1(
              location, PLAINROOT_UI_TEXT_LTR);
        row_ready = location != nullptr && add(recent_content, entry_content) &&
                    add(recent_content, location) && add(row, recent_content);
      } else if (row_ready) {
        row_ready = add(row, entry_content);
      }
      if (!row_ready || !add(entries_list_, row))
        continue;
      rows_.push_back(row);
      actions_.push_back(std::move(action));
    }

    if (status_error_.empty() && !directory_loading_) {
      if (selected_filter_has_mime() && entries_mime_classified_ &&
          !mime_database_available_) {
        set_status(tr("MIME type filtering is unavailable on this system."));
      } else if (visible.empty()) {
        set_status(search_query_.empty()
                       ? tr(recent_mode_ ? "No recent files."
                                         : "This folder is empty.")
                       : tr("No matching files."));
      } else {
        set_status("");
      }
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
    if (filter.patterns.empty())
      return true;
    for (const BrowserFilterPattern &pattern : filter.patterns) {
      if (pattern.type == 0U) {
        if (fnmatch(pattern.pattern.c_str(), entry.name.c_str(), 0) == 0)
          return true;
      } else if (pattern.type == 1U) {
        if (mime_database_available_ &&
            detail::mime_type_pattern_matches(entry.mime_types,
                                              pattern.pattern))
          return true;
      }
    }
    return false;
  }

  bool selected_filter_has_mime() const {
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

  bool read_choice_value(size_t index, std::string &value) const {
    if (index >= choices_.size() || index >= choice_widgets_.size() ||
        index >= choice_selections_.size() ||
        choice_widgets_[index] == nullptr)
      return false;
    const BrowserChoice &choice = choices_[index];
    if (choice.options.empty()) {
      uint32_t checked = 0;
      if (plainroot_ui_checkbox_get_checked_v1(choice_widgets_[index],
                                                &checked) !=
          PLAINROOT_UI_RESULT_OK)
        return false;
      value = checked != 0U ? "true" : "false";
      return true;
    }

    int32_t selected = -1;
    uint32_t has_value = 0;
    if (plainroot_ui_dropdown_get_value_v1(choice_widgets_[index], &selected,
                                           &has_value) !=
            PLAINROOT_UI_RESULT_OK ||
        has_value == 0U || selected < 0 ||
        static_cast<size_t>(selected) >= choice.options.size())
      return false;
    value = choice.options[static_cast<size_t>(selected)].id;
    return true;
  }

  bool collect_choice_selections(
      std::vector<BrowserChoiceSelection> &selections) const {
    selections = choice_selections_;
    for (size_t index = 0; index < selections.size(); ++index)
      if (!read_choice_value(index, selections[index].value))
        return false;
    return true;
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
  bool allow_read_only_ = false;
  bool writable_ = true;
  bool finished_ = false;
  bool show_hidden_ = false;
  bool initializing_filter_dropdown_ = false;
  bool initializing_sort_dropdown_ = false;
  bool initializing_places_dropdown_ = false;
  bool initializing_choice_controls_ = false;
  bool overwrite_dialog_open_ = false;
  bool overwrite_confirmed_ = false;
  bool new_folder_created_ = false;
  bool folder_creation_pending_ = false;
  bool directory_loading_ = false;
  bool setting_symlink_selection_ = false;
  bool recent_mode_ = false;
  bool entries_mime_classified_ = false;
  bool mime_database_available_ = false;
  bool bookmarks_available_ = true;
  int32_t selected_filter_index_ = -1;
  detail::DirectorySortOrder sort_order_ =
      detail::DirectorySortOrder::name_ascending;
  uint64_t generation_ = 0;
  uint64_t folder_create_request_id_ = 0;
  uint64_t symlink_selection_request_id_ = 0;
  std::string current_path_;
  std::string search_query_;
  std::string initial_filename_;
  std::vector<std::string> save_files_;
  std::string pending_save_path_;
  std::string selected_path_;
  std::vector<std::string> selected_paths_;
  std::unordered_map<uint64_t, std::string> pending_symlink_selections_;
  std::vector<BrowserFilter> filters_;
  std::vector<BrowserChoice> choices_;
  std::vector<BrowserChoiceSelection> choice_selections_;
  std::vector<detail::Bookmark> bookmarks_;
  std::vector<Place> places_;
  std::vector<plainroot_ui_widget *> place_option_widgets_;
  std::string status_error_;
  std::vector<detail::DirectoryEntry> entries_;
  std::vector<plainroot_ui_widget *> rows_;
  std::vector<std::unique_ptr<EntryAction>> actions_;
  std::vector<plainroot_ui_widget *> breadcrumb_widgets_;
  std::vector<std::unique_ptr<BreadcrumbAction>> breadcrumb_actions_;
  std::vector<std::unique_ptr<ChoiceAction>> choice_actions_;
  std::vector<plainroot_ui_widget *> choice_widgets_;
  plainroot_ui_widget *breadcrumb_scroll_ = nullptr;
  plainroot_ui_widget *breadcrumb_strip_ = nullptr;
  plainroot_ui_widget *new_folder_button_ = nullptr;
  plainroot_ui_widget *new_folder_form_ = nullptr;
  plainroot_ui_widget *new_folder_field_ = nullptr;
  plainroot_ui_widget *new_folder_create_button_ = nullptr;
  plainroot_ui_widget *new_folder_cancel_button_ = nullptr;
  plainroot_ui_widget *filename_field_ = nullptr;
  plainroot_ui_widget *search_field_ = nullptr;
  plainroot_ui_widget *hidden_switch_ = nullptr;
  plainroot_ui_widget *places_dropdown_ = nullptr;
  plainroot_ui_widget *recent_button_ = nullptr;
  plainroot_ui_widget *bookmark_button_ = nullptr;
  plainroot_ui_widget *filter_dropdown_ = nullptr;
  plainroot_ui_widget *sort_dropdown_ = nullptr;
  plainroot_ui_widget *recent_sort_option_ = nullptr;
  plainroot_ui_widget *list_scroll_ = nullptr;
  plainroot_ui_widget *entries_list_ = nullptr;
  plainroot_ui_widget *status_label_ = nullptr;
  plainroot_ui_widget *selected_label_ = nullptr;
  plainroot_ui_widget *read_only_checkbox_ = nullptr;
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

void BrowserController::set_choices(std::vector<BrowserChoice> choices) {
  if (impl_)
    impl_->set_choices(std::move(choices));
}

void BrowserController::set_allow_read_only(bool allow_read_only) {
  if (impl_)
    impl_->set_allow_read_only(allow_read_only);
}

void BrowserController::set_save_file(bool save_file) {
  if (impl_)
    impl_->set_save_file(save_file);
}

void BrowserController::set_save_files(std::vector<std::string> filenames) {
  if (impl_)
    impl_->set_save_files(std::move(filenames));
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
