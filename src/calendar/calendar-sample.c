/* calendar-sample.c – statische Beispieldaten für den Prototyp (#339)
 *
 * Nur Befüllen, Muster mail/mail-sample.c. Alle Kalender, Termine, Orte
 * und Namen sind erfunden. Die Farben stehen hier, weil sie Nutzerdaten
 * sind (später e_source_selectable_get_color, Briefing 3, Punkt 2).
 * Entfällt, sobald echte Daten aus EDS kommen.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "calendar-sample.h"

/* Kalender je Konto; color als „#rrggbb“ wie ESourceSelectable:color.
 * Der Index (Position) ist die Farbklasse calendar-color-N der Termine. */
static const struct {
  const char *group;
  const char *title;
  const char *color;
  gboolean    visible;
} calendars[] = {
  { N_("Privat"), N_("Persönlich"),        "#3584e4", TRUE  },
  { N_("Privat"), N_("Geburtstage"),       "#9141ac", TRUE  },
  { N_("Privat"), N_("Feiertage"),         "#26a269", TRUE  },
  { N_("Arbeit"), N_("Arbeit"),            "#e66100", TRUE  },
  { N_("Arbeit"), N_("Projekt Nordlicht"), "#c01c28", TRUE  },
  { N_("Arbeit"), N_("Urlaubsplanung"),    "#865e3c", FALSE },
};

/* Tag 0 = Mo 28.09.2026; 21. Oktober = Tag 23. Urlaubsplanung ist
 * ausgeblendet und hat deshalb keine sichtbaren Termine. */
static const RevolutionSampleEvent events[] = {
  {  1, 1, 1110, 1200, 0, 0, N_("Chorprobe"), N_("Gemeindehaus St. Marien"), NULL },
  {  3, 1,  540,  660, 0, 3, N_("Quartalsplanung"), N_("Besprechungsraum 3"), NULL },
  {  5, 1,   -1,   -1, 0, 2, N_("Tag der Deutschen Einheit"), NULL, NULL },
  {  7, 1,  600,  660, 0, 4, N_("Jour fixe Nordlicht"), N_("Besprechungsraum 3"), NULL },
  {  8, 1, 1110, 1200, 0, 0, N_("Chorprobe"), N_("Gemeindehaus St. Marien"), NULL },
  { 10, 1,  840,  900, 0, 0, N_("Zahnarzt"), N_("Praxis Dr. Hellwig, Lindenstraße 4"), NULL },
  { 11, 1,   -1,   -1, 0, 1, N_("Geburtstag Miriam"), NULL, NULL },
  { 14, 1,  600,  660, 0, 4, N_("Jour fixe Nordlicht"), N_("Besprechungsraum 3"), NULL },
  { 15, 1, 1110, 1200, 0, 0, N_("Chorprobe"), N_("Gemeindehaus St. Marien"), NULL },
  { 16, 1,  570,  660, 0, 3, N_("Kundentermin Stadtwerke"), N_("Stadtwerke Lindenau"), NULL },
  { 18, 1, 1140, 1320, 0, 0, N_("Abendessen bei Sophie"), NULL, NULL },
  { 21, 1,  600,  660, 0, 4, N_("Jour fixe Nordlicht"), N_("Besprechungsraum 3"), NULL },
  { 22, 1,  510,  600, 0, 3, N_("Budgetrunde"), N_("Besprechungsraum 1"), NULL },
  { 22, 1, 1110, 1200, 1, 0, N_("Chorprobe"), N_("Gemeindehaus St. Marien"), NULL },
  { 23, 1,  540,  630, 0, 4, N_("Abstimmung Bauamt"), N_("Bauamt Lindenau, Raum 2.14"),
    N_("Unterlagen mitbringen: Pläne für Fassade und Außenanlagen, aktuelle Kostenschätzung.") },
  { 23, 1,  750,  810, 1, 0, N_("Mittagessen mit Tobias"), N_("Café am Rathaus"), NULL },
  { 23, 1,  960, 1050, 2, 4, N_("Review Fassadenpläne"), N_("Videokonferenz"),
    N_("Agenda: Fassade, Außenanlagen, Zeitplan bis Jahresende.") },
  { 24, 2,   -1,   -1, 0, 3, N_("Fachmesse Ausbau"), N_("Messe Leipzig"), NULL },
  { 24, 1, 1140, 1260, 1, 0, N_("Vorstandssitzung Gartenfreunde"), N_("Vereinsheim"),
    N_("Protokoll der letzten Sitzung prüfen.") },
  { 26, 1,   -1,   -1, 0, 1, N_("Geburtstag Jana"), NULL, NULL },
  { 26, 1,  600,  690, 1, 0, N_("Wochenmarkt"), N_("Marktplatz"), NULL },
  { 27, 1,  900, 1020, 0, 0, N_("Kaffee bei den Eltern"), NULL, NULL },
  { 28, 1,  600,  660, 0, 4, N_("Jour fixe Nordlicht"), N_("Besprechungsraum 3"), NULL },
  { 29, 1, 1110, 1200, 0, 0, N_("Chorprobe"), N_("Gemeindehaus St. Marien"), NULL },
  { 32, 1, 1170, 1260, 0, 0, N_("Lesung in der Bücherstube"), N_("Bücherstube am Markt"), NULL },
  { 33, 1,   -1,   -1, 0, 2, N_("Reformationstag"), NULL, NULL },
  { 34, 1,   -1,   -1, 0, 2, N_("Allerheiligen"), NULL, NULL },
};

