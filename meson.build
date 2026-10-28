project('revolution', 'c',
          version: '0.1.0',
          license: 'GPL-3.0-or-later',
    meson_version: '>= 1.0.0',
  default_options: [ 'warning_level=2', 'werror=false', 'c_std=gnu11' ],
)

# Profil wie GNOME Calendar 50.0 (quelltext/gnome-calendar/meson_options.txt:2,
# meson.build:36-52): „development“ ergibt die Entwicklungsvariante mit eigener App-ID.
base_id = 'de.freiraum_medien.Revolution'
if get_option('profile') == 'development'
  profile = 'Devel'
  application_id = base_id + '.Devel'
else
  profile = ''
  application_id = base_id
endif

i18n = import('i18n')
gnome = import('gnome')

conf = configuration_data()
conf.set_quoted('APPLICATION_ID', application_id)
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('GETTEXT_PACKAGE', meson.project_name())
conf.set_quoted('LOCALEDIR', get_option('prefix') / get_option('localedir'))
# Wie Stamp (quelltext/stamp/meson.build:22): DEVEL schaltet die Stilklasse „devel“.
conf.set('DEVEL', profile == 'Devel')
# #439: Daten- und Cache-Ordner je Profil (~/.local/share|.cache/revolution bzw. revolution-devel),
# damit Testläufe der Entwicklungsvariante die Daten der installierten App nicht berühren.
conf.set_quoted('USER_DIR_NAME', profile == 'Devel' ? 'revolution-devel' : 'revolution')
# #354: Web-Process-Extension des Lesebereichs (src/web-extension). Installiert unter libdir;
# aus dem Build-Ordner gestartet nimmt die App die Erweiterung im Build-Ordner (mail-reader.c).
web_extensions_dir = get_option('prefix') / get_option('libdir') / 'revolution' / 'web-extensions'
conf.set_quoted('WEB_EXTENSIONS_DIR', web_extensions_dir)
conf.set_quoted('BUILD_SRC_DIR', meson.project_build_root() / 'src')
# #438: aus dem Build-Ordner gestartet die dort gebauten Übersetzungen (po/<Sprache>/LC_MESSAGES, main.c).
conf.set_quoted('BUILD_LOCALE_DIR', meson.project_build_root() / 'po')
configure_file(output: 'config.h', configuration: conf)

subdir('data')
subdir('src')
subdir('po')
subdir('tests')

# #349: Standardlauf ohne die Suite „smoke“ (Netz, GOA); --suite smoke schließt sie ausdrücklich
# ein (meson mtest.py test_suitable: include_suites gehen vor add_test_setup).
add_test_setup('default', exclude_suites: ['smoke'], is_default: true)

gnome.post_install(
     glib_compile_schemas: true,
    gtk_update_icon_cache: true,
  update_desktop_database: true,
)
