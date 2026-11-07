#include "../src/browser_controller.hpp"
#include "../src/ui_context_factory.hpp"

#include <plainroot/i18n/i18n.h>
#include <plainroot/ui/widgets.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

class TemporaryConfigHome {
public:
  TemporaryConfigHome() {
    if (const char *previous = std::getenv("XDG_CONFIG_HOME")) {
      had_previous_ = true;
      previous_ = previous;
    }

    std::string pattern = "/tmp/plainroot-filechooser-accessibility-XXXXXX";
    std::vector<char> writable_pattern(pattern.begin(), pattern.end());
    writable_pattern.push_back('\0');
    char *created = ::mkdtemp(writable_pattern.data());
    if (created == nullptr)
      return;

    root_ = created;
    if (::setenv("XDG_CONFIG_HOME", root_.c_str(), 1) != 0)
      return;

    std::error_code error;
    std::filesystem::create_directories(root_ / "gtk-3.0", error);
    if (error)
      return;
    browse_directory_ = root_ / "browse";
    std::filesystem::create_directories(browse_directory_, error);
    ready_ = !error;
  }

  TemporaryConfigHome(const TemporaryConfigHome &) = delete;
  TemporaryConfigHome &operator=(const TemporaryConfigHome &) = delete;

  ~TemporaryConfigHome() {
    if (had_previous_)
      (void)::setenv("XDG_CONFIG_HOME", previous_.c_str(), 1);
    else
      (void)::unsetenv("XDG_CONFIG_HOME");

    std::error_code ignored;
    if (!root_.empty())
      std::filesystem::remove_all(root_, ignored);
  }

  bool ready() const { return ready_; }
  const std::filesystem::path &browse_directory() const {
    return browse_directory_;
  }
  std::filesystem::path bookmarks_file() const {
    return root_ / "gtk-3.0" / "bookmarks";
  }

private:
  std::filesystem::path root_;
  std::filesystem::path browse_directory_;
  std::string previous_;
  bool had_previous_ = false;
  bool ready_ = false;
};

