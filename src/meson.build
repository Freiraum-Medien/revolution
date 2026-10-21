blueprints = custom_target('blueprints',
  input: files('window.blp'),
  output: 'window.ui',
  command: [find_program('blueprint-compiler'), 'compile', '--output', '@OUTPUT@', '@INPUT@'],
)

resources = gnome.compile_resources('revolution-resources',
  'revolution.gresource.xml',
  c_name: 'revolution',
  source_dir: meson.current_build_dir(),
  dependencies: blueprints,
)

executable('revolution',
  'main.c', resources,
  dependencies: [
    dependency('gtk4', version: '>= 4.22'),
    dependency('libadwaita-1', version: '>= 1.9'),
  ],
  include_directories: include_directories('..'),
  install: true,
)
