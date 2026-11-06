#include "../src/browser_controller.hpp"
#include "../src/ui_context_factory.hpp"

#include <plainroot/ui/widgets.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <string>

namespace {

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

} // namespace

int main() {
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
  const auto *new_folder_button = find_widget(
      root, "New folder", PLAINROOT_UI_WIDGET_BUTTON);
  const bool folder_form_opened =
      new_folder_button != nullptr &&
      plainroot_ui_widget_accessibility_perform_action_v1(
          const_cast<plainroot_ui_widget *>(new_folder_button),
          PLAINROOT_UI_ACCESSIBILITY_ACTION_DEFAULT) == PLAINROOT_UI_RESULT_OK;
  const auto *folder_name = folder_form_opened
                                ? find_widget(root, "Folder name",
                                              PLAINROOT_UI_WIDGET_TEXT_FIELD)
                                : nullptr;
  const bool passed =
      require(search != nullptr, "search field has a stable accessible name") &&
      require(filename != nullptr,
              "filename field has a stable accessible name") &&
      require(folder_form_opened,
              "New folder opens from the accessibility action") &&
      require(folder_name != nullptr,
              "visible new-folder field has a stable accessible name") &&
      require(stable_name_after_input(search, "Search files", "notes"),
              "search query does not replace the accessible name") &&
      require(stable_name_after_input(filename, "File name", "report.txt"),
              "filename value does not replace the accessible name") &&
      require(stable_name_after_input(folder_name, "Folder name", "archive"),
              "folder value does not replace the accessible name");
  controller.shutdown();
  return passed ? 0 : 1;
}
