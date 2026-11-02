#pragma once

#include <plainroot/toolkit/toolkit.h>
#include <plainroot/ui/ui.h>

#include "browser_choice.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace plainroot::filechooser {

struct BrowserFilterPattern {
  uint32_t type = 0;
  std::string pattern;
};

struct BrowserFilter {
  std::string name;
  std::vector<BrowserFilterPattern> patterns;
};

class BrowserController {
public:
  using CompletionFn = void (*)(void *user_data, bool accepted,
                                const std::vector<std::string> &native_paths,
                                const BrowserFilter *current_filter,
                                const std::vector<BrowserChoiceSelection>
                                    &choices);

  BrowserController(plainroot_toolkit_application *application,
                    plainroot_ui_context *context,
                    CompletionFn completion = nullptr,
                    void *completion_user_data = nullptr);
  BrowserController(const BrowserController &) = delete;
  BrowserController &operator=(const BrowserController &) = delete;
  ~BrowserController();

  bool ready() const;
  void shutdown();
  bool create_ui();
  void set_window(plainroot_toolkit_window *window);
  void set_title(std::string title);
  void set_select_directory(bool select_directory);
  void set_allow_multiple(bool allow_multiple);
  void set_filters(std::vector<BrowserFilter> filters,
                   int32_t selected_filter);
  void set_choices(std::vector<BrowserChoice> choices);
  void set_save_file(bool save_file);
  void set_save_files(std::vector<std::string> filenames);
  void set_initial_filename(std::string filename);
  void set_accept_label(std::string label);
  void navigate(std::string path);
  void cancel();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace plainroot::filechooser
