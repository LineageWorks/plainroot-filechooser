#pragma once

#include <plainroot/toolkit/toolkit.h>
#include <plainroot/ui/ui.h>

#include <memory>
#include <string>

namespace plainroot::filechooser {

class BrowserController {
public:
  BrowserController(plainroot_toolkit_application *application,
                    plainroot_ui_context *context);
  BrowserController(const BrowserController &) = delete;
  BrowserController &operator=(const BrowserController &) = delete;
  ~BrowserController();

  bool ready() const;
  void shutdown();
  bool create_ui();
  void set_window(plainroot_toolkit_window *window);
  void navigate(std::string path);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace plainroot::filechooser
