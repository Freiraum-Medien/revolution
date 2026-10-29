/* Übernommen aus gnome-calendar 50.0 src/utils/gcal-utils.h:1-141 (ff4d7f7d, GPL-3.0-or-later). Revolution
 * #373: nur die benutzten Deklarationen und Makros, ohne gweather. */
/*
 * gcal-window.h
 * Copyright (C) 2015 Erick Pérez Castellanos <erickpc@gnome.org>
 *
 * gnome-calendar is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * gnome-calendar is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __GCAL_UTILS_H__
#define __GCAL_UTILS_H__

#include "gcal-date-time-utils.h"
#include "gcal-manager.h"

#include <gtk/gtk.h>
#include <libecal/libecal.h>

#define N_WEEKDAYS 7
#define ALIGNED(x)      (round (x) + 0.5)
#define MINUTES_PER_DAY 1440
#define MAX_MINUTES     (N_WEEKDAYS * MINUTES_PER_DAY)

#define gcal_clear_timeout(pp) { if (pp && *pp) { g_source_remove (*pp); *pp = 0; } }
#define gcal_clear_signal_handler(pp,instance) { if (pp && *pp > 0) { g_signal_handler_disconnect (instance, *pp); *pp = 0; } }

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ECalComponent, g_object_unref)

G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalTime, g_object_unref)

gchar*               gcal_get_weekday                            (gint                i);

gchar*               get_desc_from_component                     (ECalComponent      *component,
                                                                  const gchar        *joint_char);

gint                 get_first_weekday                           (void);

gchar*               format_utc_offset                           (gint64              offset);

gint                 get_alarm_trigger_minutes                   (GcalEvent          *event,
                                                                  ECalComponentAlarm *alarm);

gboolean             is_workday                                  (guint	              day);

GList*               filter_children_by_uid_and_modtype          (GtkWidget             *widget,
                                                                  GcalRecurrenceModType  mod,
                                                                  const gchar           *uid);

const gchar*         gcal_get_service_name_from_url              (const gchar        *url);

#endif /* __GCAL_UTILS_H__ */
