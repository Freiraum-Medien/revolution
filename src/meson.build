blueprints = []
foreach blp : ['window.blp', 'mail/mail-view.blp', 'mail/mail-reader.blp', 'mail/mail-conversation.blp', 'mail/mail-attachment.blp', 'composer/composer.blp', 'contacts/contacts-view.blp', 'contacts/contacts-detail.blp', 'contacts/contacts-editor.blp', 'calendar/calendar-view.blp']
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
  'composer/contact-completion.c',
  'mail/mail-account.c',
  'mail/mail-attachment.c',
  'mail/mail-conversation.c',
  'mail/mail-favorites.c',
  'mail/mail-folder.c',
  'mail/mail-message.c',
  'mail/mail-notification.c',
  'mail/mail-outbox.c',
  'mail/mail-reader.c',
  'mail/mail-search.c',
  'mail/mail-session.c',
  'mail/mail-thread.c',
  'mail/mail-view.c',
  'contacts/contacts-book.c',
  'contacts/contacts-contact.c',
  'contacts/contacts-detail.c',
  'contacts/contacts-editor.c',
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
    dependency('libebook-1.2', version: '>= 3.56'), # #361 Adressvervollständigung
    dependency('webkitgtk-6.0', version: '>= 2.52'),
  ],
  include_directories: include_directories('..'),
  install: true,
)

subdir('web-extension')
