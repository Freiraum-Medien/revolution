# #347: Prüfprogramm für die Anmeldung gegen die echten Server (kein meson test, braucht
# GOA-Konten und Netz). Aufruf und Fehlerfälle: tests/anmeldung.c, README „Anmeldung“.
executable('anmeldung',
  'anmeldung.c',
  '../src/mail/mail-account.c',
  '../src/mail/mail-session.c',
  dependencies: [
    dependency('camel-1.2', version: '>= 3.56'),
    dependency('libedataserver-1.2', version: '>= 3.56'),
  ],
  include_directories: include_directories('..', '../src'),
)
