# Vorlage: quelltext/stamp/data/meson.build (Desktop-Datei, Metainfo, Schema, Tests)
# und quelltext/gnome-calendar/data/meson.build:18-30 (App-ID je Profil per configure_file).
id_conf = configuration_data()
id_conf.set('APPLICATION_ID', application_id)

desktop_file = i18n.merge_file(
        input: configure_file(
                       input: base_id + '.desktop.in.in',
                      output: application_id + '.desktop.in',
               configuration: id_conf,
               ),
       output: application_id + '.desktop',
         type: 'desktop',
       po_dir: '../po',
      install: true,
  install_dir: get_option('datadir') / 'applications',
)

desktop_utils = find_program('desktop-file-validate', required: false, disabler: true)
test('validate-desktop', desktop_utils, args: [desktop_file])

metainfo_file = i18n.merge_file(
        input: configure_file(
                       input: base_id + '.metainfo.xml.in.in',
                      output: application_id + '.metainfo.xml.in',
               configuration: id_conf,
               ),
       output: application_id + '.metainfo.xml',
       po_dir: '../po',
      install: true,
  install_dir: get_option('datadir') / 'metainfo',
)

appstreamcli = find_program('appstreamcli', required: false, disabler: true)
test('validate-metainfo', appstreamcli,
     args: ['validate', '--no-net', '--explain', metainfo_file])

# Ein Schema für beide Profile wie GNOME Calendar (org.gnome.calendar).
install_data(base_id + '.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas',
)

compile_schemas = find_program('glib-compile-schemas')
test('validate-schema', compile_schemas,
     args: ['--strict', '--dry-run', meson.current_source_dir()])

# Für den Start aus dem Build-Ordner: GSETTINGS_SCHEMA_DIR=<build>/data
# (wie gnome-calendar data/meson.build:59-67, dort für die Unit-Tests).
custom_target('gschemas.compiled',
            input: base_id + '.gschema.xml',
           output: 'gschemas.compiled',
          command: [compile_schemas, '--strict', '--targetdir', meson.current_build_dir(), meson.current_source_dir()],
  build_by_default: true,
)

# Icons wie Stamp (quelltext/stamp/data/icons/meson.build); Devel-Profil mit
# eigenem Farbsymbol, Symbol-Icon für beide Profile.
install_data('icons' / 'hicolor' / 'scalable' / 'apps' / application_id + '.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)
install_data('icons' / 'hicolor' / 'symbolic' / 'apps' / base_id + '-symbolic.svg',
       rename: application_id + '-symbolic.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'symbolic' / 'apps',
)
