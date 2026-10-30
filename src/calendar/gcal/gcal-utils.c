/* Übernommen aus gnome-calendar 50.0 src/utils/gcal-utils.c:1-1451 (ff4d7f7d, GPL-3.0-or-later). Revolution
 * #373: nur die benutzten Funktionen (Zeilen 1-62, 107-119, 204-249, 371-431, 706-784, 855-1017, 1309-1355),
 * erster Wochentag nur aus der Locale (ohne Portal), ohne gweather/libsoup. #375 dazu 434-523
 * (build_component_from_details, Zeitzone des Systems), 155-182 (get_circle_paintable_from_color), 1160-1233 (gcal_utils_ask_recurrence_modification_type),
 * 1294-1307 (gcal_is_valid_event_name), 1383-1410 (gcal_create_writable_calendars_model). */
/* gcal-utils.c
 *
 * Copyright (C) 2012 - Erick Pérez Castellanos
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include "config.h"

#define G_LOG_DOMAIN "Utils"

/* langinfo.h in glibc 2.27 defines ALTMON_* only if _GNU_SOURCE is defined.  */
#define _GNU_SOURCE

#include "gcal-enums.h"
#include "gcal-utils.h"
#include "gcal-event-widget.h"
#include "gcal-view.h"

#include <libecal/libecal.h>
#include <libedataserver/libedataserver.h>
#include <adwaita.h>

#include <glib/gi18n.h>

#include <langinfo.h>
#include <locale.h>

#include <string.h>
#include <math.h>
#include <stdlib.h>

/**
 * SECTION:gcal-utils
 * @short_description: Utility functions
 * @title:Utility functions
 */

static const gint
ab_day[N_WEEKDAYS] =
{
  ABDAY_1,
  ABDAY_2,
  ABDAY_3,
  ABDAY_4,
  ABDAY_5,
  ABDAY_6,
  ABDAY_7,
};

/**
 * gcal_get_weekday:
 * @i: the weekday index
 *
 * Retrieves the weekday name.
 *
 * Returns: (transfer full): the weekday name
 */
gchar*
gcal_get_weekday (gint i)
{
  return nl_langinfo (ab_day[i]);
}

/**
 * get_desc_from_component:
 * @component: an #ECalComponent
 * @joint_char: the character to use when merging event comments
 *
 * Utility method to handle the extraction of the description from an
 * #ECalComponent. This cycle through the list of #ECalComponentText
 * and concatenate each string into one.
 *
 * Returns: (nullable)(transfer full) a new allocated string with the
 * description
 **/
gchar*
get_desc_from_component (ECalComponent *component,
                         const gchar   *joint_char)
{
  GSList *text_list;
  GSList *l;

  gchar *desc = NULL;
  text_list = e_cal_component_get_descriptions (component);

  for (l = text_list; l != NULL; l = l->next)
    {
      if (l->data != NULL)
        {
          ECalComponentText *text;
          gchar *carrier;
          text = l->data;

          if (desc != NULL)
            {
              carrier = g_strconcat (desc, joint_char, e_cal_component_text_get_value (text), NULL);
              g_free (desc);
              desc = carrier;
            }
          else
            {
              desc = g_strdup (e_cal_component_text_get_value (text));
            }
        }
    }

  g_slist_free_full (text_list, e_cal_component_text_free);
  return desc != NULL ? g_strstrip (desc) : NULL;
}

/**
 * get_first_weekday:
 *
 * Copied from Clocks, which by itself is
 * copied from GtkCalendar.
 *
 * Returns: the first weekday, from 0 to 6
 */
gint
get_first_weekday (void)
{
  gint week_start;

#ifdef HAVE__NL_TIME_FIRST_WEEKDAY

  union { unsigned int word; char *string; } langinfo;
  gint week_1stday = 0;
  gint first_weekday = 1;
  guint week_origin;

  langinfo.string = nl_langinfo (_NL_TIME_FIRST_WEEKDAY);
  first_weekday = langinfo.string[0];
  langinfo.string = nl_langinfo (_NL_TIME_WEEK_1STDAY);
  week_origin = langinfo.word;
  if (week_origin == 19971130) /* Sunday */
    week_1stday = 0;
  else if (week_origin == 19971201) /* Monday */
    week_1stday = 1;
  else
    g_warning ("Unknown value of _NL_TIME_WEEK_1STDAY.\n");

  week_start = (week_1stday + first_weekday - 1) % N_WEEKDAYS;

#else

  gchar *gtk_week_start;


  /* Use a define to hide the string from xgettext */
# define GTK_WEEK_START "calendar:week_start:0"
  gtk_week_start = dgettext ("gtk40", GTK_WEEK_START);

  if (strncmp (gtk_week_start, "calendar:week_start:", 20) == 0)
    week_start = *(gtk_week_start + 20) - '0';
  else
    week_start = -1;

  if (week_start < 0 || week_start > N_WEEKDAYS - 1)
    {
      g_warning ("Whoever translated calendar:week_start:0 for GTK+ "
                 "did so wrongly.\n");
      week_start = 0;
    }

#endif

  return week_start;
}

