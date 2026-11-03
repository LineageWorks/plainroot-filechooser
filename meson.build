project(
  'plainroot-filechooser',
  ['cpp'],
  version: '0.1.0',
  license: 'LGPL-3.0-or-later',
  license_files: ['LICENSE', 'COPYING'],
  meson_version: '>= 1.3.0',
  default_options: [
    'warning_level=3',
    'cpp_std=c++23',
  ],
)

toolkit_dep = dependency('plainroot-toolkit', version: '>= 0.21.0')
core_dep = dependency('plainroot-core', version: '>= 0.8.0')
i18n_dep = dependency('plainroot-i18n', version: '>= 0.1.0')
storage_dep = dependency('plainroot-storage-client', version: '>= 0.1.0')
network_storage_dep = dependency(
  'plainroot-storage-network-client',
  version: '>= 0.1.0',
)
glib_dep = dependency('glib-2.0', version: '>= 2.66')
magic_dep = dependency('libmagic', version: '>= 5.39')
threads_dep = dependency('threads')
systemd_dep = dependency('libsystemd')

browser_sources = [
  'src/bookmarks.cpp',
  'src/browser_controller.cpp',
  'src/directory_breadcrumbs.cpp',
  'src/directory_sort.cpp',
  'src/directory_loader.cpp',
  'src/mime_database.cpp',
  'src/recent_files.cpp',
  'src/open_filename.cpp',
  'src/storage_bridge.cpp',
  'src/storage_locations_view.cpp',
  'src/ui_context_factory.cpp',
]

filechooser_common = static_library(
  'plainroot-filechooser-common',
  browser_sources,
  dependencies: [
    toolkit_dep,
    core_dep,
    i18n_dep,
    storage_dep,
    network_storage_dep,
    glib_dep,
    magic_dep,
    threads_dep,
  ],
  install: false,
)

common_dependencies = [
  toolkit_dep,
  core_dep,
  i18n_dep,
  storage_dep,
  network_storage_dep,
  glib_dep,
  magic_dep,
  threads_dep,
]

executable(
  'plainroot-filechooser-browser',
  'src/main.cpp',
  link_with: filechooser_common,
  dependencies: common_dependencies,
  install: true,
)

executable(
  'plainroot-filechooser-portal',
  [
    'src/portal_main.cpp',
    'src/portal_backend.cpp',
    'src/portal_choices.cpp',
    'src/portal_response.cpp',
    'src/save_files.cpp',
  ],
  link_with: filechooser_common,
  dependencies: common_dependencies + [systemd_dep],
  install_dir: get_option('libexecdir'),
  install: true,
)

portal_service_config = configuration_data()
portal_service_config.set(
  'LIBEXECDIR',
  join_paths(get_option('prefix'), get_option('libexecdir')),
)
portal_dbus_service = configure_file(
  input: 'data/org.freedesktop.impl.portal.desktop.plainroot.service.in',
  output: 'org.freedesktop.impl.portal.desktop.plainroot.service',
  configuration: portal_service_config,
)
portal_user_service = configure_file(
  input: 'data/plainroot-filechooser-portal.service.in',
  output: 'plainroot-filechooser-portal.service',
  configuration: portal_service_config,
)
if get_option('portal_registration')
  install_data(
    'data/plainroot.portal',
    install_dir: get_option('datadir') / 'xdg-desktop-portal/portals',
  )
  install_data(
    portal_dbus_service,
    install_dir: get_option('datadir') / 'dbus-1/services',
  )
  install_data(
    portal_user_service,
    install_dir: get_option('prefix') / 'lib/systemd/user',
  )
  install_data(
    'data/plainrootde-portals.conf',
    install_dir: get_option('datadir') / 'xdg-desktop-portal',
  )
else
  # Keep the prototype out of the desktop's active portal set until its live
  # broker behavior and GTK capability parity have been accepted.
  message('Portal registration is disabled; use -Dportal_registration=true to install it')
endif

directory_sort_test = executable(
  'filechooser-directory-sort-test',
  [
    'tests/directory_sort_test.cpp',
    'src/directory_sort.cpp',
  ],
  dependencies: [core_dep, toolkit_dep, glib_dep],
  install: false,
)
test('FileChooser directory sorting', directory_sort_test)

directory_breadcrumbs_test = executable(
  'filechooser-directory-breadcrumbs-test',
  [
    'tests/directory_breadcrumbs_test.cpp',
    'src/directory_breadcrumbs.cpp',
  ],
  install: false,
)
test('FileChooser directory breadcrumbs', directory_breadcrumbs_test)

mime_database_test = executable(
  'filechooser-mime-database-test',
  [
    'tests/mime_database_test.cpp',
    'src/mime_database.cpp',
  ],
  dependencies: [glib_dep, magic_dep],
  install: false,
)
test('FileChooser MIME database', mime_database_test)

recent_files_test = executable(
  'filechooser-recent-files-test',
  [
    'tests/recent_files_test.cpp',
    'src/recent_files.cpp',
    'src/bookmarks.cpp',
  ],
  dependencies: [core_dep, glib_dep],
  install: false,
)
test('FileChooser XBEL recent files', recent_files_test)

open_filename_test = executable(
  'filechooser-open-filename-test',
  [
    'tests/open_filename_test.cpp',
    'src/open_filename.cpp',
  ],
  dependencies: [glib_dep],
  install: false,
)
test('FileChooser typed open filename', open_filename_test)

portal_choices_test = executable(
  'filechooser-portal-choices-test',
  [
    'tests/portal_choices_test.cpp',
    'src/portal_choices.cpp',
  ],
  dependencies: [systemd_dep],
  install: false,
)
dbus_run_session = find_program('dbus-run-session')
test(
  'FileChooser portal choices contract',
  dbus_run_session,
  args: ['--', portal_choices_test.full_path()],
)

portal_save_files_test = executable(
  'filechooser-portal-save-files-test',
  [
    'tests/portal_save_files_test.cpp',
    'src/save_files.cpp',
  ],
  dependencies: [systemd_dep],
  install: false,
)
test(
  'FileChooser portal SaveFiles contract',
  dbus_run_session,
  args: ['--', portal_save_files_test.full_path()],
)

portal_response_test = executable(
  'filechooser-portal-response-test',
  [
    'tests/portal_response_test.cpp',
    'src/portal_choices.cpp',
    'src/portal_response.cpp',
  ],
  dependencies: [systemd_dep],
  install: false,
)
test(
  'FileChooser delayed portal response',
  dbus_run_session,
  args: ['--', portal_response_test.full_path()],
)

portal_limits_test = executable(
  'filechooser-portal-limits-test',
  'tests/portal_limits_test.cpp',
  install: false,
)
test('FileChooser portal request size limits', portal_limits_test)

# The portal target is deliberately not registered by default. Its request
# lifecycle and FileChooser parity must be validated before a portals.conf
# preference can route applications to it.

subdir('po')
