#include "storage_locations_view.hpp"

#include <plainroot/i18n/i18n.h>

#include <glib.h>

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
    status_ = make_label("Checking storage locations…", 0.0F);
    list_ = plainroot_ui_list_create_v1(context_, 0.0F);

    if (root_ == nullptr || heading_row == nullptr || heading_ == nullptr ||
        refresh == nullptr || status_ == nullptr || list_ == nullptr ||
        !add(root_, heading_row) || !add(heading_row, heading_) ||
        !add(heading_row, refresh) || !add(root_, status_) ||
        !add(root_, list_)) {
      root_ = nullptr;
      return nullptr;
    }
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

  static void refresh_activated(void *user_data, plainroot_ui_widget *) {
    auto &self = *static_cast<Impl *>(user_data);
    if (self.actions_.refresh)
      self.actions_.refresh();
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
      self.confirm_unmount_id_ = action.id;
      self.render();
      break;
    case ActionKind::confirm_unmount: {
      const std::string id = action.id;
      self.confirm_unmount_id_.clear();
      self.render();
      if (self.actions_.unmount)
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
      std::string label = display_path(mount.target);
      if (!mount.filesystem_type.empty())
        label += " · " + display_path(mount.filesystem_type);
      add_row(std::move(label), mount.target, mount.id, false, true, true);
    }
  }

  void add_row(std::string label, const std::string &open_path,
               const std::string &entity_id, bool read_only, bool mounted,
               bool label_is_ltr) {
    auto *row = plainroot_ui_list_item_create_v1(context_, 0.0F);
    auto *layout = plainroot_ui_adaptive_box_create_v1(
        context_, PLAINROOT_UI_AXIS_VERTICAL, PLAINROOT_UI_AXIS_HORIZONTAL,
        kExpandedLocationWidth, 8.0F, 0.0F);
    auto *text = plainroot_ui_label_create_v1(
        context_, label.c_str(), static_cast<uint32_t>(label.size()),
        PLAINROOT_UI_COLOR_ON_SURFACE_V3, 1.0F);
    auto *actions = plainroot_ui_box_create_v1(
        context_, PLAINROOT_UI_AXIS_HORIZONTAL, 8.0F, 0.0F);
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

    if (catalog_.complete) {
      if (mounted && !open_path.empty())
        add_action(actions, "Open", ActionKind::open, open_path, false);
      else if (!mounted)
        add_action(actions, "Mount", ActionKind::mount, entity_id, read_only);

      if (mounted && !entity_id.empty()) {
        const bool confirming = confirm_unmount_id_ == entity_id;
        add_action(actions, confirming ? "Confirm unmount" : "Unmount",
                   confirming ? ActionKind::confirm_unmount
                              : ActionKind::request_unmount,
                   entity_id, false);
        if (confirming)
          add_action(actions, "Cancel", ActionKind::cancel_unmount, entity_id,
                     false);
      }
    }

    rows_.push_back(row);
    if (!add(list_, row)) {
      rows_.pop_back();
      (void)plainroot_ui_widget_destroy_subtree_v1(row);
    }
  }

  void add_action(plainroot_ui_widget *parent, const char *title,
                  ActionKind kind, std::string id, bool read_only) {
    auto action = std::make_unique<Action>();
    action->view = this;
    action->kind = kind;
    action->id = std::move(id);
    action->read_only = read_only;
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
  plainroot_ui_widget *status_ = nullptr;
  plainroot_ui_widget *list_ = nullptr;
  StorageCatalog catalog_;
  bool have_catalog_ = false;
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