/**
 * format_utc_offset:
 * @offset: an UTC offset
 *
 * Formats the UTC offset to a string that GTimeZone can
 * parse. E.g. "-0300" or "+0530".
 *
 * Returns: (transfer full): a string representing the
 * offset
 */
gchar*
format_utc_offset (gint64 offset)
{
  const char *sign = "+";
  gint hours, minutes, seconds;

  if (offset < 0) {
      offset = -offset;
      sign = "-";
  }

  /* offset can be seconds or microseconds */
  if (offset >= 1000000)
    offset = offset / 1000000;

  hours = offset / 3600;
  minutes = (offset % 3600) / 60;
  seconds = offset % 60;

  if (seconds > 0)
    return g_strdup_printf ("%s%02i%02i%02i", sign, hours, minutes, seconds);
  else
    return g_strdup_printf ("%s%02i%02i", sign, hours, minutes);
}

/**
 * get_alarm_trigger_minutes:
 * @event: a #GcalEvent
 * @alarm: a #ECalComponentAlarm
 *
 * Calculates the number of minutes before @event's
 * start time that the alarm should be triggered.
 *
 * Returns: the number of minutes before the event
 * start that @alarm will be triggered.
 */
gint
get_alarm_trigger_minutes (GcalEvent          *event,
                           ECalComponentAlarm *alarm)
{
  ECalComponentAlarmTrigger *trigger;
  ICalDuration *duration;
  GDateTime *alarm_dt;
  gint diff;

  trigger = e_cal_component_alarm_get_trigger (alarm);

  /*
   * We only support alarms relative to the start date, and solely
   * ignore whetever different it may be.
   */
  if (!trigger || e_cal_component_alarm_trigger_get_kind (trigger) != E_CAL_COMPONENT_ALARM_TRIGGER_RELATIVE_START)
    return -1;

  duration = e_cal_component_alarm_trigger_get_duration (trigger);
  alarm_dt = g_date_time_add_full (gcal_event_get_date_start (event),
                                   0,
                                   0,
                                   - (i_cal_duration_get_days (duration) + i_cal_duration_get_weeks (duration) * N_WEEKDAYS),
                                   - i_cal_duration_get_hours (duration),
                                   - i_cal_duration_get_minutes (duration),
                                   - i_cal_duration_get_seconds (duration));

  diff = g_date_time_difference (gcal_event_get_date_start (event), alarm_dt) / G_TIME_SPAN_MINUTE;

  g_clear_pointer (&alarm_dt, g_date_time_unref);

  return diff;
}

