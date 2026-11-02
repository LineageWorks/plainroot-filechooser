#include "browser_controller.hpp"

#include "directory_loader.hpp"

#include <plainroot/i18n/i18n.h>

#include <glib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
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
       plainroot_ui_context *context)
      : application_(application), context_(context),
        loader_(application, [this](detail::DirectoryResult result) {
          apply_directory_result(std::move(result));
        }) {}

  ~Impl() { loader_.stop(); }

  bool ready() const { return loader_.ready(); }

  void shutdown() { loader_.stop(); }

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
    const std::string title_text = tr("Choose a file");
    auto *title = plainroot_ui_label_create_v1(
        context_, title_text.c_str(), static_cast<uint32_t>(title_text.size()),
        PLAINROOT_UI_COLOR_ON_SURFACE_V3, 0.0F);
    if (title != nullptr)
      (void)plainroot_ui_label_set_typography_v1(
          title, PLAINROOT_UI_LABEL_HEADLINE_SMALL);

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

    list_scroll_ =
        plainroot_ui_vertical_scroll_area_create_v1(context_, 0, 0, 1.0F);
    entries_list_ = plainroot_ui_list_create_v1(context_, 1.0F);
    status_label_ = make_label("Loading folder…", 0.0F);
    auto *footer = plainroot_ui_adaptive_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, PLAINROOT_UI_AXIS_HORIZONTAL,
        kExpandedControlsWidth, 12.0F, 0.0F);
    selected_label_ = make_label("No file selected", 1.0F);
    if (selected_label_ != nullptr)
      (void)plainroot_ui_widget_set_text_direction_v1(selected_label_,
                                                      PLAINROOT_UI_TEXT_LTR);
    auto *select = make_button("Open", &Impl::accept_activated);
    if (select != nullptr)
      (void)plainroot_ui_button_set_style_v1(select,
                                             PLAINROOT_UI_BUTTON_STYLE_FILLED);

    if (outer == nullptr || padding == nullptr || page == nullptr ||
        title == nullptr || location_row == nullptr ||
        location_actions == nullptr || up == nullptr || home == nullptr ||
        path_label_ == nullptr || controls == nullptr ||
        search_field_ == nullptr || hidden_switch_ == nullptr ||
        list_scroll_ == nullptr || entries_list_ == nullptr ||
        status_label_ == nullptr || footer == nullptr ||
        selected_label_ == nullptr || select == nullptr)
      return false;

    if (!add(outer, padding) || !add(padding, page) || !add(page, title) ||
        !add(page, location_row) || !add(location_row, location_actions) ||
        !add(location_actions, up) || !add(location_actions, home) ||
        !add(location_row, path_label_) || !add(page, controls) ||
        !add(controls, search_field_) || !add(controls, hidden_switch_) ||
        !add(page, list_scroll_) || !add(list_scroll_, entries_list_) ||
        !add(page, status_label_) || !add(page, footer) ||
        !add(footer, selected_label_) || !add(footer, select) ||
        plainroot_ui_context_set_root_v1(context_, outer) !=
            PLAINROOT_UI_RESULT_OK)
      return false;

    ui_ready_ = true;
    return true;
  }

  void set_window(plainroot_toolkit_window *window) { window_ = window; }

  void navigate(std::string path) {
    if (path.empty() || path.front() != '/') {
      set_status(tr("Choose an absolute directory path."));
      return;
    }
    current_path_ = std::move(path);
    selected_path_.clear();
    ++generation_;
    set_label(path_label_, display_path(current_path_));
    set_label(selected_label_, tr("No file selected"));
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

  static void entry_activated(void *user_data, plainroot_ui_widget *) {
    auto &action = *static_cast<EntryAction *>(user_data);
    if (action.kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY) {
      action.controller->navigate(action.path);
      return;
    }
    action.controller->selected_path_ = action.path;
    set_label(action.controller->selected_label_, display_path(action.path));
    action.controller->set_status(
        tr("File selected. Choose Open to return it."));
    action.controller->request_redraw();
  }

  static void accept_activated(void *user_data, plainroot_ui_widget *) {
    auto &self = *static_cast<Impl *>(user_data);
    if (self.selected_path_.empty()) {
      self.set_status(tr("Select a file first."));
      self.request_redraw();
      return;
    }
    (void)std::fwrite(self.selected_path_.data(), 1, self.selected_path_.size(),
                      stdout);
    (void)std::fputc('\n', stdout);
    (void)std::fflush(stdout);
    plainroot_toolkit_application_quit(self.application_);
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
      auto *button = plainroot_ui_button_create_v1(
          context_, label.c_str(), static_cast<uint32_t>(label.size()), 1.0F,
          &Impl::entry_activated, action.get());
      if (button == nullptr || row == nullptr ||
          plainroot_ui_button_set_style_v1(button,
                                           PLAINROOT_UI_BUTTON_STYLE_TEXT) !=
              PLAINROOT_UI_RESULT_OK ||
          !add(row, button) || !add(entries_list_, row))
        continue;
      rows_.push_back(row);
      actions_.push_back(std::move(action));
    }

    if (visible.empty() && status_error_.empty()) {
      set_status(search_query_.empty() ? tr("This folder is empty.")
                                       : tr("No matching files."));
    } else if (!visible.empty() && status_error_.empty()) {
      set_status("");
    }
    request_redraw();
  }

  static void set_label(plainroot_ui_widget *widget, const std::string &value) {
    if (widget == nullptr)
      return;
    (void)plainroot_ui_widget_set_text_v1(widget, value.c_str(),
                                          static_cast<uint32_t>(value.size()));
  }

  void set_status(const std::string &text) { set_label(status_label_, text); }

  void request_redraw() {
    if (window_ != nullptr)
      (void)plainroot_toolkit_window_request_redraw(window_);
  }

  plainroot_toolkit_application *application_ = nullptr;
  plainroot_ui_context *context_ = nullptr;
  plainroot_toolkit_window *window_ = nullptr;
  detail::DirectoryLoader loader_;
  bool ui_ready_ = false;
  bool show_hidden_ = false;
  uint64_t generation_ = 0;
  std::string current_path_;
  std::string search_query_;
  std::string selected_path_;
  std::string status_error_;
  std::vector<detail::DirectoryEntry> entries_;
  std::vector<plainroot_ui_widget *> rows_;
  std::vector<std::unique_ptr<EntryAction>> actions_;
  plainroot_ui_widget *path_label_ = nullptr;
  plainroot_ui_widget *search_field_ = nullptr;
  plainroot_ui_widget *hidden_switch_ = nullptr;
  plainroot_ui_widget *list_scroll_ = nullptr;
  plainroot_ui_widget *entries_list_ = nullptr;
  plainroot_ui_widget *status_label_ = nullptr;
  plainroot_ui_widget *selected_label_ = nullptr;
};

BrowserController::BrowserController(plainroot_toolkit_application *application,
                                     plainroot_ui_context *context)
    : impl_(std::make_unique<Impl>(application, context)) {}

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

void BrowserController::navigate(std::string path) {
  if (impl_)
    impl_->navigate(std::move(path));
}

} // namespace plainroot::filechooser