struct _RevolutionSampleCalendar
{
  GObject     parent_instance;
  const char *title;
  const char *group;
  const char *color;
  char       *color_class;
  gboolean    visible;
};

G_DEFINE_FINAL_TYPE (RevolutionSampleCalendar, revolution_sample_calendar, G_TYPE_OBJECT)

enum { PROP_0, PROP_TITLE, PROP_GROUP, PROP_COLOR, PROP_CSS_CLASSES, PROP_VISIBLE, N_PROPS };

static void
revolution_sample_calendar_get_property (GObject    *object,
                                         guint       prop_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
  RevolutionSampleCalendar *self = REVOLUTION_SAMPLE_CALENDAR (object);

  switch (prop_id)
    {
    case PROP_TITLE:       g_value_set_static_string (value, self->title); break;
    case PROP_GROUP:       g_value_set_static_string (value, self->group); break;
    case PROP_COLOR:       g_value_set_static_string (value, self->color); break;
    case PROP_CSS_CLASSES: g_value_take_boxed (value, g_strdupv ((char *[]) { self->color_class, NULL })); break;
    case PROP_VISIBLE:     g_value_set_boolean (value, self->visible); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
revolution_sample_calendar_finalize (GObject *object)
{
  g_free (REVOLUTION_SAMPLE_CALENDAR (object)->color_class);

  G_OBJECT_CLASS (revolution_sample_calendar_parent_class)->finalize (object);
}

static void
revolution_sample_calendar_class_init (RevolutionSampleCalendarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamSpec *props[N_PROPS] = { NULL, };
  GParamFlags flags = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;

  object_class->get_property = revolution_sample_calendar_get_property;
  object_class->finalize = revolution_sample_calendar_finalize;

  props[PROP_TITLE] = g_param_spec_string ("title", NULL, NULL, NULL, flags);
  props[PROP_GROUP] = g_param_spec_string ("group", NULL, NULL, NULL, flags);
  props[PROP_COLOR] = g_param_spec_string ("color", NULL, NULL, NULL, flags);
  props[PROP_CSS_CLASSES] = g_param_spec_boxed ("css-classes", NULL, NULL, G_TYPE_STRV, flags);
  props[PROP_VISIBLE] = g_param_spec_boolean ("visible", NULL, NULL, FALSE, flags);
  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
revolution_sample_calendar_init (G_GNUC_UNUSED RevolutionSampleCalendar *self)
{
}

/* Je Konto ein GListStore; das GtkFlattenListModel macht daraus
 * Abschnitte (GtkSectionModel) für die Abschnittsköpfe der GtkListView. */
GListModel *
revolution_calendar_sample_calendars (void)
{
  GListStore *groups = g_list_store_new (G_TYPE_LIST_STORE);
  GListStore *group = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (calendars); i++)
    {
      RevolutionSampleCalendar *calendar;

      if (i == 0 || g_strcmp0 (calendars[i].group, calendars[i - 1].group) != 0)
        {
          group = g_list_store_new (REVOLUTION_TYPE_SAMPLE_CALENDAR);
          g_list_store_append (groups, group);
          g_object_unref (group);
        }

      calendar = g_object_new (REVOLUTION_TYPE_SAMPLE_CALENDAR, NULL);
      calendar->title = _(calendars[i].title);
      calendar->group = _(calendars[i].group);
      calendar->color = calendars[i].color;
      calendar->color_class = g_strdup_printf ("calendar-color-%u", i);
      calendar->visible = calendars[i].visible;
      g_list_store_append (group, calendar);
      g_object_unref (calendar);
    }

  return G_LIST_MODEL (gtk_flatten_list_model_new (G_LIST_MODEL (groups)));
}

const RevolutionSampleEvent *
revolution_calendar_sample_events (guint *n_events)
{
  *n_events = G_N_ELEMENTS (events);

  return events;
}
