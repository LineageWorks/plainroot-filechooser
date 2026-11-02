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
core_dep = dependency('plainroot-core', version: '>= 0.7.0')
i18n_dep = dependency('plainroot-i18n', version: '>= 0.1.0')
storage_dep = dependency('plainroot-storage-client', version: '>= 0.1.0')
glib_dep = dependency('glib-2.0', version: '>= 2.66')
threads_dep = dependency('threads')

executable(
  'plainroot-filechooser-browser',
  [
    'src/main.cpp',
    'src/browser_controller.cpp',
    'src/directory_loader.cpp',
    'src/storage_bridge.cpp',
    'src/storage_locations_view.cpp',
  ],
  dependencies: [toolkit_dep, core_dep, i18n_dep, storage_dep, glib_dep, threads_dep],
  install: true,
)

subdir('po')
