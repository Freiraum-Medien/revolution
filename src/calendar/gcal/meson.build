# #373: Übernommen aus gnome-calendar 50.0 (ff4d7f7d, GPL-3.0-or-later), gekürzt; Quelle je Datei
# im Kopf. Eigene statische Bibliothek wie in der Vorlage (src/meson.build:69-75 „gcal-private“),
# damit die Dateien mit dem Warnniveau der Vorlage bauen (gnome-calendar meson.build: Standard
# warning_level 1); -Dwerror=true gilt weiter.

# gcal-enum-types.{c,h} wie src/meson.build:20 und data/meson.build:3.
gcal_enum_types = gnome.mkenums_simple('gcal-enum-types', sources: files('gcal-enums.h'))

# gcal-debug.h wie src/meson.build:28-45, Tracing aus.
gcal_debug_conf = configuration_data()
gcal_debug_conf.set('BUGREPORT_URL', 'https://gitlab.gnome.org/GNOME/gnome-calendar/issues')
gcal_debug_conf.set10('ENABLE_TRACING', false)
gcal_debug_h = configure_file(input: 'gcal-debug.h.in', output: 'gcal-debug.h', configuration: gcal_debug_conf)

# Erster Wochentag aus der Locale wie gnome-calendar meson.build:94-102.
gcal_c_args = ['-DEDS_DISABLE_DEPRECATED']
nl_time_first_weekday_src = '''
  #define _GNU_SOURCE
  #include <langinfo.h>
  int main() { nl_langinfo(_NL_TIME_FIRST_WEEKDAY); };
'''
if meson.get_compiler('c').compiles(nl_time_first_weekday_src, name: '_NL_TIME_FIRST_WEEKDAY')
  gcal_c_args += '-DHAVE__NL_TIME_FIRST_WEEKDAY=1'
endif

gcal_deps = [
  dependency('gtk4', version: '>= 4.22'),
  dependency('libadwaita-1', version: '>= 1.9'),
  dependency('libedataserver-1.2', version: '>= 3.56'),
  dependency('libecal-2.0', version: '>= 3.56'),
  meson.get_compiler('c').find_library('m', required: false),
]

gcal_lib = static_library('gcal',
  'gcal-calendar.c',
  'gcal-calendar-monitor.c',
  'gcal-clock.c',
  'gcal-context.c',
  'gcal-date-time-utils.c',
  'gcal-event.c',
  'gcal-event-attendee.c',
  'gcal-event-organizer.c',
  'gcal-event-widget.c',
  'gcal-global.c',
  'gcal-manager.c',
  'gcal-month-cell.c',
  'gcal-month-popover.c',
  'gcal-month-view.c',
  'gcal-month-view-row.c',
  'gcal-overflow-bin.c',
  'gcal-overflow-bin-layout.c',
  'gcal-range.c',
  'gcal-range-tree.c',
  'gcal-recurrence.c',
  'gcal-time-zone-monitor.c',
  'gcal-timeline.c',
  'gcal-timeline-subscriber.c',
  'gcal-utils.c',
  'gcal-view.c',
  # #374: Woche und Tag
  'gcal-gui-utils.c',
  'gcal-week-grid.c',
  'gcal-week-header.c',
  'gcal-week-hour-bar.c',
  'gcal-week-view.c',
  'gcal-week-view-common.c',
  gcal_enum_types,
  gcal_debug_h,
  dependencies: gcal_deps,
  include_directories: include_directories('.', '../../..'),
  # Wie gnome-calendar meson.build:128-136 (EDS_DISABLE_DEPRECATED).
  c_args: gcal_c_args,
  override_options: ['warning_level=1'],
)

gcal_dep = declare_dependency(
  link_with: gcal_lib,
  sources: [gcal_enum_types[1], gcal_debug_h],
  dependencies: gcal_deps,
  include_directories: include_directories('.'),
)
