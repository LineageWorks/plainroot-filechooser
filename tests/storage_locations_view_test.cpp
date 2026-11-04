#include "../src/storage_locations_view.hpp"
#include "../src/ui_context_factory.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

namespace {

using plainroot::filechooser::detail::NetworkMount;
using plainroot::filechooser::detail::StorageCatalog;
using plainroot::filechooser::detail::StorageLocationActions;
using plainroot::filechooser::detail::StorageLocationsView;

bool require(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

bool layout(plainroot_ui_context *context, float width, float height) {
  plainroot_ui_rect_v1 bounds{};
  bounds.struct_size = sizeof(bounds);
  bounds.width = width;
  bounds.height = height;
  const auto result = plainroot_ui_context_layout_v1(context, &bounds);
  if (result != PLAINROOT_UI_RESULT_OK)
    std::cerr << "Layout failed at " << width << 'x' << height
              << " with UI result " << static_cast<int>(result) << '\n';
  return result == PLAINROOT_UI_RESULT_OK;
}

plainroot_ui_rect_v1 widget_bounds(plainroot_ui_widget *widget) {
  plainroot_ui_rect_v1 bounds{};
  bounds.struct_size = sizeof(bounds);
  if (plainroot_ui_widget_bounds_v1(widget, &bounds) != PLAINROOT_UI_RESULT_OK)
    bounds.width = -1.0F;
  return bounds;
}

bool fits_width(const plainroot_ui_widget *widget, float width) {
  const auto bounds = widget_bounds(const_cast<plainroot_ui_widget *>(widget));
  return bounds.width > 0.0F && bounds.x >= 0.0F &&
         bounds.x + bounds.width <= width + 0.5F;
}

plainroot_ui_event_v1 pointer_event(uint32_t type, float x, float y) {
  plainroot_ui_event_v1 event{};
  event.struct_size = sizeof(event);
  event.type = type;
  event.x = x;
  event.y = y;
  return event;
}

const plainroot_ui_widget *find_widget(const plainroot_ui_widget *parent,
                                       const char *name) {
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

bool click(plainroot_ui_context *context, const plainroot_ui_widget *widget) {
  auto *mutable_widget = const_cast<plainroot_ui_widget *>(widget);
  const auto rect = widget_bounds(mutable_widget);
  if (rect.width <= 0.0F || rect.height <= 0.0F)
    return false;
  const float x = rect.x + rect.width * 0.5F;
  const float y = rect.y + rect.height * 0.5F;
  auto down = pointer_event(PLAINROOT_UI_EVENT_POINTER_DOWN, x, y);
  auto up = pointer_event(PLAINROOT_UI_EVENT_POINTER_UP, x, y);
  return plainroot_ui_context_handle_event_v1(context, &down) ==
             PLAINROOT_UI_RESULT_OK &&
         plainroot_ui_context_handle_event_v1(context, &up) ==
             PLAINROOT_UI_RESULT_OK;
}

bool set_text(const plainroot_ui_widget *widget, const char *text) {
  return widget != nullptr &&
         plainroot_ui_widget_set_text_v1(
             const_cast<plainroot_ui_widget *>(widget), text,
             static_cast<uint32_t>(std::strlen(text))) == PLAINROOT_UI_RESULT_OK;
}

} // namespace

int main() {
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
  std::unique_ptr<plainroot_ui_context, decltype(&plainroot_ui_context_destroy)>
      context(plainroot::filechooser::make_ui_context("en"),
              &plainroot_ui_context_destroy);
  if (!require(context != nullptr, "UI context creation"))
    return 1;

  std::string submitted_host;
  std::string submitted_path;
  std::string disconnected_mount_id;
  unsigned redraws = 0;
  StorageLocationActions actions;
  actions.connect_sftp = [&](std::string host, std::string, uint16_t,
                             std::string path) {
    submitted_host = std::move(host);
    submitted_path = std::move(path);
    return PLAINROOT_STORAGE_OK;
  };
  actions.disconnect_network = [&](std::string id) {
    disconnected_mount_id = std::move(id);
  };
  actions.request_redraw = [&] { ++redraws; };

  StorageLocationsView view(context.get(), std::move(actions));
  auto *root = view.create();
  if (!require(root != nullptr, "storage locations view creation") ||
      !require(plainroot_ui_context_set_root_v1(context.get(), root) ==
                   PLAINROOT_UI_RESULT_OK,
               "storage view root installation") ||
      !require(layout(context.get(), 1200.0F, 800.0F),
               "initial storage view layout")) {
    return 1;
  }

  const auto *toggle = find_widget(root, "Connect to SFTP");
  if (!require(toggle != nullptr, "SFTP form toggle is discoverable") ||
      !require(click(context.get(), toggle), "open SFTP form")) {
    return 1;
  }

  for (const Viewport &viewport : viewports) {
    const auto *server = find_widget(root, "Server address");
    const auto *remote = find_widget(root, "Remote folder");
    const auto *connect = find_widget(root, "Connect");
    if (!require(layout(context.get(), viewport.width, viewport.height),
                 "layout SFTP form at device viewport") ||
        !require(fits_width(server, viewport.width),
                 "server field remains within viewport width") ||
        !require(fits_width(remote, viewport.width),
                 "remote path field remains within viewport width") ||
        !require(fits_width(connect, viewport.width),
                 "connect button remains within viewport width")) {
      std::cerr << "Failed SFTP form viewport: " << viewport.name << '\n';
      return 1;
    }
  }

  const auto *host = find_widget(root, "Server address");
  const auto *remote_path = find_widget(root, "Remote folder");
  const auto *connect = find_widget(root, "Connect");
  if (!require(set_text(host, "localhost"), "set SFTP host") ||
      !require(set_text(remote_path, "/etc"), "set remote folder") ||
      !require(connect != nullptr, "Connect button is discoverable") ||
      !require(click(context.get(), connect), "submit SFTP form and hide it") ||
      !require(submitted_host == "localhost" && submitted_path == "/etc",
               "submit the entered SFTP endpoint") ||
      !require(layout(context.get(), viewports.back().width,
                      viewports.back().height),
               "layout after hiding submitted SFTP form")) {
    return 1;
  }

  StorageCatalog catalog;
  catalog.complete = false;
  catalog.network_complete = true;
  catalog.revision = 1;
  NetworkMount mount;
  mount.id = "test-mount";
  mount.target = "/run/user/1000/plainroot/network/test-mount";
  mount.filesystem_type = "fuse.sshfs";
  mount.host = "localhost";
  mount.username = "alexin";
  mount.remote_path = "/etc";
  mount.managed = true;
  catalog.network_mounts.push_back(std::move(mount));
  view.set_catalog(std::move(catalog));

  for (const Viewport &viewport : viewports) {
    if (!require(layout(context.get(), viewport.width, viewport.height),
                 "layout mounted network row at device viewport") ||
        !require(fits_width(find_widget(root, "Disconnect"), viewport.width),
                 "disconnect action remains within viewport width")) {
      std::cerr << "Failed mounted-share viewport: " << viewport.name << '\n';
      return 1;
    }
  }

  const auto *disconnect = find_widget(root, "Disconnect");
  if (!require(disconnect != nullptr, "managed network mount has Disconnect") ||
      !require(click(context.get(), disconnect),
               "request disconnect confirmation")) {
    return 1;
  }

  for (const Viewport &viewport : viewports) {
    if (!require(layout(context.get(), viewport.width, viewport.height),
                 "layout disconnect confirmation at device viewport") ||
        !require(fits_width(find_widget(root, "Confirm disconnect"),
                            viewport.width),
                 "disconnect confirmation remains within viewport width")) {
      std::cerr << "Failed disconnect confirmation viewport: "
                << viewport.name << '\n';
      return 1;
    }
  }

  const auto *confirm_disconnect = find_widget(root, "Confirm disconnect");
  if (!require(confirm_disconnect != nullptr,
               "network disconnect confirmation is discoverable") ||
      !require(click(context.get(), confirm_disconnect),
               "confirm network disconnect") ||
      !require(disconnected_mount_id == "test-mount",
               "disconnect callback receives the managed mount ID")) {
    return 1;
  }

  StorageCatalog disconnected_catalog;
  disconnected_catalog.complete = false;
  disconnected_catalog.network_complete = true;
  disconnected_catalog.revision = 2;
  view.set_catalog(std::move(disconnected_catalog));
  for (const Viewport &viewport : viewports) {
    if (!require(layout(context.get(), viewport.width, viewport.height),
                 "layout after disconnected snapshot at device viewport") ||
        !require(find_widget(root, "Disconnect") == nullptr,
                 "disconnected share action is removed")) {
      std::cerr << "Failed disconnected-share viewport: " << viewport.name
                << '\n';
      return 1;
    }
  }

  auto *commands = plainroot_render_command_list_v2_create();
  const bool emitted = commands != nullptr &&
                       plainroot_ui_context_emit_v1(context.get(), commands) ==
                           PLAINROOT_UI_RESULT_OK;
  if (commands != nullptr)
    plainroot_render_command_list_v2_destroy(commands);
  if (!require(emitted, "emit updated storage view") ||
      !require(redraws > 0, "storage view requested redraws")) {
    return 1;
  }

  std::cout << "FileChooser storage locations view tests passed\n";
  return 0;
}
