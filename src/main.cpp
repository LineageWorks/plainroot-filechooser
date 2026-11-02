#include "browser_controller.hpp"
#include "ui_context_factory.hpp"

#include <plainroot/i18n/i18n.h>
#include <plainroot/toolkit/toolkit.h>
#include <plainroot/ui/ui.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr char kDomain[] = "plainroot-filechooser";
using plainroot::filechooser::BrowserController;

} // namespace

int main(int argc, char **argv) {
  std::array<char, 512> error{};
  std::string start_path;
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::fprintf(stdout, "%s\n",
                 "Usage: plainroot-filechooser-browser [ABSOLUTE_DIRECTORY]");
    return 0;
  }
  if (argc > 2) {
    std::fputs("Usage: plainroot-filechooser-browser [ABSOLUTE_DIRECTORY]\n",
               stderr);
    return 2;
  }
  if (argc == 2) {
    start_path = argv[1];
    if (start_path.empty() || start_path.front() != '/') {
      std::fputs("The starting directory must be an absolute path.\n", stderr);
      return 2;
    }
  } else {
    const char *home = std::getenv("HOME");
    start_path = home != nullptr && home[0] == '/' ? home : "/";
  }

  plainroot_i18n_options_v1 i18n_options{};
  i18n_options.struct_size = sizeof(i18n_options);
  i18n_options.fallback_language_tag = "en";
  if (plainroot_i18n_initialize_v1(&i18n_options, error.data(), error.size()) !=
          PLAINROOT_I18N_RESULT_OK ||
      plainroot_i18n_bind_domain_v1(kDomain, nullptr, error.data(),
                                    error.size()) != PLAINROOT_I18N_RESULT_OK) {
    std::fprintf(stderr, "plainroot-filechooser-browser: %s\n",
                 error[0] == '\0' ? "unable to initialize localization"
                                  : error.data());
    return 1;
  }
  const char *language_tag = plainroot_i18n_language_tag_v1();
  if (language_tag == nullptr)
    language_tag = "en";

  plainroot_ui_context *context =
      plainroot::filechooser::make_ui_context(language_tag);
  if (context == nullptr) {
    std::fputs("plainroot-filechooser-browser: unable to create UI context\n",
               stderr);
    return 1;
  }

  plainroot_toolkit_application_options_v1 application_options{};
  application_options.struct_size = sizeof(application_options);
  plainroot_toolkit_application *application =
      plainroot_toolkit_application_create_v1(&application_options,
                                              error.data(), error.size());
  if (application == nullptr) {
    std::fprintf(stderr, "plainroot-filechooser-browser: %s\n", error.data());
    plainroot_ui_context_destroy(context);
    return 1;
  }

  BrowserController controller(application, context);
  if (!controller.ready() || !controller.create_ui()) {
    std::fputs("plainroot-filechooser-browser: unable to create browser UI\n",
               stderr);
    controller.shutdown();
    plainroot_toolkit_application_destroy(application);
    plainroot_ui_context_destroy(context);
    return 1;
  }

  plainroot_toolkit_window_options_v1 window_options{};
  window_options.struct_size = sizeof(window_options);
  window_options.title = "Plainroot FileChooser";
  window_options.app_id = "org.plainroot.FileChooserBrowser";
  window_options.initial_width = 1000;
  window_options.initial_height = 720;
  window_options.minimum_width = 320;
  window_options.minimum_height = 320;
  window_options.maximum_width = 1600;
  window_options.maximum_height = 1200;
  window_options.content_max_width = 1280.0F;
  window_options.ui_context = context;
  window_options.clear_color[0] = 0.055F;
  window_options.clear_color[1] = 0.065F;
  window_options.clear_color[2] = 0.085F;
  window_options.clear_color[3] = 1.0F;
  plainroot_toolkit_window *window = nullptr;
  if (plainroot_toolkit_window_create_v1(
          application, &window_options, nullptr, nullptr, &window, error.data(),
          error.size()) != PLAINROOT_TOOLKIT_RESULT_OK) {
    std::fprintf(stderr, "plainroot-filechooser-browser: %s\n", error.data());
    controller.shutdown();
    plainroot_toolkit_application_destroy(application);
    plainroot_ui_context_destroy(context);
    return 1;
  }
  controller.set_window(window);
  controller.navigate(std::move(start_path));

  const auto run_result = plainroot_toolkit_application_run_v1(
      application, error.data(), error.size());
  controller.shutdown();
  (void)plainroot_toolkit_window_destroy(window);
  plainroot_toolkit_application_destroy(application);
  plainroot_ui_context_destroy(context);
  if (run_result != PLAINROOT_TOOLKIT_RESULT_OK) {
    std::fprintf(stderr, "plainroot-filechooser-browser: %s\n",
                 error[0] == '\0' ? "application stopped with an error"
                                  : error.data());
    return 1;
  }
  return 0;
}
