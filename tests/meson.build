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

# #416: Verschieben und Papierkorb gegen ein Testkonto (kein meson test, braucht GOA und Netz).
# Aufruf und Prüfung: tests/verschieben.sh, README „eds-compat.c“.
executable('verschieben',
  'verschieben.c',
  '../src/eds-compat.c',
  '../src/mail/mail-account.c',
  '../src/mail/mail-session.c',
  dependencies: [
    dependency('camel-1.2', version: '>= 3.56'),
    dependency('libedataserver-1.2', version: '>= 3.56'),
  ],
  include_directories: include_directories('..', '../src'),
)

# #349: Smoke-Test gegen die echten Konten (braucht Netz, GOA und den Session-Bus), daher eigene
# Suite, die der Standardlauf auslässt (add_test_setup in meson.build); Aufruf:
# meson test -C build --suite smoke -v. Ablauf und Prüfungen: tests/smoke.sh, README.
test('smoke', find_program('smoke.sh'),
  suite: 'smoke',
  env: ['BUILD=' + meson.project_build_root()],
  is_parallel: false,
  timeout: 1200,
)
