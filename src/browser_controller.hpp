#pragma once

#include <plainroot/toolkit/toolkit.h>
#include <plainroot/ui/ui.h>

#include <memory>
#include <string>

namespace plainroot::filechooser {

class BrowserController {
public:
  using CompletionFn = void (*)(void *user_data, bool accepted,
                                const char *native_path);

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
  void set_accept_label(std::string label);
  void navigate(std::string path);
  void cancel();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace plainroot::filechooser