bool require(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

const plainroot_ui_widget *find_widget(const plainroot_ui_widget *parent,
                                      const std::string &name,
                                      uint32_t role) {
  if (parent == nullptr)
    return nullptr;

  plainroot_ui_accessibility_info_v1 info{};
  info.struct_size = sizeof(info);
  if (plainroot_ui_widget_accessibility_v1(parent, &info) ==
          PLAINROOT_UI_RESULT_OK &&
      info.role == role &&
      info.name_utf8 != nullptr &&
      std::string(info.name_utf8, info.name_length) == name)
    return parent;

  size_t child_count = 0;
  if (plainroot_ui_widget_accessibility_child_count_v1(parent, &child_count) !=
      PLAINROOT_UI_RESULT_OK)
    return nullptr;
  for (size_t index = 0; index < child_count; ++index) {
    const plainroot_ui_widget *child = nullptr;
    if (plainroot_ui_widget_accessibility_child_at_v1(parent, index, &child) !=
        PLAINROOT_UI_RESULT_OK)
      continue;
    if (const auto *match = find_widget(child, name, role); match != nullptr)
      return match;
  }
  return nullptr;
}

plainroot_ui_rect_v1 widget_bounds(const plainroot_ui_widget *widget) {
  plainroot_ui_rect_v1 bounds{};
  bounds.struct_size = sizeof(bounds);
  if (widget == nullptr ||
      plainroot_ui_widget_bounds_v1(
          const_cast<plainroot_ui_widget *>(widget), &bounds) !=
          PLAINROOT_UI_RESULT_OK)
    bounds.width = -1.0F;
  return bounds;
}

bool fits_width(const plainroot_ui_widget *widget, float width) {
  const auto bounds = widget_bounds(widget);
  return bounds.width > 0.0F && bounds.x >= -0.5F &&
         bounds.x + bounds.width <= width + 0.5F;
}

bool shares_row(const plainroot_ui_widget *first,
                const plainroot_ui_widget *second) {
  const auto first_bounds = widget_bounds(first);
  const auto second_bounds = widget_bounds(second);
  return first_bounds.height > 0.0F && second_bounds.height > 0.0F &&
         std::abs(first_bounds.y - second_bounds.y) <= 1.0F;
}

bool is_below(const plainroot_ui_widget *first,
              const plainroot_ui_widget *second) {
  return widget_bounds(first).y > widget_bounds(second).y;
}

bool stable_name_after_input(const plainroot_ui_widget *field,
                             const std::string &name,
                             const std::string &value) {
  auto *mutable_field = const_cast<plainroot_ui_widget *>(field);
  if (field == nullptr ||
      plainroot_ui_widget_set_text_v1(
          mutable_field, value.c_str(), static_cast<uint32_t>(value.size())) !=
          PLAINROOT_UI_RESULT_OK)
    return false;

  plainroot_ui_accessibility_info_v1 info{};
  info.struct_size = sizeof(info);
  return plainroot_ui_widget_accessibility_v1(field, &info) ==
             PLAINROOT_UI_RESULT_OK &&
         info.name_utf8 != nullptr &&
         std::string(info.name_utf8, info.name_length) == name;
}

bool dispatch_key(plainroot_ui_context *context, uint32_t key) {
  plainroot_ui_event_v1 event{};
  event.struct_size = sizeof(event);
  event.type = PLAINROOT_UI_EVENT_KEY_DOWN;
  event.key = key;
  if (plainroot_ui_context_handle_event_v1(context, &event) !=
      PLAINROOT_UI_RESULT_OK)
    return false;
  event.type = PLAINROOT_UI_EVENT_KEY_UP;
  return plainroot_ui_context_handle_event_v1(context, &event) ==
         PLAINROOT_UI_RESULT_OK;
}

bool dispatch_pointer_click(plainroot_ui_context *context,
                            const plainroot_ui_widget *widget) {
  const auto bounds = widget_bounds(widget);
  if (bounds.width <= 0.0F || bounds.height <= 0.0F)
    return false;

  plainroot_ui_event_v1 event{};
  event.struct_size = sizeof(event);
  event.x = bounds.x + bounds.width * 0.5F;
  event.y = bounds.y + bounds.height * 0.5F;
  event.type = PLAINROOT_UI_EVENT_POINTER_DOWN;
  if (plainroot_ui_context_handle_event_v1(context, &event) !=
      PLAINROOT_UI_RESULT_OK)
    return false;
  event.type = PLAINROOT_UI_EVENT_POINTER_UP;
  return plainroot_ui_context_handle_event_v1(context, &event) ==
         PLAINROOT_UI_RESULT_OK;
}

bool is_expanded(const plainroot_ui_widget *widget) {
  plainroot_ui_accessibility_info_v1 info{};
  info.struct_size = sizeof(info);
  return widget != nullptr &&
         plainroot_ui_widget_accessibility_v1(widget, &info) ==
             PLAINROOT_UI_RESULT_OK &&
         (info.state & PLAINROOT_UI_WIDGET_STATE_EXPANDED) != 0;
}

int run_rtl_responsive_layout_test(const char *language_tag) {
  const bool arabic = std::strcmp(language_tag, "ar") == 0;
  const char *places_label = arabic ? "الأماكن" : "מקומות";
  const char *more_actions_label = arabic ? "إجراءات أخرى" : "פעולות נוספות";
  const char *folder_name_label = arabic ? "اسم المجلد" : "שם התיקייה";
  const char *cancel_label = arabic ? "إلغاء" : "ביטול";
  const char *bookmark_label =
      arabic ? "إضافة المجلد إلى الإشارات المرجعية" : "הוספת התיקייה לסימניות";
  const char *remove_bookmark_label =
      arabic ? "إزالة الإشارة المرجعية" : "הסרת הסימנייה";
  const char *new_folder_label = arabic ? "مجلد جديد" : "תיקייה חדשה";
  TemporaryConfigHome temporary_config;
  if (!require(temporary_config.ready(),
               "isolated config home for RTL responsive layout"))
    return 1;

  std::array<char, 512> localization_error{};
  plainroot_i18n_options_v1 localization_options{};
  localization_options.struct_size = sizeof(localization_options);
  localization_options.fallback_language_tag = "en";
  if (!require(plainroot_i18n_initialize_v1(
                   &localization_options, localization_error.data(),
                   localization_error.size()) == PLAINROOT_I18N_RESULT_OK &&
                   plainroot_i18n_bind_domain_v1(
                       "plainroot-filechooser", nullptr,
                       localization_error.data(),
                       localization_error.size()) == PLAINROOT_I18N_RESULT_OK,
               "RTL FileChooser localization initialization"))
    return 1;
  const char *translated_filter_hint = plainroot_i18n_gettext_v1(
      "plainroot-filechooser", "Filter files");
  const std::string filter_hint =
      translated_filter_hint == nullptr ? "Filter files"
                                        : translated_filter_hint;

  std::unique_ptr<plainroot_ui_context, decltype(&plainroot_ui_context_destroy)>
      context(plainroot::filechooser::make_ui_context(language_tag),
              &plainroot_ui_context_destroy);
  if (!require(context != nullptr, "RTL UI context creation"))
    return 1;

  plainroot::filechooser::BrowserController controller(nullptr, context.get());
  controller.set_filters(
      {{"Text files", {{0U, "*.txt"}}},
       {"Image files", {{0U, "*.png"}}}},
      0);
  if (!require(controller.ready(), "directory worker startup") ||
      !require(controller.create_ui(), "RTL browser UI creation"))
    return 1;
  controller.navigate(temporary_config.browse_directory().string());

  struct Viewport {
    float width;
    float height;
    const char *name;
  };
  constexpr std::array<Viewport, 5> viewports{{
      {390.0F, 844.0F, "phone portrait"},
      {844.0F, 390.0F, "phone landscape"},
      {768.0F, 1024.0F, "tablet portrait"},
      {1024.0F, 768.0F, "tablet landscape"},
      {1366.0F, 768.0F, "laptop"},
  }};

  plainroot_ui_rect_v1 bounds{};
  bounds.struct_size = sizeof(bounds);
  for (const Viewport &viewport : viewports) {
    controller.update_viewport(viewport.width, viewport.height);
    bounds.width = viewport.width;
    bounds.height = viewport.height;
    const auto result = plainroot_ui_context_layout_v1(context.get(), &bounds);
    if (result != PLAINROOT_UI_RESULT_OK) {
      std::cerr << "RTL responsive layout failed at " << viewport.name << " ("
                << viewport.width << 'x' << viewport.height << ", UI result "
                << static_cast<unsigned>(result) << ")\n";
      return 1;
    }
  }

  const auto layout_at = [&](float width, float height) {
    controller.update_viewport(width, height);
    bounds.width = width;
    bounds.height = height;
    return plainroot_ui_context_layout_v1(context.get(), &bounds) ==
           PLAINROOT_UI_RESULT_OK;
  };
  const auto bookmark_is_present = [&] {
    std::ifstream input(temporary_config.bookmarks_file());
    const std::string contents{std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()};
    return contents.find(temporary_config.browse_directory().string()) !=
           std::string::npos;
  };
  const plainroot_ui_widget *root = nullptr;
  const auto open_folder_form = [&](const plainroot_ui_widget *action) {
    if (action == nullptr || !dispatch_pointer_click(context.get(), action) ||
        plainroot_ui_context_layout_v1(context.get(), &bounds) !=
            PLAINROOT_UI_RESULT_OK)
      return false;
    const plainroot_ui_widget *root = nullptr;
    return plainroot_ui_context_get_accessibility_root_v1(
               context.get(), &root) == PLAINROOT_UI_RESULT_OK &&
           find_widget(root, folder_name_label,
                       PLAINROOT_UI_WIDGET_TEXT_FIELD) != nullptr;
  };
  const auto cancel_folder_form = [&] {
    const auto *cancel =
        find_widget(root, cancel_label, PLAINROOT_UI_WIDGET_BUTTON);
    return cancel != nullptr && dispatch_pointer_click(context.get(), cancel) &&
           plainroot_ui_context_layout_v1(context.get(), &bounds) ==
               PLAINROOT_UI_RESULT_OK &&
           find_widget(root, folder_name_label,
                       PLAINROOT_UI_WIDGET_TEXT_FIELD) == nullptr;
  };

  if (!require(plainroot_ui_context_get_accessibility_root_v1(
                   context.get(), &root) == PLAINROOT_UI_RESULT_OK,
               "RTL responsive accessibility root"))
    return 1;

  if (!require(layout_at(390.0F, 844.0F), "RTL phone interaction layout"))
    return 1;
  const auto *phone_filter =
      find_widget(root, filter_hint, PLAINROOT_UI_WIDGET_DROPDOWN);
  int32_t initial_filter = -1;
  uint32_t has_initial_filter = 0;
  const bool filter_ready_on_phone =
      phone_filter != nullptr && fits_width(phone_filter, 390.0F) &&
      plainroot_ui_dropdown_get_value_v1(phone_filter, &initial_filter,
                                         &has_initial_filter) ==
          PLAINROOT_UI_RESULT_OK &&
      has_initial_filter != 0U && initial_filter == 0;
  const bool filter_menu_opened =
      filter_ready_on_phone &&
      dispatch_pointer_click(context.get(), phone_filter) &&
      plainroot_ui_context_layout_v1(context.get(), &bounds) ==
          PLAINROOT_UI_RESULT_OK &&
      is_expanded(phone_filter);
  const bool second_filter_selected =
      filter_menu_opened && dispatch_key(context.get(), PLAINROOT_UI_KEY_DOWN) &&
      dispatch_key(context.get(), PLAINROOT_UI_KEY_DOWN) &&
      dispatch_key(context.get(), PLAINROOT_UI_KEY_ENTER);
  int32_t selected_filter = -1;
  uint32_t has_selected_filter = 0;
  const auto *filter_after_selection =
      find_widget(root, filter_hint, PLAINROOT_UI_WIDGET_DROPDOWN);
  const bool filter_selection_changed =
      second_filter_selected &&
      filter_after_selection == phone_filter &&
      plainroot_ui_dropdown_get_value_v1(phone_filter, &selected_filter,
                                         &has_selected_filter) ==
          PLAINROOT_UI_RESULT_OK &&
      has_selected_filter != 0U && selected_filter == 1;
  if (!require(filter_ready_on_phone,
               "RTL phone filter control fits and exposes its initial value") ||
      !require(filter_selection_changed,
               "RTL phone filter menu selects its next option by keyboard"))
    return 1;

  const auto *phone_places =
      find_widget(root, places_label, PLAINROOT_UI_WIDGET_DROPDOWN);
  const auto *phone_more_actions =
      find_widget(root, more_actions_label, PLAINROOT_UI_WIDGET_BUTTON);
  uint8_t phone_more_visible = 0;
  const auto phone_places_bounds = widget_bounds(phone_places);
  const auto phone_more_bounds = widget_bounds(phone_more_actions);
  const bool phone_menu_in_rtl_order =
      phone_places != nullptr && phone_more_actions != nullptr &&
      plainroot_ui_widget_get_visible_v1(phone_more_actions,
                                         &phone_more_visible) ==
          PLAINROOT_UI_RESULT_OK &&
      phone_more_visible != 0 && shares_row(phone_places, phone_more_actions) &&
      phone_more_bounds.x + phone_more_bounds.width < phone_places_bounds.x;
  if (!require(phone_menu_in_rtl_order,
               "RTL phone keeps More beside Places in RTL order") ||
      !require(dispatch_pointer_click(context.get(), phone_more_actions) &&
                   plainroot_ui_context_layout_v1(context.get(), &bounds) ==
                       PLAINROOT_UI_RESULT_OK &&
                   is_expanded(phone_more_actions),
               "RTL phone More menu opens by pointer"))
    return 1;

  const auto ensure_phone_menu_open = [&] {
    if (!is_expanded(phone_more_actions) &&
        (!dispatch_pointer_click(context.get(), phone_more_actions) ||
         plainroot_ui_context_layout_v1(context.get(), &bounds) !=
             PLAINROOT_UI_RESULT_OK))
      return false;
    return is_expanded(phone_more_actions);
  };
  const auto ensure_phone_menu_closed = [&] {
    if (is_expanded(phone_more_actions) &&
        (!dispatch_pointer_click(context.get(), phone_more_actions) ||
         plainroot_ui_context_layout_v1(context.get(), &bounds) !=
             PLAINROOT_UI_RESULT_OK))
      return false;
    return !is_expanded(phone_more_actions);
  };
  const bool phone_bookmark_saved =
      ensure_phone_menu_open() &&
      dispatch_key(context.get(), PLAINROOT_UI_KEY_ENTER) &&
      bookmark_is_present();
  if (!require(phone_bookmark_saved,
               "RTL phone menu adds the current folder bookmark"))
    return 1;

  const bool phone_bookmark_removed =
      ensure_phone_menu_closed() && ensure_phone_menu_open() &&
      dispatch_key(context.get(), PLAINROOT_UI_KEY_ENTER) &&
      !bookmark_is_present();
  if (!require(phone_bookmark_removed,
               "RTL phone menu removes the current folder bookmark"))
    return 1;

  const bool phone_new_folder_opened =
      ensure_phone_menu_closed() && ensure_phone_menu_open() &&
      dispatch_key(context.get(), PLAINROOT_UI_KEY_DOWN) &&
      dispatch_key(context.get(), PLAINROOT_UI_KEY_ENTER) &&
      plainroot_ui_context_layout_v1(context.get(), &bounds) ==
          PLAINROOT_UI_RESULT_OK &&
      find_widget(root, folder_name_label,
                  PLAINROOT_UI_WIDGET_TEXT_FIELD) != nullptr;
  if (!require(phone_new_folder_opened,
               "RTL phone menu opens the New folder form") ||
      !require(cancel_folder_form(),
               "RTL phone cancels the New folder form by pointer"))
    return 1;

  if (!require(layout_at(1024.0F, 768.0F),
               "RTL tablet landscape interaction layout"))
    return 1;
  const auto *places =
      find_widget(root, places_label, PLAINROOT_UI_WIDGET_DROPDOWN);
  const auto *more_actions =
      find_widget(root, more_actions_label, PLAINROOT_UI_WIDGET_BUTTON);
  uint8_t more_visible = 0;
  const auto places_bounds = widget_bounds(places);
  const auto more_bounds = widget_bounds(more_actions);
  const bool tablet_menu_in_rtl_order =
      places != nullptr && more_actions != nullptr &&
      plainroot_ui_widget_get_visible_v1(more_actions, &more_visible) ==
          PLAINROOT_UI_RESULT_OK &&
      more_visible != 0 && shares_row(places, more_actions) &&
      more_bounds.x + more_bounds.width < places_bounds.x;
  if (!tablet_menu_in_rtl_order) {
    std::cerr << "RTL tablet menu geometry: places=" << (places != nullptr)
              << " more=" << (more_actions != nullptr)
              << " visible=" << static_cast<unsigned>(more_visible)
              << " places_bounds=" << places_bounds.x << ',' << places_bounds.y
              << ' ' << places_bounds.width << 'x' << places_bounds.height
              << " more_bounds=" << more_bounds.x << ',' << more_bounds.y << ' '
              << more_bounds.width << 'x' << more_bounds.height << '\n';
  }
  if (!require(tablet_menu_in_rtl_order,
               "RTL tablet keeps More beside Places in RTL order") ||
      !require(dispatch_pointer_click(context.get(), more_actions) &&
                   plainroot_ui_context_layout_v1(context.get(), &bounds) ==
                       PLAINROOT_UI_RESULT_OK &&
                   is_expanded(more_actions),
               "RTL tablet More menu opens by pointer"))
    return 1;

  const auto ensure_menu_open = [&] {
    if (!is_expanded(more_actions) &&
        (!dispatch_pointer_click(context.get(), more_actions) ||
         plainroot_ui_context_layout_v1(context.get(), &bounds) !=
             PLAINROOT_UI_RESULT_OK))
      return false;
    return is_expanded(more_actions);
  };
  const auto ensure_menu_closed = [&] {
    if (is_expanded(more_actions) &&
        (!dispatch_pointer_click(context.get(), more_actions) ||
         plainroot_ui_context_layout_v1(context.get(), &bounds) !=
             PLAINROOT_UI_RESULT_OK))
      return false;
    return !is_expanded(more_actions);
  };
  const bool tablet_bookmark_saved =
      ensure_menu_open() &&
      dispatch_key(context.get(), PLAINROOT_UI_KEY_ENTER) &&
      bookmark_is_present();
  if (!require(tablet_bookmark_saved,
               "RTL tablet menu adds the current folder bookmark"))
    return 1;

  const bool tablet_bookmark_removed =
      ensure_menu_closed() && ensure_menu_open() &&
      dispatch_key(context.get(), PLAINROOT_UI_KEY_ENTER) &&
      !bookmark_is_present();
  if (!require(tablet_bookmark_removed,
               "RTL tablet menu removes the current folder bookmark"))
    return 1;

  const bool tablet_new_folder_opened =
      ensure_menu_closed() && ensure_menu_open() &&
      dispatch_key(context.get(), PLAINROOT_UI_KEY_DOWN) &&
      dispatch_key(context.get(), PLAINROOT_UI_KEY_ENTER) &&
      plainroot_ui_context_layout_v1(context.get(), &bounds) ==
          PLAINROOT_UI_RESULT_OK &&
      find_widget(root, folder_name_label, PLAINROOT_UI_WIDGET_TEXT_FIELD) !=
          nullptr;
  if (!require(tablet_new_folder_opened,
               "RTL tablet menu opens the New folder form") ||
      !require(cancel_folder_form(),
               "RTL tablet cancels the New folder form by pointer"))
    return 1;

  if (!require(layout_at(1366.0F, 768.0F), "RTL laptop interaction layout"))
    return 1;
  const auto *wide_filter =
      find_widget(root, filter_hint, PLAINROOT_UI_WIDGET_DROPDOWN);
  const auto *wide_hidden = find_widget(
      root, arabic ? "إظهار الملفات المخفية" : "הצגת קבצים מוסתרים",
      PLAINROOT_UI_WIDGET_SWITCH);
  const auto wide_filter_bounds = widget_bounds(wide_filter);
  const auto wide_hidden_bounds = widget_bounds(wide_hidden);
  if (!require(wide_filter != nullptr && wide_hidden != nullptr &&
                   shares_row(wide_filter, wide_hidden) &&
                   wide_filter_bounds.x < wide_hidden_bounds.x,
               "RTL laptop filter control follows the switches in RTL order"))
    return 1;

  const auto *wide_bookmark =
      find_widget(root, bookmark_label, PLAINROOT_UI_WIDGET_BUTTON);
  if (!require(wide_bookmark != nullptr &&
                   dispatch_pointer_click(context.get(), wide_bookmark) &&
                   bookmark_is_present(),
               "RTL laptop inline action adds the current folder bookmark"))
    return 1;
  if (!require(layout_at(1366.0F, 768.0F),
               "RTL laptop layout after adding bookmark"))
    return 1;
  const auto *wide_remove_bookmark =
      find_widget(root, remove_bookmark_label, PLAINROOT_UI_WIDGET_BUTTON);
  if (!require(
          wide_remove_bookmark != nullptr &&
              dispatch_pointer_click(context.get(), wide_remove_bookmark) &&
              !bookmark_is_present(),
          "RTL laptop inline action removes the current bookmark"))
    return 1;
  if (!require(layout_at(1366.0F, 768.0F),
               "RTL laptop layout before New folder"))
    return 1;
  const auto *wide_new_folder =
      find_widget(root, new_folder_label, PLAINROOT_UI_WIDGET_BUTTON);
  if (!require(open_folder_form(wide_new_folder),
               "RTL laptop inline action opens the New folder form") ||
      !require(cancel_folder_form(),
               "RTL laptop cancels the New folder form by pointer"))
    return 1;

  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::strcmp(argv[1], "--rtl-responsive") == 0)
    return run_rtl_responsive_layout_test("ar");
  if (argc == 2 && std::strcmp(argv[1], "--rtl-responsive-he") == 0)
    return run_rtl_responsive_layout_test("he");

  TemporaryConfigHome temporary_config;
  if (!require(temporary_config.ready(),
               "isolated config home for bookmark actions"))
    return 1;

  std::unique_ptr<plainroot_ui_context, decltype(&plainroot_ui_context_destroy)>
      context(plainroot::filechooser::make_ui_context("en"),
              &plainroot_ui_context_destroy);
  if (!require(context != nullptr, "UI context creation"))
    return 1;

  plainroot::filechooser::BrowserController controller(nullptr, context.get());
  controller.set_save_file(true);
  controller.set_initial_filename("draft.txt");
  if (!require(controller.ready(), "directory worker startup") ||
      !require(controller.create_ui(), "browser UI creation"))
    return 1;
  controller.navigate(temporary_config.browse_directory().string());

  plainroot_ui_rect_v1 bounds{};
  bounds.struct_size = sizeof(bounds);
  bounds.width = 1000.0F;
  bounds.height = 720.0F;
  if (!require(plainroot_ui_context_layout_v1(context.get(), &bounds) ==
                   PLAINROOT_UI_RESULT_OK,
               "browser accessibility layout"))
    return 1;

  const plainroot_ui_widget *root = nullptr;
  if (!require(plainroot_ui_context_get_accessibility_root_v1(
                   context.get(), &root) == PLAINROOT_UI_RESULT_OK,
               "accessibility root"))
    return 1;

  const auto *search = find_widget(root, "Search files",
                                   PLAINROOT_UI_WIDGET_TEXT_FIELD);
  const auto *filename = find_widget(root, "File name",
                                     PLAINROOT_UI_WIDGET_TEXT_FIELD);
  const auto *sort = find_widget(
      root, "Sort files", PLAINROOT_UI_WIDGET_DROPDOWN);
  const auto *hidden = find_widget(root, "Show hidden files",
                                   PLAINROOT_UI_WIDGET_SWITCH);
  const auto *details = find_widget(root, "Show file details",
                                    PLAINROOT_UI_WIDGET_SWITCH);
  plainroot_ui_accessibility_info_v1 sort_info{};
  sort_info.struct_size = sizeof(sort_info);
  const bool stable_sort_accessible_name =
      sort != nullptr &&
      plainroot_ui_dropdown_set_value_v1(
          const_cast<plainroot_ui_widget *>(sort), 1) ==
          PLAINROOT_UI_RESULT_OK &&
      plainroot_ui_widget_accessibility_v1(sort, &sort_info) ==
          PLAINROOT_UI_RESULT_OK &&
      sort_info.name_utf8 != nullptr &&
      std::string(sort_info.name_utf8, sort_info.name_length) == "Sort files";
  const bool compact_controls_found =
      search != nullptr && sort != nullptr && hidden != nullptr &&
      details != nullptr;
  struct Viewport {
    float width;
    float height;
    bool controls_horizontal;
    const char *name;
  };
  constexpr std::array<Viewport, 5> viewports{{
      {390.0F, 844.0F, false, "phone portrait"},
      {844.0F, 390.0F, true, "phone landscape"},
      {768.0F, 1024.0F, false, "tablet portrait"},
      {1024.0F, 768.0F, true, "tablet landscape"},
      {1366.0F, 768.0F, true, "laptop"},
  }};
  bool responsive_controls_passed = compact_controls_found;
  for (const Viewport &viewport : viewports) {
    bounds.width = viewport.width;
    bounds.height = viewport.height;
    const bool laid_out =
        plainroot_ui_context_layout_v1(context.get(), &bounds) ==
        PLAINROOT_UI_RESULT_OK;
    const bool fits = fits_width(search, viewport.width) &&
                      fits_width(sort, viewport.width) &&
                      fits_width(hidden, viewport.width) &&
                      fits_width(details, viewport.width);
    const bool arrangement =
        (shares_row(search, sort) == viewport.controls_horizontal) &&
        (shares_row(hidden, details) == viewport.controls_horizontal) &&
        (viewport.controls_horizontal ||
         (is_below(sort, search) && is_below(details, hidden)));
    if (!laid_out || !fits || !arrangement) {
      std::cerr << "Responsive browser controls failed at " << viewport.name
                << " (" << viewport.width << 'x' << viewport.height << ")\n";
      responsive_controls_passed = false;
    }
  }
  const auto *bookmark_button = find_widget(
      root, "Bookmark folder", PLAINROOT_UI_WIDGET_BUTTON);
  const auto *new_folder_button = find_widget(
      root, "New folder", PLAINROOT_UI_WIDGET_BUTTON);
  const bool wide_actions_present = bookmark_button != nullptr &&
                                    new_folder_button != nullptr;
  controller.update_viewport(390.0F, 844.0F);
  bounds.width = 390.0F;
  bounds.height = 844.0F;
  const bool phone_layout = wide_actions_present &&
      plainroot_ui_context_layout_v1(context.get(), &bounds) ==
          PLAINROOT_UI_RESULT_OK;
  const auto *places = find_widget(root, "Places", PLAINROOT_UI_WIDGET_DROPDOWN);
  const auto *more_actions = find_widget(
      root, "More actions", PLAINROOT_UI_WIDGET_BUTTON);
  uint8_t more_actions_visible = 0;
  const bool overflow_button_visible =
      phone_layout && more_actions != nullptr &&
      plainroot_ui_widget_get_visible_v1(more_actions,
                                         &more_actions_visible) ==
          PLAINROOT_UI_RESULT_OK &&
      more_actions_visible != 0;
  const bool overflow_beside_places =
      overflow_button_visible && places != nullptr &&
      shares_row(places, more_actions);
  const bool wide_actions_hidden =
      find_widget(root, "Bookmark folder", PLAINROOT_UI_WIDGET_BUTTON) ==
          nullptr &&
      find_widget(root, "New folder", PLAINROOT_UI_WIDGET_BUTTON) == nullptr;
  const bool overflow_pointer_opened =
      overflow_button_visible &&
      dispatch_pointer_click(context.get(), more_actions);
  const bool overflow_pointer_expanded =
      overflow_pointer_opened && is_expanded(more_actions);
  const bool overflow_pointer_closed =
      overflow_pointer_expanded &&
      dispatch_pointer_click(context.get(), more_actions) &&
      !is_expanded(more_actions);
  const bool overflow_opened =
      overflow_pointer_closed &&
      plainroot_ui_widget_accessibility_perform_action_v1(
          const_cast<plainroot_ui_widget *>(more_actions),
          PLAINROOT_UI_ACCESSIBILITY_ACTION_DEFAULT) == PLAINROOT_UI_RESULT_OK;
  const bool overflow_expanded =
      overflow_opened && is_expanded(more_actions);

  const bool bookmark_activated =
      overflow_expanded && dispatch_key(context.get(), PLAINROOT_UI_KEY_ENTER);
  std::ifstream bookmarks_input(temporary_config.bookmarks_file());
  const std::string bookmark_contents{
      std::istreambuf_iterator<char>(bookmarks_input),
      std::istreambuf_iterator<char>()};
  const bool bookmark_saved =
      bookmark_activated &&
      bookmark_contents.find(temporary_config.browse_directory().string()) !=
          std::string::npos;
  const bool remove_bookmark_activated =
      bookmark_activated &&
      plainroot_ui_widget_accessibility_perform_action_v1(
          const_cast<plainroot_ui_widget *>(more_actions),
          PLAINROOT_UI_ACCESSIBILITY_ACTION_DEFAULT) == PLAINROOT_UI_RESULT_OK &&
      dispatch_key(context.get(), PLAINROOT_UI_KEY_ENTER);
  std::ifstream removed_bookmarks_input(temporary_config.bookmarks_file());
  const std::string removed_bookmark_contents{
      std::istreambuf_iterator<char>(removed_bookmarks_input),
      std::istreambuf_iterator<char>()};
  const bool bookmark_removed =
      remove_bookmark_activated &&
      removed_bookmark_contents.find(
          temporary_config.browse_directory().string()) == std::string::npos;
  const bool overflow_reopened_for_folder =
      remove_bookmark_activated &&
      plainroot_ui_widget_accessibility_perform_action_v1(
          const_cast<plainroot_ui_widget *>(more_actions),
          PLAINROOT_UI_ACCESSIBILITY_ACTION_DEFAULT) == PLAINROOT_UI_RESULT_OK;
  const bool new_folder_menu_action =
      overflow_reopened_for_folder &&
      dispatch_key(context.get(), PLAINROOT_UI_KEY_DOWN) &&
      dispatch_key(context.get(), PLAINROOT_UI_KEY_ENTER);
  const auto *folder_name = new_folder_menu_action
                                ? find_widget(root, "Folder name",
                                              PLAINROOT_UI_WIDGET_TEXT_FIELD)
                                : nullptr;
  const bool passed =
      require(search != nullptr, "search field has a stable accessible name") &&
      require(filename != nullptr,
              "filename field has a stable accessible name") &&
      require(overflow_button_visible,
              "phone layout exposes the More actions button") &&
      require(overflow_beside_places,
              "More actions remains beside Places on phone") &&
      require(wide_actions_hidden,
              "phone layout moves folder actions out of the wide action row") &&
      require(overflow_pointer_expanded,
              "pointer click opens More actions on phone") &&
      require(overflow_pointer_closed,
              "pointer click closes the open More actions menu") &&
      require(overflow_expanded,
              "More actions opens from its accessibility action") &&
      require(bookmark_saved,
              "Bookmark menu action persists the current directory") &&
      require(bookmark_removed,
              "Bookmark menu action removes the current directory") &&
      require(new_folder_menu_action,
              "New folder menu action opens the folder form") &&
      require(folder_name != nullptr,
              "visible new-folder field has a stable accessible name") &&
      require(stable_sort_accessible_name,
              "sort dropdown keeps its accessible name when its value changes") &&
      require(responsive_controls_passed,
              "search, sort, and switches adapt across phone, tablet, and "
              "laptop layouts") &&
      require(stable_name_after_input(search, "Search files", "notes"),
              "search query does not replace the accessible name") &&
      require(stable_name_after_input(filename, "File name", "report.txt"),
              "filename value does not replace the accessible name") &&
      require(stable_name_after_input(folder_name, "Folder name", "archive"),
              "folder value does not replace the accessible name");
  controller.shutdown();
  return passed ? 0 : 1;
}
