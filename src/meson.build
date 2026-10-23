blueprints = []
foreach blp : ['window.blp', 'mail/mail-view.blp', 'mail/mail-reader.blp', 'composer/composer.blp', 'contacts/contacts-view.blp', 'calendar/calendar-view.blp']
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
  'eds-compat.c',
  'composer/composer.c',
  'mail/mail-account.c',
  'mail/mail-folder.c',
  'mail/mail-message.c',
  'mail/mail-reader.c',
  'mail/mail-session.c',
  'mail/mail-view.c',
  'contacts/contacts-sample.c',
  'contacts/contacts-view.c',
  'calendar/calendar-sample.c',
  'calendar/calendar-view.c',
  resources,
  dependencies: [
    dependency('gtk4', version: '>= 4.22'),
    dependency('libadwaita-1', version: '>= 1.9'),
    # Unterbau laut #342, genutzt ab #343 (EDS 3.56.2, WebKitGTK für #354).
    dependency('camel-1.2', version: '>= 3.56'),
    dependency('libedataserver-1.2', version: '>= 3.56'),
    dependency('webkitgtk-6.0', version: '>= 2.52'),
  ],
  include_directories: include_directories('..'),
  install: true,
)

subdir('web-extension')
