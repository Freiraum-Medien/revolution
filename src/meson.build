blueprints = []
foreach blp : ['window.blp', 'mail/mail-view.blp']
  blueprints += custom_target(
    input: blp,
    output: '@BASENAME@.ui',
    command: [find_program('blueprint-compiler'), 'compile', '--output', '@OUTPUT@', '@INPUT@'],
  )
endforeach

resources = gnome.compile_resources('revolution-resources',
  'revolution.gresource.xml',
  c_name: 'revolution',
  source_dir: [meson.current_build_dir(), meson.current_source_dir()],
  dependencies: blueprints,
)

executable('revolution',
  'main.c',
  'mail/mail-sample.c',
  'mail/mail-view.c',
  resources,
  dependencies: [
    dependency('gtk4', version: '>= 4.22'),
    dependency('libadwaita-1', version: '>= 1.9'),
  ],
  include_directories: include_directories('..'),
  install: true,
)
