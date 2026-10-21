project('revolution', 'c',
          version: '0.1.0',
          license: 'GPL-3.0-or-later',
    meson_version: '>= 1.0.0',
  default_options: [ 'warning_level=2', 'werror=false', 'c_std=gnu11' ],
)

application_id = 'org.revolution.Mail'

i18n = import('i18n')
gnome = import('gnome')

conf = configuration_data()
conf.set_quoted('APPLICATION_ID', application_id)
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('GETTEXT_PACKAGE', meson.project_name())
conf.set_quoted('LOCALEDIR', get_option('prefix') / get_option('localedir'))
configure_file(output: 'config.h', configuration: conf)

subdir('data')
subdir('src')
subdir('po')

gnome.post_install(
    gtk_update_icon_cache: true,
  update_desktop_database: true,
)
