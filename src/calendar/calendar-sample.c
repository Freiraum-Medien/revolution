/* calendar-sample.c – Listeneintrag des Prototyps (#339): Kalender
 *
 * Die Blueprint-Zeilenvorlage (calendar-view.blp) bindet an diese Eigenschaften.
 * Die statischen Beispieldaten sind entfernt (#342, Stand: Tag m2-prototyp);
 * echte Einträge folgen in M7.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "calendar-sample.h"

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

