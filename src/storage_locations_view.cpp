#include "storage_locations_view.hpp"
#include "trace.hpp"

#include <plainroot/i18n/i18n.h>

#include <glib.h>

#include <charconv>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace plainroot::filechooser::detail {
namespace {

constexpr char kDomain[] = "plainroot-filechooser";
constexpr float kExpandedLocationWidth = 560.0F;
constexpr char kLeftToRightIsolate[] = "\xE2\x81\xA6";
constexpr char kPopDirectionalIsolate[] = "\xE2\x81\xA9";

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

} // namespace

class StorageLocationsView::Impl {
public:
  enum class ActionKind {
    mount,
    open,
    request_unmount,
    confirm_unmount,
    cancel_unmount,
  };

  struct Action {
    Impl *view = nullptr;
    ActionKind kind = ActionKind::mount;
    std::string id;
    bool read_only = false;
    bool network = false;
  };

  Impl(plainroot_ui_context *context, StorageLocationActions actions)
      : context_(context), actions_(std::move(actions)) {}

  plainroot_ui_widget *create() {
    if (context_ == nullptr)
      return nullptr;
    root_ = plainroot_ui_box_create_v1(context_, PLAINROOT_UI_AXIS_VERTICAL,
                                       8.0F, 0.0F);
    auto *heading_row = plainroot_ui_adaptive_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, PLAINROOT_UI_AXIS_HORIZONTAL,
        kExpandedLocationWidth, 8.0F, 0.0F);
    heading_ = make_label("Locations", 1.0F);
    auto *refresh = make_button("Refresh", &Impl::refresh_activated, this);
    sftp_toggle_ = make_button("Connect to SFTP", &Impl::toggle_sftp_activated,
                               this);
    status_ = make_label("Checking storage locations…", 0.0F);
    list_ = plainroot_ui_list_create_v1(context_, 0.0F);
    create_sftp_form();

