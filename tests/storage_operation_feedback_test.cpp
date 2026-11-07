#include "../src/storage_locations_view.hpp"
#include "../src/storage_operation_error.hpp"
#include "../src/network_operation_error.hpp"
#include "../src/ui_context_factory.hpp"

#include <plainroot/i18n/i18n.h>
#include <plainroot/ui/widgets.h>

#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

using plainroot::filechooser::detail::StorageLocationActions;
using plainroot::filechooser::detail::StorageLocationsView;

struct Viewport {
  float width;
  float height;
  const char *name;
};

constexpr std::array<Viewport, 5> kViewports{{
    {390.0F, 844.0F, "phone portrait"},
    {844.0F, 390.0F, "phone landscape"},
    {768.0F, 1024.0F, "tablet portrait"},
    {1024.0F, 768.0F, "tablet landscape"},
    {1366.0F, 768.0F, "laptop"},
}};

bool require(bool condition, const std::string &message) {
  if (condition)
    return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

const plainroot_ui_widget *find_widget(const plainroot_ui_widget *parent,
                                       const std::string &name) {
  if (parent == nullptr)
    return nullptr;

  plainroot_ui_accessibility_info_v1 info{};
  info.struct_size = sizeof(info);
  if (plainroot_ui_widget_accessibility_v1(parent, &info) ==
          PLAINROOT_UI_RESULT_OK &&
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
    if (const auto *match = find_widget(child, name); match != nullptr)
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

bool status_fits(const plainroot_ui_widget *widget, float width) {
  const auto bounds = widget_bounds(widget);
  return bounds.width > 0.0F && bounds.height > 0.0F && bounds.x >= -0.5F &&
         bounds.x + bounds.width <= width + 0.5F;
}

int run(const char *language_tag) {
  std::array<char, 512> error{};
  plainroot_i18n_options_v1 localization_options{};
  localization_options.struct_size = sizeof(localization_options);
  localization_options.fallback_language_tag = "en";
  if (!require(plainroot_i18n_initialize_v1(
                  &localization_options, error.data(), error.size()) ==
                  PLAINROOT_I18N_RESULT_OK &&
                  plainroot_i18n_bind_domain_v1(
                      "plainroot-filechooser", nullptr, error.data(),
                      error.size()) == PLAINROOT_I18N_RESULT_OK,
              "initialize FileChooser translations"))
    return 1;

  std::unique_ptr<plainroot_ui_context, decltype(&plainroot_ui_context_destroy)>
      context(plainroot::filechooser::make_ui_context(language_tag),
              &plainroot_ui_context_destroy);
  if (!require(context != nullptr, "create localized UI context"))
    return 1;

  StorageLocationActions actions;
  StorageLocationsView view(context.get(), std::move(actions));
  plainroot_ui_widget *root = view.create();
  if (!require(root != nullptr, "create storage locations view") ||
      !require(plainroot_ui_context_set_root_v1(context.get(), root) ==
                   PLAINROOT_UI_RESULT_OK,
               "set storage locations view root"))
    return 1;

  struct BusyMessage {
    const char *source;
    const char *description;
  };
  const std::array<BusyMessage, 2> messages{{
      {plainroot::filechooser::detail::storage_operation_error_message(
           PLAINROOT_STORAGE_OPERATION_ERROR_BUSY),
       "local unmount busy feedback"},
      {plainroot::filechooser::detail::network_operation_error_message(
           PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_BUSY),
       "network disconnect busy feedback"},
  }};

  bool passed = true;
  for (const BusyMessage &busy : messages) {
    const char *translated =
        plainroot_i18n_gettext_v1("plainroot-filechooser", busy.source);
    const std::string message =
        translated == nullptr ? busy.source : translated;
    const bool translation_present =
        std::strcmp(language_tag, "en") == 0 || message != busy.source;
    passed = require(translation_present,
                     std::string(busy.description) + " has a " +
                         language_tag + " translation") &&
             passed;
    view.set_status(message);

    for (const Viewport &viewport : kViewports) {
      plainroot_ui_rect_v1 bounds{};
      bounds.struct_size = sizeof(bounds);
      bounds.width = viewport.width;
      bounds.height = viewport.height;
      const bool laid_out = plainroot_ui_context_layout_v1(
                                context.get(), &bounds) ==
                            PLAINROOT_UI_RESULT_OK;
      const plainroot_ui_widget *accessibility_root = nullptr;
      const bool have_accessibility_root =
          plainroot_ui_context_get_accessibility_root_v1(
              context.get(), &accessibility_root) == PLAINROOT_UI_RESULT_OK;
      const auto *status = have_accessibility_root
                               ? find_widget(accessibility_root, message)
                               : nullptr;
      if (!require(laid_out,
                   std::string("layout ") + busy.description + " at " +
                       viewport.name) ||
          !require(status != nullptr,
                   std::string(busy.description) +
                       " is exposed in the accessibility tree") ||
          !require(status_fits(status, viewport.width),
                   std::string(busy.description) + " stays in bounds at " +
                       viewport.name))
        passed = false;
    }
  }

  std::cout << "FileChooser " << language_tag
            << " storage operation feedback tests passed\n";
  return passed ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: storage-operation-feedback-test LANGUAGE_TAG\n";
    return 2;
  }
  return run(argv[1]);
}