struct
{
  const gchar        *territory;
  GcalWeekDay         no_work_days;
} no_work_day_per_locale[] = {
  { "AE", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* United Arab Emirates */,
  { "AF", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Afghanistan */,
  { "BD", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Bangladesh */,
  { "BH", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Bahrain */,
  { "BN", GCAL_WEEK_DAY_SUNDAY   | GCAL_WEEK_DAY_FRIDAY   } /* Brunei Darussalam */,
  { "CR", GCAL_WEEK_DAY_SATURDAY                          } /* Costa Rica */,
  { "DJ", GCAL_WEEK_DAY_FRIDAY                            } /* Djibouti */,
  { "DZ", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Algeria */,
  { "EG", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Egypt */,
  { "GN", GCAL_WEEK_DAY_SATURDAY                          } /* Equatorial Guinea */,
  { "HK", GCAL_WEEK_DAY_SATURDAY                          } /* Hong Kong */,
  { "IL", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Israel */,
  { "IQ", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Iraq */,
  { "IR", GCAL_WEEK_DAY_THURSDAY | GCAL_WEEK_DAY_FRIDAY   } /* Iran */,
  { "KW", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Kuwait */,
  { "KZ", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Kazakhstan */,
  { "LY", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Libya */,
  { "MX", GCAL_WEEK_DAY_SATURDAY                          } /* Mexico */,
  { "MY", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Malaysia */,
  { "NP", GCAL_WEEK_DAY_SATURDAY                          } /* Nepal */,
  { "OM", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Oman */,
  { "QA", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Qatar */,
  { "SA", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Saudi Arabia */,
  { "SU", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Sudan */,
  { "SY", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Syria */,
  { "UG", GCAL_WEEK_DAY_SUNDAY                            } /* Uganda */,
  { "YE", GCAL_WEEK_DAY_FRIDAY   | GCAL_WEEK_DAY_SATURDAY } /* Yemen */,
};


/**
 * is_workday:
 * @day: a guint representing the day of a week (0…Sunday, 6…Saturday)
 *
 * Checks whether @day is workday or not based on the Territory part of Locale.
 *
 * Returns: %TRUE if @day is a workday, %FALSE otherwise.
 */
gboolean
is_workday (guint day)
{
  GcalWeekDay no_work_days;
  gchar *locale;
  gchar territory[3] = { 0, };
  guint i;

  if (day > N_WEEKDAYS - 1)
    return FALSE;

  no_work_days = GCAL_WEEK_DAY_SATURDAY | GCAL_WEEK_DAY_SUNDAY;

  locale = setlocale (LC_TIME, NULL);

  if (!locale || g_utf8_strlen (locale, -1) < 5)
    {
      g_warning ("Locale is unset or lacks territory code, assuming Saturday and Sunday as non workdays");
      return !(no_work_days & 1 << day);
    }

  territory[0] = locale[3];
  territory[1] = locale[4];

  for (i = 0; i < G_N_ELEMENTS (no_work_day_per_locale); i++)
    {
      if (g_strcmp0 (territory, no_work_day_per_locale[i].territory) == 0)
        {
          no_work_days = no_work_day_per_locale[i].no_work_days;
          break;
        }
    }

  return !(no_work_days & 1 << day);
}

GList*
filter_children_by_uid_and_modtype (GtkWidget             *widget,
                                    GcalRecurrenceModType  mod,
                                    const gchar           *uid)
{
  GtkWidget *child;
  GcalEvent *event;
  GList *result;

  event = NULL;
  result = NULL;

  /* First pass: find the GcalEvent */
  for (child = gtk_widget_get_first_child (widget);
       child;
       child = gtk_widget_get_next_sibling (child))
    {
      GcalEventWidget *event_widget;
      GcalEvent *ev;

      /* Safeguard against stray widgets */
      if (!GCAL_IS_EVENT_WIDGET (child))
        continue;

      event_widget = GCAL_EVENT_WIDGET (child);
      ev = gcal_event_widget_get_event (event_widget);

      if (g_str_equal (uid, gcal_event_get_uid (ev)))
        {
          result = g_list_prepend (result, event_widget);
          event = ev;
        }
    }

  /* Second pass: find the other related events */
  if (event && mod != GCAL_RECURRENCE_MOD_THIS_ONLY)
    {
      g_autofree gchar *id_prefix = NULL;
      ECalComponentId *id;
      ECalComponent *component;
      GcalCalendar *calendar;

      component = gcal_event_get_component (event);
      calendar = gcal_event_get_calendar (event);
      id = e_cal_component_get_id (component);
      id_prefix = g_strdup_printf ("%s:%s", gcal_calendar_get_id (calendar), e_cal_component_id_get_uid (id));

      for (child = gtk_widget_get_first_child (widget);
           child;
           child = gtk_widget_get_next_sibling (child))
        {
          GcalEventWidget *event_widget;
          GcalEvent *ev;

          /* Safeguard against stray widgets */
          if (!GCAL_IS_EVENT_WIDGET (child))
            continue;

          event_widget = GCAL_EVENT_WIDGET (child);
          ev = gcal_event_widget_get_event (event_widget);

          if (g_str_equal (gcal_event_get_uid (ev), uid))
            continue;

          if (!g_str_has_prefix (gcal_event_get_uid (ev), id_prefix))
            continue;

          if (mod == GCAL_RECURRENCE_MOD_ALL)
            {
              result = g_list_prepend (result, event_widget);
            }
          else if (mod == GCAL_RECURRENCE_MOD_THIS_AND_FUTURE)
            {
              if (g_date_time_compare (gcal_event_get_date_start (event), gcal_event_get_date_start (ev)) < 0)
                result = g_list_prepend (result, event_widget);
            }

        }

      e_cal_component_id_free (id);
    }

  return result;
}

/**
 * gcal_get_service_name_from_url:
 * @url: the meeting url to get service name from
 *
 * Given a meeting service URL, get the service provider name,
 * or return NULL in the case the service is not listed.
 *
 * Returns: the service name or NULL
 */
const gchar *
gcal_get_service_name_from_url (const gchar *url)
{
  struct {
    const gchar *needle;
    const gchar *service_name;
  } service_name_vtable[] = {
    // Conferencing
    { "bluejeans", N_("BlueJeans") },
    { "bigbluebutton", N_("BigBlueButton") },
    { "bbb", N_("BigBlueButton") },
    { "meet.google.com", N_("Google Meet") },
    { "meet.jit.si", N_("Jitsi") },
    { "jitsi", N_("Jitsi") },
    { "meetings.dialpad.com", N_("Uber Conference") },
    { "meet.gnome.com", N_("GNOME Meet") },
    { "teams.microsoft.com", N_("Microsoft Teams") },
    { "whereby.com", N_("Whereby") },
    { "webex", N_("Webex") },
    { "zoom.us", N_("Zoom") },

    // Map
    { "geo:", N_("View Map") },
    { "openstreetmap.org", N_("OpenStreetMap") },
    { "maps.app.goo.gl", N_("Google Maps") },
    { "google.com/maps", N_("Google Maps") },
    { "bing.com/maps", N_("Bing Maps") },
  };
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (service_name_vtable); i++)
    {
      if (strstr (url, service_name_vtable[i].needle))
        return gettext (service_name_vtable[i].service_name);
    }

  return NULL;
}

/**
 * build_component_from_details:
 * @summary:
 * @initial_date:
 * @final_date:
 *
 * Create a component with the provided details
 *
 * Returns: (transfer full): an {@link ECalComponent} object
 **/
ECalComponent*
build_component_from_details (const gchar *summary,
                              GDateTime   *initial_date,
                              GDateTime   *final_date)
{
  ECalComponent *event = NULL;
  ECalComponentDateTime *dt = NULL;
  ECalComponentText *summ = NULL;
  ICalTimezone *tz = NULL;
  ICalTime *itt = NULL;
  gboolean all_day = FALSE;

  event = e_cal_component_new ();
  e_cal_component_set_new_vtype (event, E_CAL_COMPONENT_EVENT);

  /*
   * Check if the event is all day. Notice that it can be all day even
   * without the final date.
   */
  all_day = gcal_date_time_is_date (initial_date) && (final_date ? gcal_date_time_is_date (final_date) : TRUE);

  /*
   * When the event is all day, we consider UTC timezone by default. Otherwise,
   * we always use the system timezone to create new events
   */
  if (all_day)
    {
      tz = i_cal_timezone_get_utc_timezone ();
    }
  else
    {
      g_autoptr (GTimeZone) zone = NULL;
      /* Revolution #375: Zeitzone des Systems (gcal_util_get_app_timezone_or_local braucht GcalApplication). */
      zone = g_time_zone_new_local ();
      tz = gcal_timezone_to_icaltimezone (zone);
    }

  /* Start date */
  itt = gcal_date_time_to_icaltime (initial_date);
  i_cal_time_set_timezone (itt, tz);
  i_cal_time_set_is_date (itt, all_day);
  dt = e_cal_component_datetime_new_take (itt, tz ? g_strdup (i_cal_timezone_get_tzid (tz)) : NULL);
  e_cal_component_set_dtstart (event, dt);

  e_cal_component_datetime_free (dt);

  /* End date */
  if (!final_date)
    final_date = g_date_time_add_days (initial_date, 1);

  itt = gcal_date_time_to_icaltime (final_date);
  i_cal_time_set_timezone (itt, tz);
  i_cal_time_set_is_date (itt, all_day);
  dt = e_cal_component_datetime_new_take (itt, tz ? g_strdup (i_cal_timezone_get_tzid (tz)) : NULL);
  e_cal_component_set_dtend (event, dt);

  e_cal_component_datetime_free (dt);

  /* Summary */
  summ = e_cal_component_text_new (summary, NULL);
  e_cal_component_set_summary (event, summ);
  e_cal_component_text_free (summ);

  e_cal_component_commit_sequence (event);

  return event;
}

typedef struct
{
  GcalEvent                 *event;
  GcalAskRecurrenceCallback  callback;
  gpointer                   user_data;
} AskRecurrenceData;

static void
on_message_dialog_response_cb (GObject      *source_object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  GcalRecurrenceModType mod_type;
  AskRecurrenceData *data;
  const char *response;

  response = adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source_object), result);

  if (g_strcmp0 (response, "this-only") == 0)
    mod_type = GCAL_RECURRENCE_MOD_THIS_ONLY;
  else if (g_strcmp0 (response, "subsequent-events") == 0)
    mod_type = GCAL_RECURRENCE_MOD_THIS_AND_FUTURE;
  else if (g_strcmp0 (response, "all-events") == 0)
    mod_type = GCAL_RECURRENCE_MOD_ALL;
  else
    mod_type = GCAL_RECURRENCE_MOD_NONE;

  data = (AskRecurrenceData *) user_data;
  data->callback (data->event, mod_type, data->user_data);
  g_clear_object (&data->event);
  g_clear_pointer (&data, g_free);
}

void
gcal_utils_ask_recurrence_modification_type (GtkWidget                 *parent,
                                             GcalEvent                 *event,
                                             gboolean                   show_mod_all,
                                             GcalAskRecurrenceCallback  callback,
                                             gpointer                   user_data)
{
  AskRecurrenceData *data;
  ECalClient *client;
  AdwDialog *dialog;

  data = g_new0 (AskRecurrenceData, 1);
  data->event = g_object_ref (event);
  data->callback = callback;
  data->user_data = user_data;

  dialog = adw_alert_dialog_new (_("Modify Multiple Events?"),
                                 _("The event you are trying to modify is recurring. The changes you have selected should be applied to:"));

  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                  "close", _("_Cancel"),
                                  "this-only", _("_Only This Event"),
                                  NULL);

  client = gcal_calendar_get_client (gcal_event_get_calendar (event));

  if (!e_client_check_capability (E_CLIENT (client), E_CAL_STATIC_CAPABILITY_NO_THISANDFUTURE))
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "subsequent-events", _("_Subsequent Events"));

  if (show_mod_all)
    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "all-events",  _("_All Events"));

  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");

  adw_alert_dialog_choose (ADW_ALERT_DIALOG (dialog),
                           parent,
                           NULL,
                           on_message_dialog_response_cb,
                           data);
}

/**
 * gcal_is_valid_event_name:
 * @event_name: the name of the event to check
 *
 * Check if the event name is valid
 *
 * Returns: %TRUE if the name is valid, %FALSE otherwise.
 */
gboolean
gcal_is_valid_event_name (const gchar *event_name)
{
  g_autofree gchar *aux = g_strstrip (g_strdup (event_name));
  return g_utf8_strlen (aux, -1) > 0;
}

/**
 * gcal_create_writable_calendars_model:
 * @manager: a #GcalManager
 *
 * Retrieves a model with all available read-write #GcalCalendar.
 * This is useful for binding to combo rows.
 *
 * Returns: (transfer full): a #GListModel with all available read-write #GcalCalendar
 */
GListModel*
gcal_create_writable_calendars_model (GcalManager *manager)
{
  g_autoptr (GtkFilterListModel) filter_model = NULL;
  GtkBoolFilter *bool_filter;
  GListModel *calendars;

  g_return_val_if_fail (GCAL_IS_MANAGER (manager), NULL);

  calendars = gcal_manager_get_calendars_model (manager);

  bool_filter = gtk_bool_filter_new (gtk_property_expression_new (GCAL_TYPE_CALENDAR, NULL, "read-only"));
  gtk_bool_filter_set_invert (bool_filter, TRUE);

  filter_model = gtk_filter_list_model_new (g_object_ref (calendars), GTK_FILTER (bool_filter));
  gtk_filter_list_model_set_watch_items (filter_model, TRUE);

  return G_LIST_MODEL (g_steal_pointer (&filter_model));
}

/**
 * get_circle_paintable_from_color:
 * @color: a #GdkRGBA
 * @size: the size of the surface
 *
 * Creates a circular surface filled with @color. The
 * surface is always @size x @size.
 *
 * Returns: (transfer full): a #cairo_surface_t
 */
GdkPaintable*
get_circle_paintable_from_color (const GdkRGBA *color,
                                 gint           size)
{
  g_autoptr (GtkSnapshot) snapshot = NULL;
  GskRoundedRect rect;

  snapshot = gtk_snapshot_new ();

  gtk_snapshot_push_rounded_clip (snapshot,
                                  gsk_rounded_rect_init_from_rect (&rect,
                                                                   &GRAPHENE_RECT_INIT (0, 0, size, size),
                                                                   size / 2.0));

  gtk_snapshot_append_color (snapshot, color, &GRAPHENE_RECT_INIT (0, 0, size, size));

  gtk_snapshot_pop (snapshot);

  return gtk_snapshot_to_paintable (snapshot, &GRAPHENE_SIZE_INIT (size, size));
}