    if (root_ == nullptr || heading_row == nullptr || heading_ == nullptr ||
        refresh == nullptr || sftp_toggle_ == nullptr || status_ == nullptr ||
        list_ == nullptr || sftp_form_ == nullptr || host_field_ == nullptr ||
        username_field_ == nullptr || remote_path_field_ == nullptr ||
        port_field_ == nullptr ||
        connect_button_ == nullptr || cancel_button_ == nullptr ||
        !add(root_, heading_row) || !add(heading_row, heading_) ||
        !add(heading_row, refresh) || !add(heading_row, sftp_toggle_) ||
        !add(root_, sftp_form_) || !add(root_, status_) ||
        !add(root_, list_)) {
      root_ = nullptr;
      return nullptr;
    }
    (void)plainroot_ui_widget_set_visible_v1(sftp_form_, 0);
    return root_;
  }

  void set_catalog(StorageCatalog catalog) {
    catalog_ = std::move(catalog);
    have_catalog_ = true;
    confirm_unmount_id_.clear();
    if (list_ != nullptr)
      render();
  }

  void set_status(const std::string &status) { set_text(status_, status); }

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

  plainroot_ui_widget *make_button(const char *message, ButtonCallback callback,
                                   void *user_data) {
    const std::string text = tr(message);
    return plainroot_ui_button_create_v1(context_, text.c_str(),
                                         static_cast<uint32_t>(text.size()),
                                         0.0F, callback, user_data);
  }

  plainroot_ui_widget *make_sftp_field(const char *placeholder,
                                       const char *accessible_name,
                                       const char *initial = "",
                                       uint32_t max_bytes = 1024) {
    const std::string hint = tr(placeholder);
    const std::string initial_value(initial);
    auto *field = plainroot_ui_text_field_create_v1(
        context_, initial_value.c_str(),
        static_cast<uint32_t>(initial_value.size()), hint.c_str(),
        static_cast<uint32_t>(hint.size()), max_bytes, 1.0F, nullptr, nullptr);
    if (field != nullptr) {
      const std::string name = tr(accessible_name);
      (void)plainroot_ui_widget_set_accessible_name_v1(
          field, name.c_str(), static_cast<uint32_t>(name.size()));
      (void)plainroot_ui_widget_set_text_direction_v1(field,
                                                      PLAINROOT_UI_TEXT_LTR);
    }
    return field;
  }

  void create_sftp_form() {
    sftp_form_ = plainroot_ui_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, 8.0F, 0.0F);
    auto *endpoint_row = plainroot_ui_adaptive_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, PLAINROOT_UI_AXIS_HORIZONTAL,
        kExpandedLocationWidth, 8.0F, 0.0F);
    host_field_ = make_sftp_field("Server name or address", "Server address");
    username_field_ = make_sftp_field("Username (optional)", "Username");
    port_field_ = make_sftp_field("Port (default 22)", "Port", "22", 5);
    auto *path_row = plainroot_ui_box_create_v1(
        context_, PLAINROOT_UI_AXIS_HORIZONTAL, 8.0F, 0.0F);
    remote_path_field_ = make_sftp_field("Remote folder (optional)",
                                          "Remote folder");
    auto *actions = plainroot_ui_adaptive_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, PLAINROOT_UI_AXIS_HORIZONTAL,
        kExpandedLocationWidth, 8.0F, 0.0F);
    connect_button_ = make_button("Connect", &Impl::submit_sftp_activated,
                                  this);
    cancel_button_ = make_button("Cancel", &Impl::cancel_sftp_activated,
                                 this);
    if (connect_button_ != nullptr)
      (void)plainroot_ui_button_set_style_v1(
          connect_button_, PLAINROOT_UI_BUTTON_STYLE_FILLED);
    if (sftp_form_ == nullptr || endpoint_row == nullptr ||
        host_field_ == nullptr || username_field_ == nullptr ||
        port_field_ == nullptr ||
        path_row == nullptr ||
        remote_path_field_ == nullptr || actions == nullptr ||
        connect_button_ == nullptr || cancel_button_ == nullptr)
      return;
    const bool attached =
        add(endpoint_row, host_field_) && add(endpoint_row, username_field_) &&
        add(endpoint_row, port_field_) &&
        add(path_row, remote_path_field_) && add(actions, connect_button_) &&
        add(actions, cancel_button_) && add(sftp_form_, endpoint_row) &&
        add(sftp_form_, path_row) && add(sftp_form_, actions);
    if (!attached)
      sftp_form_ = nullptr;
  }

  static std::string field_value(const plainroot_ui_widget *field) {
    const char *text = nullptr;
    uint32_t length = 0;
    if (field == nullptr ||
        plainroot_ui_text_field_get_text_v1(field, &text, &length) !=
            PLAINROOT_UI_RESULT_OK ||
        (text == nullptr && length != 0U))
      return {};
    return std::string(text == nullptr ? "" : text, length);
  }

  static void refresh_activated(void *user_data, plainroot_ui_widget *) {
    auto &self = *static_cast<Impl *>(user_data);
    if (self.actions_.refresh)
      self.actions_.refresh();
    self.request_redraw();
  }

  static void toggle_sftp_activated(void *user_data, plainroot_ui_widget *) {
    auto &self = *static_cast<Impl *>(user_data);
    self.show_sftp_form_ = !self.show_sftp_form_;
    trace("sftp-form visible=%u", self.show_sftp_form_ ? 1U : 0U);
    (void)plainroot_ui_widget_set_visible_v1(
        self.sftp_form_, self.show_sftp_form_ ? 1 : 0);
    set_text(self.sftp_toggle_,
             tr(self.show_sftp_form_ ? "Hide SFTP form" : "Connect to SFTP"));
    self.request_redraw();
  }

  static void submit_sftp_activated(void *user_data, plainroot_ui_widget *) {
    auto &self = *static_cast<Impl *>(user_data);
    std::string host = field_value(self.host_field_);
    if (host.empty()) {
      trace("sftp-submit rejected=empty-host");
      self.set_status(tr("Enter a server name or address."));
      self.request_redraw();
      return;
    }
    const std::string username = field_value(self.username_field_);
    const std::string port_text = field_value(self.port_field_);
    uint16_t port = 22;
    if (!port_text.empty()) {
      uint32_t parsed_port = 0;
      const auto [end, error] = std::from_chars(
          port_text.data(), port_text.data() + port_text.size(), parsed_port);
      if (error != std::errc{} || end != port_text.data() + port_text.size() ||
          parsed_port == 0 || parsed_port > 65535U) {
        trace("sftp-submit rejected=invalid-port");
        self.set_status(tr("Enter a valid port from 1 to 65535."));
        self.request_redraw();
        return;
      }
      port = static_cast<uint16_t>(parsed_port);
    }
    const std::string remote_path = field_value(self.remote_path_field_);
    trace("sftp-submit begin");
    const auto result = self.actions_.connect_sftp
                            ? self.actions_.connect_sftp(
                                  std::move(host), username, port,
                                  remote_path)
                            : PLAINROOT_STORAGE_NOT_READY;
    trace("sftp-submit result=%u", static_cast<unsigned>(result));
    if (result == PLAINROOT_STORAGE_OK) {
      self.show_sftp_form_ = false;
      (void)plainroot_ui_widget_set_visible_v1(self.sftp_form_, 0);
      set_text(self.sftp_toggle_, tr("Connect to SFTP"));
    }
    self.request_redraw();
  }

  static void cancel_sftp_activated(void *user_data, plainroot_ui_widget *) {
    auto &self = *static_cast<Impl *>(user_data);
    self.show_sftp_form_ = false;
    (void)plainroot_ui_widget_set_visible_v1(self.sftp_form_, 0);
    set_text(self.sftp_toggle_, tr("Connect to SFTP"));
    self.request_redraw();
  }

  static void action_activated(void *user_data, plainroot_ui_widget *) {
    auto &action = *static_cast<Action *>(user_data);
    Impl &self = *action.view;
    switch (action.kind) {
    case ActionKind::mount:
      if (self.actions_.mount)
        self.actions_.mount(action.id, action.read_only);
      break;
    case ActionKind::open:
      if (self.actions_.open)
        self.actions_.open(action.id);
      break;
    case ActionKind::request_unmount:
      self.confirm_unmount_id_ =
          (action.network ? "network:" : "local:") + action.id;
      self.render();
      break;
    case ActionKind::confirm_unmount: {
      const std::string id = action.id;
      const bool network = action.network;
      self.confirm_unmount_id_.clear();
      self.render();
      if (network && self.actions_.disconnect_network)
        self.actions_.disconnect_network(id);
      else if (!network && self.actions_.unmount)
        self.actions_.unmount(id);
    } break;
    case ActionKind::cancel_unmount:
      self.confirm_unmount_id_.clear();
      self.render();
      break;
    }
    self.request_redraw();
  }

  static void set_text(plainroot_ui_widget *widget, const std::string &value) {
    if (widget != nullptr)
      (void)plainroot_ui_widget_set_text_v1(
          widget, value.c_str(), static_cast<uint32_t>(value.size()));
  }

  void request_redraw() {
    if (actions_.request_redraw)
      actions_.request_redraw();
  }

  void render() {
    for (plainroot_ui_widget *row : rows_) {
      (void)plainroot_ui_widget_remove_child_v1(list_, row);
      (void)plainroot_ui_widget_destroy_subtree_v1(row);
    }
    rows_.clear();
    row_actions_.clear();
    if (!have_catalog_)
      return;

    for (const StorageVolume &volume : catalog_.removable_volumes) {
      const bool path_label = volume.label.empty();
      std::string label = path_label ? display_path(volume.device_node)
                                     : display_path(volume.label);
      if (!volume.filesystem_type.empty())
        label += " · " + display_path(volume.filesystem_type);
      if (volume.mounted && !volume.mount_target.empty())
        label += " — " + std::string(kLeftToRightIsolate) +
                 display_path(volume.mount_target) + kPopDirectionalIsolate;
      add_row(std::move(label),
              volume.mounted ? volume.mount_target : std::string{},
              volume.mounted ? volume.mount_id : volume.id, volume.read_only,
              volume.mounted, path_label);
    }
    for (const NetworkMount &mount : catalog_.network_mounts) {
      std::string label = mount.host.empty() ? display_path(mount.target)
                                             : display_path(mount.host);
      if (!mount.username.empty())
        label = display_path(mount.username) + "@" + label;
      if (!mount.remote_path.empty())
        label += ":" + display_path(mount.remote_path);
      label += " — " + std::string(kLeftToRightIsolate) +
               display_path(mount.target) + kPopDirectionalIsolate;
      if (!mount.filesystem_type.empty())
        label += " · " + display_path(mount.filesystem_type);
      add_row(std::move(label), mount.target, mount.id, false, true, true,
              true, mount.managed);
    }
  }

  void add_row(std::string label, const std::string &open_path,
               const std::string &entity_id, bool read_only, bool mounted,
               bool label_is_ltr, bool network = false,
               bool managed_network = false) {
    auto *row = plainroot_ui_list_item_create_v1(context_, 0.0F);
    auto *layout = plainroot_ui_adaptive_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, PLAINROOT_UI_AXIS_HORIZONTAL,
        kExpandedLocationWidth, 8.0F, 1.0F);
    auto *text = plainroot_ui_label_create_v1(
        context_, label.c_str(), static_cast<uint32_t>(label.size()),
        PLAINROOT_UI_COLOR_ON_SURFACE_V3, 1.0F);
    auto *actions = plainroot_ui_adaptive_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, PLAINROOT_UI_AXIS_HORIZONTAL,
        kExpandedLocationWidth, 8.0F, 0.0F);
    if (text != nullptr && label_is_ltr)
      (void)plainroot_ui_widget_set_text_direction_v1(text,
                                                      PLAINROOT_UI_TEXT_LTR);
    if (row == nullptr || layout == nullptr || text == nullptr ||
        actions == nullptr || !add(row, layout) || !add(layout, text) ||
        !add(layout, actions)) {
      if (row != nullptr)
        (void)plainroot_ui_widget_destroy_subtree_v1(row);
      return;
    }

    if (mounted && !open_path.empty())
      add_action(actions, "Open", ActionKind::open, open_path, false);

    if (!network && catalog_.complete) {
      if (!mounted)
        add_action(actions, "Mount", ActionKind::mount, entity_id, read_only);

      if (mounted && !entity_id.empty()) {
        const std::string confirmation_key = "local:" + entity_id;
        const bool confirming = confirm_unmount_id_ == confirmation_key;
        add_action(actions, confirming ? "Confirm unmount" : "Unmount",
                   confirming ? ActionKind::confirm_unmount
                              : ActionKind::request_unmount,
                   entity_id, false);
        if (confirming)
          add_action(actions, "Cancel", ActionKind::cancel_unmount, entity_id,
                     false);
      }
    } else if (network && catalog_.network_complete && managed_network &&
               !entity_id.empty()) {
      const std::string confirmation_key = "network:" + entity_id;
      const bool confirming = confirm_unmount_id_ == confirmation_key;
      add_action(actions, confirming ? "Confirm disconnect" : "Disconnect",
                 confirming ? ActionKind::confirm_unmount
                            : ActionKind::request_unmount,
                 entity_id, false, true);
      if (confirming)
        add_action(actions, "Cancel", ActionKind::cancel_unmount, entity_id,
                   false, true);
    }

    rows_.push_back(row);
    if (!add(list_, row)) {
      rows_.pop_back();
      (void)plainroot_ui_widget_destroy_subtree_v1(row);
    }
  }

  void add_action(plainroot_ui_widget *parent, const char *title,
                  ActionKind kind, std::string id, bool read_only,
                  bool network = false) {
    auto action = std::make_unique<Action>();
    action->view = this;
    action->kind = kind;
    action->id = std::move(id);
    action->read_only = read_only;
    action->network = network;
    auto *button = make_button(title, &Impl::action_activated, action.get());
    if (button == nullptr || !add(parent, button))
      return;
    if (kind == ActionKind::confirm_unmount)
      (void)plainroot_ui_button_set_style_v1(button,
                                             PLAINROOT_UI_BUTTON_STYLE_FILLED);
    row_actions_.push_back(std::move(action));
  }

  plainroot_ui_context *context_ = nullptr;
  StorageLocationActions actions_;
  plainroot_ui_widget *root_ = nullptr;
  plainroot_ui_widget *heading_ = nullptr;
  plainroot_ui_widget *sftp_toggle_ = nullptr;
  plainroot_ui_widget *sftp_form_ = nullptr;
  plainroot_ui_widget *host_field_ = nullptr;
  plainroot_ui_widget *username_field_ = nullptr;
  plainroot_ui_widget *port_field_ = nullptr;
  plainroot_ui_widget *remote_path_field_ = nullptr;
  plainroot_ui_widget *connect_button_ = nullptr;
  plainroot_ui_widget *cancel_button_ = nullptr;
  plainroot_ui_widget *status_ = nullptr;
  plainroot_ui_widget *list_ = nullptr;
  StorageCatalog catalog_;
  bool have_catalog_ = false;
  bool show_sftp_form_ = false;
  std::string confirm_unmount_id_;
  std::vector<plainroot_ui_widget *> rows_;
  std::vector<std::unique_ptr<Action>> row_actions_;
};

StorageLocationsView::StorageLocationsView(plainroot_ui_context *context,
                                           StorageLocationActions actions)
    : impl_(std::make_unique<Impl>(context, std::move(actions))) {}

StorageLocationsView::~StorageLocationsView() = default;

plainroot_ui_widget *StorageLocationsView::create() {
  return impl_ ? impl_->create() : nullptr;
}

void StorageLocationsView::set_catalog(StorageCatalog catalog) {
  if (impl_)
    impl_->set_catalog(std::move(catalog));
}

void StorageLocationsView::set_status(const std::string &status) {
  if (impl_)
    impl_->set_status(status);
}

} // namespace plainroot::filechooser::detail
