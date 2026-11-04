#pragma once

#include "storage_bridge.hpp"

#include <plainroot/ui/ui.h>

#include <functional>
#include <memory>
#include <string>

namespace plainroot::filechooser::detail {

struct StorageLocationActions {
  std::function<void(std::string, bool)> mount;
  std::function<void(std::string)> open;
  std::function<void(std::string)> unmount;
  std::function<void(std::string)> disconnect_network;
  std::function<plainroot_storage_result_v1(std::string, std::string, uint16_t,
                                           std::string)>
      connect_sftp;
  std::function<void()> refresh;
  std::function<void()> request_redraw;
};

class StorageLocationsView {
public:
  StorageLocationsView(plainroot_ui_context *context,
                       StorageLocationActions actions);
  StorageLocationsView(const StorageLocationsView &) = delete;
  StorageLocationsView &operator=(const StorageLocationsView &) = delete;
  ~StorageLocationsView();

  plainroot_ui_widget *create();
  void set_catalog(StorageCatalog catalog);
  void set_status(const std::string &status);
  void set_sftp_form_visible(bool visible);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace plainroot::filechooser::detail
