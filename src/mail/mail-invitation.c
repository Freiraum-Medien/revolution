/* mail-invitation.c – Einladungskarte im Lesebereich: anzeigen, zusagen, vorläufig, absagen (#376)
 *
 * mail-reader.c findet beim Zerlegen den ersten Kalenderteil (text/calendar, application/ics) und übergibt ihn
 * hierher. Ein Worker-GTask (Architektur 1) liest ihn mit libical, verbindet die eingeschalteten Kalender aus der
 * ESourceRegistry, sucht den Termin über seine UID und Überschneidungen im Zeitraum; im UI-Thread füllt das
 * Ergebnis die Karte (mail-invitation.blp). Eine Antwort läuft wieder im Worker.
 *
 * Verhalten wie Evolution 3.56.2 (Vorlage, nicht übernommen): src/modules/itip-formatter/itip-view.c
 *   Suche in allen eingeschalteten Kalendern, Konflikte    find_server 5081-5227, get_object_list_ready_cb 4928-4981
 *   ohne Treffer der Standardkalender                      decrease_find_data 4716-4807
 *   Zusagen/Vorläufig/Absagen: PARTSTAT setzen, Termin     view_response_cb 6888-6986, update_item 5778-5881
 *     über e_cal_client_receive_objects (METHOD REQUEST)
 *   REPLY nur mit dem eigenen Teilnehmer an den Organisator finish_message_delete_with_rsvp 5429-5501
 *   CANCEL: receive_objects mit METHOD CANCEL               update_item (Antwort ITIP_VIEW_RESPONSE_CANCEL)
 *   REPLY beim Organisator: Status des Teilnehmers         update_attendee_status_icomp 6109-6260
 *     übernehmen, e_cal_client_modify_object
 * und src/calendar/gui/itip-utils.c (Versand der Antwort itip_send_component_begin 2130-2295, Betreff comp_subject
 * 1167-1300). Ob Revolution die Antwort selbst per Mail schickt: wie Evolution nicht, wenn der Server die Planung
 * übernimmt (e_cal_client_check_save_schedules, itip-utils.c:2219-2224; microsoft365 antwortet über die
 * Graph-Antwort des Termins mit Mail an den Organisator, evolution-ews e-cal-backend-m365.c:1591-1610). Abweichung:
 * Lag der Termin vor der Antwort nicht im gewählten Kalender, schickt Revolution sie trotzdem über den Postausgang
 * (#362, revolution_mail_send_message): Google-CalDAV meldet calendar-auto-schedule, verschickt für einen so neu
 * angelegten Termin eines fremden Organisators aber keine Antwort (gemessen 30.10.2026).
 *
 * Ohne Aufgaben und Notizen, ohne PUBLISH/ADD/REFRESH/COUNTER, ohne Wiederholungsausnahmen „alle Termine“,
 * ohne Kommentar zur Antwort, ohne Delegation, ohne Erinnerungen übernehmen/behalten, ohne Löschen der Mail.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>
#include <camel/camel.h>
#include <libecal/libecal.h>

#include "mail-invitation.h"
#include "mail-outbox.h"
#include "mail-view.h"

/* libical-glib 3.0 bringt keine g_autoptr-Aufräumfunktionen mit (gnome-calendar ebenso, gcal-utils.h:44). */
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalComponent, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalParameter, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalProperty, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalTime, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (ICalTimezone, g_object_unref)

typedef enum
{
  RESPOND_ACCEPTED,
  RESPOND_TENTATIVE,
  RESPOND_DECLINED,
  RESPOND_REMOVE,
  RESPOND_UPDATE,
} Respond;

/* Ergebnis des Lesens (Worker), danach unverändert; Antworten halten eine Referenz (g_rc_box). */
typedef struct
{
  ESourceRegistry    *registry;
  char               *account_uid;
  char               *ical;
  ICalComponent      *top;
  ICalComponent      *comp;
  ICalPropertyMethod  method;
  char               *identity_uid;
  char               *identity_address;
  char               *address;
  char               *intro;
  char               *summary;
  char               *when;
  char               *location;
  char               *organizer;
  char               *conflicts;
  ICalParameterPartstat partstat;
  GPtrArray          *calendars;
  guint               selected;
  ESource            *existing;
} Invitation;

static void
invitation_clear (Invitation *inv)
{
  g_clear_object (&inv->registry);
  g_free (inv->account_uid);
  g_free (inv->ical);
  g_clear_object (&inv->comp);
  g_clear_object (&inv->top);
  g_free (inv->identity_uid);
  g_free (inv->identity_address);
  g_free (inv->address);
  g_free (inv->intro);
  g_free (inv->summary);
  g_free (inv->when);
  g_free (inv->location);
  g_free (inv->organizer);
  g_free (inv->conflicts);
  g_clear_pointer (&inv->calendars, g_ptr_array_unref);
  g_clear_object (&inv->existing);
}

static void
invitation_unref (Invitation *inv)
{
  g_rc_box_release_full (inv, (GDestroyNotify) invitation_clear);
}

struct _RevolutionMailInvitation
{
  AdwBin              parent_instance;
  GtkLabel           *intro_label;
  GtkLabel           *title_label;
  GtkLabel           *when_label;
  GtkLabel           *where_label;
  GtkLabel           *organizer_label;
  GtkLabel           *conflict_label;
  GtkLabel           *status_label;
  GtkWidget          *calendar_box;
  GtkDropDown        *calendar_dropdown;
  GtkWidget          *accept_button;
  GtkWidget          *tentative_button;
  GtkWidget          *decline_button;
  GtkWidget          *remove_button;
  GtkWidget          *update_button;
  AdwSpinner         *spinner;
  GSimpleActionGroup *actions;
  GCancellable       *cancellable;
  guint               generation;
  Invitation         *inv;
};

G_DEFINE_FINAL_TYPE (RevolutionMailInvitation, revolution_mail_invitation, ADW_TYPE_BIN)

/* ------------------------------------------------------------------ libical (Worker) */

/* Übernommen aus Evolution 3.56.2 src/calendar/gui/itip-utils.c:2938-2970 (itip_utils_find_attendee_property),
 * Lizenz LGPL-2.1-or-later/LGPL-3.0 (Evolution COPYING:1-4), hier GPL-3.0-or-later. */
static ICalProperty *
find_attendee (ICalComponent *icomp,
               const char    *address)
{
  ICalProperty *prop;

  if (address == NULL || *address == '\0')
    return NULL;

  for (prop = i_cal_component_get_first_property (icomp, I_CAL_ATTENDEE_PROPERTY); prop != NULL;
       g_object_unref (prop), prop = i_cal_component_get_next_property (icomp, I_CAL_ATTENDEE_PROPERTY))
    {
      const char *email = e_cal_util_get_property_email (prop);

      if (email != NULL && e_cal_util_email_addresses_equal (address, email))
        break;
    }

  return prop;
}

/* Übernommen aus Evolution 3.56.2 src/calendar/gui/itip-utils.c:2972-3036 (itip_utils_prepare_attendee_response),
 * ohne den Zweig ohne eigene Adresse (Standardname aus der Registry). */
static void
set_partstat (ICalComponent        *icomp,
              const char           *address,
              ICalParameterPartstat partstat)
{
  ICalProperty *prop = find_attendee (icomp, address);

  if (prop == NULL)
    {
      g_autofree char *mailto = g_strconcat ("mailto:", address, NULL);

      prop = i_cal_property_new_attendee (mailto);
      i_cal_property_take_parameter (prop, i_cal_parameter_new_role (I_CAL_ROLE_OPTPARTICIPANT));
      i_cal_component_add_property (icomp, prop);
    }
  i_cal_property_remove_parameter_by_kind (prop, I_CAL_PARTSTAT_PARAMETER);
  i_cal_property_take_parameter (prop, i_cal_parameter_new_partstat (partstat));
  g_object_unref (prop);
}

/* Übernommen aus Evolution 3.56.2 src/calendar/gui/itip-utils.c:2910-2935 (itip_utils_update_cdo_replytime):
 * Outlook wertet bei REPLY nicht SEQUENCE, sondern X-MICROSOFT-CDO-REPLYTIME aus. */
static void
update_cdo_replytime (ICalComponent *icomp)
{
  g_autoptr (ICalTime) stamp = i_cal_time_new_current_with_zone (i_cal_timezone_get_utc_timezone ());
  g_autofree char *text = i_cal_time_as_ical_string (stamp);

  while (e_cal_util_component_remove_x_property (icomp, "X-MICROSOFT-CDO-REPLYTIME"))
    ;
  e_cal_util_component_set_x_property (icomp, "X-MICROSOFT-CDO-REPLYTIME", text);
}

/* Übernommen aus Evolution 3.56.2 src/calendar/gui/itip-utils.c:3039-3071 (itip_utils_remove_all_but_attendee). */
static void
remove_all_but_attendee (ICalComponent *icomp,
                         const char    *address)
{
  GSList *remove = NULL;
  gboolean found = FALSE;

  for (ICalProperty *prop = i_cal_component_get_first_property (icomp, I_CAL_ATTENDEE_PROPERTY); prop != NULL;
       prop = i_cal_component_get_next_property (icomp, I_CAL_ATTENDEE_PROPERTY))
    {
      if (found || !e_cal_util_email_addresses_equal (e_cal_util_get_property_email (prop), address))
        remove = g_slist_prepend (remove, prop);
      else
        {
          found = TRUE;
          g_object_unref (prop);
        }
    }

  for (GSList *link = remove; link != NULL; link = link->next)
    i_cal_component_remove_property (icomp, link->data);
  g_slist_free_full (remove, g_object_unref);
}

/* VCALENDAR mit METHOD und den VTIMEZONEs der Einladung (Evolution: top_level mit Zeitzonen, itip-view.c:5789-5792). */
static ICalComponent *
new_toplevel (Invitation         *inv,
              ICalPropertyMethod  method,
              ICalComponent      *comp)
{
  ICalComponent *top = i_cal_component_new_vcalendar ();

  i_cal_component_take_property (top, i_cal_property_new_version ("2.0"));
  i_cal_component_take_property (top, i_cal_property_new_prodid ("-//Revolution//" PACKAGE_VERSION "//EN"));
  i_cal_component_set_method (top, method);
  for (ICalComponent *tz = i_cal_component_get_first_component (inv->top, I_CAL_VTIMEZONE_COMPONENT); tz != NULL;
       g_object_unref (tz), tz = i_cal_component_get_next_component (inv->top, I_CAL_VTIMEZONE_COMPONENT))
    i_cal_component_take_component (top, i_cal_component_clone (tz));
  i_cal_component_take_component (top, comp);

  return top;
}

/* Beginn oder Ende als time_t; TZID aus den VTIMEZONEs der Einladung, sonst eingebaute Zone, sonst Ortszeit. */
static gint64
comp_time (Invitation       *inv,
           ICalPropertyKind  kind,
           gboolean         *is_date)
{
  g_autoptr (ICalProperty) prop = i_cal_component_get_first_property (inv->comp, kind);
  g_autoptr (ICalParameter) param = NULL;
  g_autoptr (ICalTime) time = NULL;
  g_autoptr (ICalTimezone) own_zone = NULL;
  ICalTimezone *zone = NULL;

  if (prop == NULL)
    return 0;
  time = kind == I_CAL_DTSTART_PROPERTY ? i_cal_property_get_dtstart (prop) : i_cal_property_get_dtend (prop);
  *is_date = i_cal_time_is_date (time);
  param = i_cal_property_get_first_parameter (prop, I_CAL_TZID_PARAMETER);
  if (i_cal_time_is_utc (time))
    zone = i_cal_timezone_get_utc_timezone ();
  else if (param != NULL)
    {
      zone = own_zone = i_cal_component_get_timezone (inv->top, i_cal_parameter_get_tzid (param));
      if (zone == NULL)
        zone = i_cal_timezone_get_builtin_timezone (i_cal_parameter_get_tzid (param));
    }
  if (*is_date || zone == NULL)
    {
      g_autoptr (GDateTime) local = g_date_time_new_local (i_cal_time_get_year (time), i_cal_time_get_month (time),
                                                           i_cal_time_get_day (time), i_cal_time_get_hour (time),
                                                           i_cal_time_get_minute (time), i_cal_time_get_second (time));
      return local != NULL ? g_date_time_to_unix (local) : 0;
    }

  return i_cal_time_as_timet_with_zone (time, zone);
}

static char *
format_when (gint64   start,
             gint64   end,
             gboolean all_day)
{
  g_autoptr (GDateTime) from = g_date_time_new_from_unix_local (start);
  g_autoptr (GDateTime) to = g_date_time_new_from_unix_local (all_day && end > start ? end - 86400 : MAX (end, start));
  /* Translators: date of a meeting in the invitation card (g_date_time_format). */
  const char *day = _("%A, %B %-d, %Y");
  g_autofree char *from_day = g_date_time_format (from, day);
  g_autofree char *to_day = g_date_time_format (to, day);

  if (all_day)
    return g_strcmp0 (from_day, to_day) == 0 ? g_steal_pointer (&from_day)
                                              : g_strdup_printf ("%s – %s", from_day, to_day);
  if (g_strcmp0 (from_day, to_day) == 0)
    {
      g_autofree char *a = g_date_time_format (from, "%H:%M");
      g_autofree char *b = g_date_time_format (to, "%H:%M");

      /* Translators: time of a meeting, day, start and end, e.g. “Tuesday, November 10, 2026, 11:00 – 11:30”. */
      return g_strdup_printf (_("%s, %s – %s"), from_day, a, b);
    }
  else
    {
      g_autofree char *a = g_date_time_format (from, "%H:%M");
      g_autofree char *b = g_date_time_format (to, "%H:%M");

      return g_strdup_printf ("%s, %s – %s, %s", from_day, a, to_day, b);
    }
}

static char *
property_name (ICalProperty *prop)
{
  g_autoptr (ICalParameter) cn = prop != NULL ? i_cal_property_get_first_parameter (prop, I_CAL_CN_PARAMETER) : NULL;
  const char *email = prop != NULL ? e_cal_util_get_property_email (prop) : NULL;

  if (cn != NULL && i_cal_parameter_get_cn (cn) != NULL && *i_cal_parameter_get_cn (cn) != '\0' &&
      g_strcmp0 (i_cal_parameter_get_cn (cn), email) != 0)
    return g_strdup_printf ("%s <%s>", i_cal_parameter_get_cn (cn), email != NULL ? email : "");

  return g_strdup (email != NULL ? email : "");
}

static ICalParameterPartstat
attendee_partstat (ICalProperty *prop)
{
  g_autoptr (ICalParameter) param = prop != NULL ? i_cal_property_get_first_parameter (prop, I_CAL_PARTSTAT_PARAMETER)
                                                 : NULL;

  return param != NULL ? i_cal_parameter_get_partstat (param) : I_CAL_PARTSTAT_NONE;
}

/* Eigene Adresse unter den Teilnehmern: die Identität des Kontos der Mail, wie Evolution find_to_address
 * (itip-view.c:3738-3956, zuletzt die Identität zum Ort der Mail 3936-3953). Anders als Evolution ohne Ersatz: Steht diese
 * Adresse nicht unter den Teilnehmern, bleibt inv->address leer, die Karte sagt das und antwortet nicht (PM #376 R2). */
static void
find_own_address (Invitation *inv)
{
  g_autoptr (ESource) account = e_source_registry_ref_source (inv->registry, inv->account_uid);
  g_autoptr (ESource) identity = NULL;
  g_autoptr (ICalProperty) prop = NULL;

  if (account != NULL && e_source_has_extension (account, E_SOURCE_EXTENSION_MAIL_ACCOUNT))
    inv->identity_uid = e_source_mail_account_dup_identity_uid (
      e_source_get_extension (account, E_SOURCE_EXTENSION_MAIL_ACCOUNT));
  identity = inv->identity_uid != NULL ? e_source_registry_ref_source (inv->registry, inv->identity_uid) : NULL;
  if (identity == NULL || !e_source_has_extension (identity, E_SOURCE_EXTENSION_MAIL_IDENTITY))
    return;

  inv->identity_address = e_source_mail_identity_dup_address (
    e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_IDENTITY));
  prop = find_attendee (inv->comp, inv->identity_address);
  if (prop != NULL)
    inv->address = g_strdup (e_cal_util_get_property_email (prop));
}

static char *
iso_utc (gint64 time)
{
  g_autoptr (GDateTime) utc = g_date_time_new_from_unix_utc (time);

  return g_date_time_format (utc, "%Y%m%dT%H%M%SZ");
}

static void
analyze_thread (GTask        *task,
                gpointer      source_object G_GNUC_UNUSED,
                gpointer      task_data,
                GCancellable *cancellable)
{
  Invitation *inv = task_data;
  g_autolist (ESource) sources = NULL;
  g_autoptr (ESource) fallback = NULL;
  g_autoptr (ESource) account = NULL;
  const char *account_parent;
  int existing_score = -1;
  g_autoptr (ICalProperty) organizer = NULL;
  g_autoptr (GString) conflicts = g_string_new (NULL);
  g_autofree char *sexp = NULL;
  const char *uid;
  gint64 start, end;
  gboolean start_date = FALSE, end_date = FALSE;

  inv->top = i_cal_component_new_from_string (inv->ical);
  if (inv->top != NULL && i_cal_component_isa (inv->top) == I_CAL_VEVENT_COMPONENT)
    {
      ICalComponent *event = inv->top;

      inv->top = i_cal_component_new_vcalendar ();
      i_cal_component_take_component (inv->top, event);
    }
  inv->comp = inv->top != NULL ? i_cal_component_get_first_component (inv->top, I_CAL_VEVENT_COMPONENT) : NULL;
  inv->method = inv->comp != NULL && e_cal_util_component_has_property (inv->top, I_CAL_METHOD_PROPERTY)
                  ? i_cal_component_get_method (inv->top) : I_CAL_METHOD_NONE;
  if (inv->comp == NULL || i_cal_component_get_uid (inv->comp) == NULL ||
      (inv->method != I_CAL_METHOD_REQUEST && inv->method != I_CAL_METHOD_CANCEL && inv->method != I_CAL_METHOD_REPLY))
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Kein VEVENT mit REQUEST/CANCEL/REPLY");
      return;
    }

  uid = i_cal_component_get_uid (inv->comp);
  inv->summary = g_strdup (i_cal_component_get_summary (inv->comp));
  inv->location = g_strdup (i_cal_component_get_location (inv->comp));
  organizer = i_cal_component_get_first_property (inv->comp, I_CAL_ORGANIZER_PROPERTY);
  inv->organizer = property_name (organizer);
  start = comp_time (inv, I_CAL_DTSTART_PROPERTY, &start_date);
  end = comp_time (inv, I_CAL_DTEND_PROPERTY, &end_date);
  if (end <= start)
    end = start_date ? start + 86400 : start;
  inv->when = format_when (start, end, start_date);
  find_own_address (inv);

  if (inv->method == I_CAL_METHOD_REPLY)
    {
      g_autoptr (ICalProperty) replier = i_cal_component_get_first_property (inv->comp, I_CAL_ATTENDEE_PROPERTY);
      g_autofree char *name = property_name (replier);

      inv->partstat = attendee_partstat (replier);
      /* Übernommen aus Evolution 3.56.2 itip-view.c:525 (set_calendar_sender_text). */
      inv->intro = g_strdup_printf (_("%s has sent back the following meeting response:"), name);
    }
  else if (inv->method == I_CAL_METHOD_CANCEL)
    /* Übernommen aus Evolution 3.56.2 itip-view.c:531. */
    inv->intro = g_strdup_printf (_("%s has cancelled the following meeting:"), inv->organizer);
  else
    /* Übernommen aus Evolution 3.56.2 itip-view.c:505. */
    inv->intro = g_strdup_printf (_("%s requests your presence at the following meeting:"), inv->organizer);

  if (start > 0)
    {
      g_autofree char *from = iso_utc (start), *to = iso_utc (end);

      GString *encoded = g_string_new (NULL);

      e_sexp_encode_string (encoded, uid);
      sexp = g_strdup_printf ("(and (occur-in-time-range? (make-time \"%s\") (make-time \"%s\")) (not (uid? %s)))",
                              from, to, encoded->str);
      g_string_free (encoded, TRUE);
    }

  account = e_source_registry_ref_source (inv->registry, inv->account_uid);
  account_parent = account != NULL ? e_source_get_parent (account) : NULL;
  inv->calendars = g_ptr_array_new_with_free_func (g_object_unref);
  sources = e_source_registry_list_enabled (inv->registry, E_SOURCE_EXTENSION_CALENDAR);
  for (GList *link = sources; link != NULL; link = link->next)
    {
      ESource *source = link->data;
      g_autoptr (EClient) client = NULL;
      g_autoptr (ICalComponent) stored = NULL;
      g_autoptr (GError) error = NULL;
      GSList *objects = NULL;
      gboolean found;

      client = e_cal_client_connect_sync (source, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, 30, cancellable, &error);
      if (client == NULL)
        {
          g_debug ("Einladung: Kalender %s nicht verbunden: %s", e_source_get_uid (source), error->message);
          continue;
        }
      if (!e_client_is_readonly (client))
        g_ptr_array_add (inv->calendars, g_object_ref (source));

      found = e_cal_client_get_object_sync (E_CAL_CLIENT (client), uid, NULL, &stored, cancellable, NULL) &&
              stored != NULL;
      /* Liegt der Termin in mehreren Kalendern (Organisator und Teilnehmer sind beides Konten von Arnd; Google übernimmt
       * Einladungen selbst), zählt der Kalender, in dem Arnd schon geantwortet hat (REQUEST, CANCEL) bzw. Organisator ist
       * (REPLY), danach der im Konto der Mail, sonst der erste (Evolution find_server). Gemessen Läufe 0219/0234: REPLY
       * fand zuerst die Kopie des Teilnehmers, CANCEL Googles eigene Kopie statt des angenommenen Termins. */
      if (found)
        {
          g_autoptr (ICalProperty) prop = inv->method == I_CAL_METHOD_REPLY ? NULL : find_attendee (stored, inv->address);
          g_autoptr (ICalProperty) org = i_cal_component_get_first_property (stored, I_CAL_ORGANIZER_PROPERTY);
          ICalParameterPartstat partstat = attendee_partstat (prop);
          int score = 0;

          if (inv->method == I_CAL_METHOD_REPLY)
            score += org != NULL && inv->identity_address != NULL &&
                     e_cal_util_email_addresses_equal (e_cal_util_get_property_email (org), inv->identity_address) ? 2 : 0;
          else
            score += partstat == I_CAL_PARTSTAT_ACCEPTED || partstat == I_CAL_PARTSTAT_TENTATIVE ||
                     partstat == I_CAL_PARTSTAT_DECLINED ? 2 : 0;
          score += account_parent != NULL && g_strcmp0 (e_source_get_parent (source), account_parent) == 0 ? 1 : 0;
          if (inv->existing == NULL || score > existing_score)
            {
              g_set_object (&inv->existing, source);
              existing_score = score;
              if (inv->method == I_CAL_METHOD_REQUEST)
                inv->partstat = partstat;
            }
        }

      if (inv->method == I_CAL_METHOD_REQUEST && sexp != NULL &&
          e_cal_client_get_object_list_sync (E_CAL_CLIENT (client), sexp, &objects, cancellable, NULL))
        {
          for (GSList *o = objects; o != NULL; o = o->next)
            {
              g_autoptr (ICalProperty) transp = i_cal_component_get_first_property (o->data, I_CAL_TRANSP_PROPERTY);

              /* Der Termin selbst zählt nicht: microsoft365 speichert ihn unter der Graph-ID als UID, die
               * iCalendar-UID der Einladung findet ihn nur e_cal_client_get_object (gemessen, Probe 30.10.). */
              if (found && g_strcmp0 (i_cal_component_get_uid (o->data), i_cal_component_get_uid (stored)) == 0)
                continue;
              /* Wie Evolution nur undurchsichtige Termine (itip-view.c:4955-4964). */
              if (transp != NULL && i_cal_property_get_transp (transp) != I_CAL_TRANSP_OPAQUE &&
                  i_cal_property_get_transp (transp) != I_CAL_TRANSP_NONE)
                continue;
              if (conflicts->len > 0)
                g_string_append_c (conflicts, '\n');
              /* Übernommen aus Evolution 3.56.2 itip-view.c:4516 (find_cal_update_ui). */
              g_string_append_printf (conflicts, _("An appointment “%s” in the calendar “%s” conflicts with this meeting"),
                                      i_cal_component_get_summary (o->data) != NULL ? i_cal_component_get_summary (o->data) : "",
                                      e_source_get_display_name (source));
            }
          e_util_free_nullable_object_slist (objects);
        }
    }

  if (conflicts->len > 0)
    inv->conflicts = g_string_free (g_steal_pointer (&conflicts), FALSE);

  /* Auswahl: Evolution nimmt den Kalender mit dem Termin, sonst den Standardkalender (decrease_find_data). Revolution
   * bevorzugt das Konto der Mail: Hat Arnd Organisator und Teilnehmer als Konten, läge der Termin sonst schon im
   * Kalender des Organisators und der wäre vorgewählt (gemessen, Lauf 0150 Fall A). Reihenfolge: Termin im Konto der
   * Mail, einziger schreibbarer Kalender des Kontos, Termin, Standardkalender. */
  {
    const char *parent = account_parent;
    int own = -1, own_count = 0, existing = -1, fallback_index = -1;
    gboolean existing_own = FALSE;

    fallback = e_source_registry_ref_default_calendar (inv->registry);
    for (guint i = 0; i < inv->calendars->len; i++)
      {
        ESource *source = g_ptr_array_index (inv->calendars, i);
        gboolean same = parent != NULL && g_strcmp0 (e_source_get_parent (source), parent) == 0;

        if (same && own_count++ == 0)
          own = i;
        if (inv->existing != NULL && e_source_equal (source, inv->existing))
          {
            existing = i;
            existing_own = same;
          }
        if (fallback != NULL && e_source_equal (source, fallback))
          fallback_index = i;
      }
    if (existing >= 0 && existing_own)
      inv->selected = existing;
    else if (own_count == 1)
      inv->selected = own;
    else if (existing >= 0)
      inv->selected = existing;
    else if (fallback_index >= 0)
      inv->selected = fallback_index;
  }

  g_debug ("Einladung: METHOD %s, UID %s, %u schreibbare Kalender, vorhanden in %s, Status %d, Konflikte %s",
           i_cal_property_method_to_string (inv->method), uid, inv->calendars->len,
           inv->existing != NULL ? e_source_get_uid (inv->existing) : "-", inv->partstat,
           inv->conflicts != NULL ? "ja" : "nein");
  if (g_task_return_error_if_cancelled (task))
    return;
  g_task_return_boolean (task, TRUE);
}

/* ------------------------------------------------------------------ Antwort (Worker) */

typedef struct
{
  Invitation *inv;
  Respond     respond;
  ESource    *calendar;
  CamelMimeMessage *reply;
  char       *status;
  gboolean    google;   /* #449 */
} Response;

static void
response_free (Response *response)
{
  invitation_unref (response->inv);
  g_clear_object (&response->calendar);
  g_clear_object (&response->reply);
  g_free (response->status);
  g_free (response);
}

/* iTIP-REPLY als Mail (iMIP, RFC 6047) an den Organisator: Teil text/calendar mit METHOD REPLY, Betreff wie Evolution
 * comp_subject (itip-utils.c:1230-1255), Absender die Identität des Kontos. */
static CamelMimeMessage *
build_reply (Response      *response,
             ICalComponent *answered)
{
  Invitation *inv = response->inv;
  g_autoptr (ESource) identity = e_source_registry_ref_source (inv->registry, inv->identity_uid);
  g_autoptr (ICalProperty) organizer = i_cal_component_get_first_property (inv->comp, I_CAL_ORGANIZER_PROPERTY);
  g_autoptr (ICalComponent) top = NULL;
  g_autoptr (CamelInternetAddress) from = camel_internet_address_new ();
  g_autoptr (CamelInternetAddress) to = camel_internet_address_new ();
  g_autofree char *text = NULL, *subject = NULL;
  ICalComponent *comp = i_cal_component_clone (answered);
  CamelMimeMessage *message;
  ESourceMailIdentity *ext;
  const char *prefix;

  if (identity == NULL || organizer == NULL || e_cal_util_get_property_email (organizer) == NULL)
    {
      g_object_unref (comp);
      return NULL;
    }

  remove_all_but_attendee (comp, inv->address);
  for (ICalComponent *alarm = i_cal_component_get_first_component (comp, I_CAL_VALARM_COMPONENT); alarm != NULL;
       alarm = i_cal_component_get_first_component (comp, I_CAL_VALARM_COMPONENT))
    {
      i_cal_component_remove_component (comp, alarm);
      g_object_unref (alarm);
    }
  top = new_toplevel (inv, I_CAL_METHOD_REPLY, comp);
  text = i_cal_component_as_ical_string (top);

  ext = e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_IDENTITY);
  camel_internet_address_add (from, e_source_mail_identity_get_name (ext), e_source_mail_identity_get_address (ext));
  camel_internet_address_add (to, NULL, e_cal_util_get_property_email (organizer));
  /* Übernommen aus Evolution 3.56.2 itip-utils.c:1236, 1243, 1250 (Kontext „Meeting“). */
  prefix = response->respond == RESPOND_ACCEPTED    ? C_("Meeting", "Accepted")
           : response->respond == RESPOND_TENTATIVE ? C_("Meeting", "Tentatively Accepted")
                                                    : C_("Meeting", "Declined");
  subject = g_strdup_printf ("%s: %s", prefix, inv->summary != NULL ? inv->summary : "");

  message = camel_mime_message_new ();
  camel_mime_message_set_from (message, from);
  camel_mime_message_set_recipients (message, CAMEL_RECIPIENT_TYPE_TO, to);
  camel_mime_message_set_subject (message, subject);
  camel_mime_message_set_date (message, CAMEL_MESSAGE_DATE_CURRENT, 0);
  camel_mime_message_set_message_id (message, NULL);
  camel_mime_part_set_content (CAMEL_MIME_PART (message), text, strlen (text),
                               "text/calendar; charset=utf-8; method=REPLY");
  camel_mime_part_set_encoding (CAMEL_MIME_PART (message), CAMEL_TRANSFER_ENCODING_QUOTEDPRINTABLE);

  return message;
}

static void
respond_thread (GTask        *task,
                gpointer      source_object G_GNUC_UNUSED,
                gpointer      task_data,
                GCancellable *cancellable)
{
  Response *response = task_data;
  Invitation *inv = response->inv;
  const char *name = e_source_get_display_name (response->calendar);
  g_autoptr (EClient) client = NULL;
  GError *error = NULL;

  client = e_cal_client_connect_sync (response->calendar, E_CAL_CLIENT_SOURCE_TYPE_EVENTS, 30, cancellable, &error);
  if (client == NULL)
    {
      g_task_return_error (task, error);
      return;
    }

  if (response->respond == RESPOND_UPDATE)
    {
      /* REPLY beim Organisator: gespeicherten Termin mit dem Status des Antwortenden ändern (Evolution
       * update_attendee_status_icomp, ohne Delegation und ohne Rückfrage bei unbekanntem Teilnehmer). */
      g_autoptr (ICalProperty) replier = i_cal_component_get_first_property (inv->comp, I_CAL_ATTENDEE_PROPERTY);
      g_autoptr (ICalComponent) stored = NULL;
      g_autoptr (ICalProperty) known = NULL;

      if (!e_cal_client_get_object_sync (E_CAL_CLIENT (client), i_cal_component_get_uid (inv->comp), NULL, &stored,
                                         cancellable, &error))
        {
          g_task_return_error (task, error);
          return;
        }
      known = replier != NULL ? find_attendee (stored, e_cal_util_get_property_email (replier)) : NULL;
      if (known == NULL || inv->partstat == I_CAL_PARTSTAT_NONE || inv->partstat == I_CAL_PARTSTAT_X)
        {
          /* Übernommen aus Evolution 3.56.2 itip-view.c:6209. */
          g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s",
                                   _("Attendee status could not be updated because the status is invalid"));
          return;
        }
      set_partstat (stored, e_cal_util_get_property_email (replier), inv->partstat);
      if (!e_cal_client_modify_object_sync (E_CAL_CLIENT (client), stored, E_CAL_OBJ_MOD_ALL, E_CAL_OPERATION_FLAG_NONE,
                                            cancellable, &error))
        {
          g_task_return_error (task, error);
          return;
        }
      /* Übernommen aus Evolution 3.56.2 itip-view.c:6099. */
      response->status = g_strdup (_("Attendee status updated"));
    }
  else if (response->respond == RESPOND_REMOVE)
    {
      g_autoptr (ICalComponent) top = new_toplevel (inv, I_CAL_METHOD_CANCEL, i_cal_component_clone (inv->comp));

      if (!e_cal_client_receive_objects_sync (E_CAL_CLIENT (client), top, E_CAL_OPERATION_FLAG_NONE, cancellable, &error))
        {
          g_task_return_error (task, error);
          return;
        }
      /* Übernommen aus Evolution 3.56.2 itip-view.c:5628. */
      response->status = g_strdup_printf (_("Sent to calendar “%s” as cancelled"), name);
    }
  else
    {
      static const ICalParameterPartstat partstats[] = { I_CAL_PARTSTAT_ACCEPTED, I_CAL_PARTSTAT_TENTATIVE,
                                                         I_CAL_PARTSTAT_DECLINED };
      g_autoptr (ICalComponent) comp = i_cal_component_clone (inv->comp);
      g_autoptr (ICalComponent) top = NULL;
      g_autoptr (ICalComponent) stored = NULL;
      gboolean existed = e_cal_client_get_object_sync (E_CAL_CLIENT (client), i_cal_component_get_uid (inv->comp), NULL,
                                                       &stored, cancellable, NULL) && stored != NULL;

      set_partstat (comp, inv->address, partstats[response->respond]);
      update_cdo_replytime (comp);
      /* new_toplevel übernimmt den Verweis (take_component), comp bleibt für die Antwort gültig. */
      top = new_toplevel (inv, I_CAL_METHOD_REQUEST, g_object_ref (comp));
      if (!e_cal_client_receive_objects_sync (E_CAL_CLIENT (client), top, E_CAL_OPERATION_FLAG_NONE, cancellable, &error))
        {
          /* #449: Google-CalDAV (Host wie EDS e-cal-backend-caldav.c:106-109) lehnt das Schreiben mit HTTP 400 ab, wenn
           * Google die Einladung schon versteckt übernommen hat (#447, Messung #376). Fehlerbereich und Code kommen über
           * D-Bus unverändert an (e-webdav-session.c:1007-1044 E_SOUP_SESSION_ERROR, Code = HTTP-Status). */
          ESourceWebdav *webdav = e_source_has_extension (response->calendar, E_SOURCE_EXTENSION_WEBDAV_BACKEND)
                                    ? e_source_get_extension (response->calendar, E_SOURCE_EXTENSION_WEBDAV_BACKEND) : NULL;
          g_autoptr (GUri) uri = webdav != NULL ? e_source_webdav_dup_uri (webdav) : NULL;
          const char *host = uri != NULL ? g_uri_get_host (uri) : NULL;

          response->google = g_error_matches (error, E_SOUP_SESSION_ERROR, SOUP_STATUS_BAD_REQUEST) && host != NULL &&
                             g_strcmp0 (e_source_backend_get_backend_name (
                                          e_source_get_extension (response->calendar, E_SOURCE_EXTENSION_CALENDAR)),
                                        "caldav") == 0 &&
                             (e_util_host_is_in_domain (host, "google.com") ||
                              e_util_host_is_in_domain (host, "googleapis.com") ||
                              e_util_host_is_in_domain (host, "googleusercontent.com"));
          g_debug ("Einladung: Schreibfehler %s %d, Google %s", g_quark_to_string (error->domain), error->code,
                   response->google ? "ja" : "nein");
          g_task_return_error (task, error);
          return;
        }
      /* Übernommen aus Evolution 3.56.2 itip-view.c:5568, 5588, 5608. */
      response->status = g_strdup_printf (response->respond == RESPOND_ACCEPTED ? _("Sent to calendar “%s” as accepted")
                                          : response->respond == RESPOND_TENTATIVE ? _("Sent to calendar “%s” as tentative")
                                                                                   : _("Sent to calendar “%s” as declined"),
                                          name);
      /* Antwort per Mail, wenn der Server nicht plant oder den Termin vorher nicht kannte (Kopf). */
      if (!e_cal_client_check_save_schedules (E_CAL_CLIENT (client)) || !existed)
        response->reply = build_reply (response, comp);
      g_debug ("Einladung: Antwort %d in %s (vorher vorhanden %s, Server plant %s), Antwort per Mail %s",
               response->respond, e_source_get_uid (response->calendar),
               existed ? "ja" : "nein", e_cal_client_check_save_schedules (E_CAL_CLIENT (client)) ? "ja" : "nein",
               response->reply != NULL ? "ja" : "nein");
    }

  if (g_task_return_error_if_cancelled (task))
    return;
  g_task_return_boolean (task, TRUE);
}

/* ------------------------------------------------------------------ Oberfläche */

static void
show_error (RevolutionMailInvitation *self,
            const char               *title,
            const char               *detail)
{
  GtkWidget *view = gtk_widget_get_ancestor (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_VIEW);

  g_warning ("%s: %s", title, detail);
  if (view != NULL)
    revolution_mail_view_show_error (REVOLUTION_MAIL_VIEW (view), title, detail, FALSE);
}

static void
set_busy (RevolutionMailInvitation *self,
          gboolean                  busy)
{
  gtk_widget_set_visible (GTK_WIDGET (self->spinner), busy);
  g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self->actions), "respond")),
                               !busy && self->inv != NULL);
  gtk_widget_set_sensitive (GTK_WIDGET (self->calendar_dropdown), !busy);
}

static void
set_status (RevolutionMailInvitation *self,
            const char               *status)
{
  gtk_label_set_label (self->status_label, status != NULL ? status : "");
  gtk_widget_set_visible (GTK_WIDGET (self->status_label), status != NULL && *status != '\0');
}

static const char *
partstat_status (ICalParameterPartstat partstat)
{
  switch (partstat)
    {
    case I_CAL_PARTSTAT_ACCEPTED:
      return _("Accepted");
    case I_CAL_PARTSTAT_TENTATIVE:
      return _("Tentatively Accepted");
    case I_CAL_PARTSTAT_DECLINED:
      return _("Declined");
    default:
      return NULL;
    }
}

static void
fill (RevolutionMailInvitation *self)
{
  Invitation *inv = self->inv;
  g_autoptr (GtkStringList) names = gtk_string_list_new (NULL);
  gboolean request = inv->method == I_CAL_METHOD_REQUEST;
  gboolean answer;
  g_autofree char *status = NULL;
  const char *state = partstat_status (inv->partstat);

  gtk_label_set_label (self->intro_label, inv->intro);
  gtk_label_set_label (self->title_label, inv->summary != NULL ? inv->summary : "");
  gtk_label_set_label (self->when_label, inv->when);
  gtk_label_set_label (self->where_label, inv->location != NULL ? inv->location : "");
  gtk_widget_set_visible (GTK_WIDGET (self->where_label), inv->location != NULL && *inv->location != '\0');
  gtk_label_set_label (self->organizer_label, inv->organizer);
  gtk_label_set_label (self->conflict_label, inv->conflicts != NULL ? inv->conflicts : "");
  gtk_widget_set_visible (GTK_WIDGET (self->conflict_label), inv->conflicts != NULL);

  for (guint i = 0; i < inv->calendars->len; i++)
    gtk_string_list_append (names, e_source_get_display_name (g_ptr_array_index (inv->calendars, i)));
  gtk_drop_down_set_model (self->calendar_dropdown, G_LIST_MODEL (names));
  if (inv->calendars->len > 0)
    gtk_drop_down_set_selected (self->calendar_dropdown, inv->selected);

  answer = request && inv->calendars->len > 0 && inv->address != NULL;
  gtk_widget_set_visible (self->calendar_box, answer);
  gtk_widget_set_visible (self->accept_button, answer);
  gtk_widget_set_visible (self->tentative_button, answer);
  gtk_widget_set_visible (self->decline_button, answer);
  gtk_widget_set_visible (self->remove_button, inv->method == I_CAL_METHOD_CANCEL && inv->existing != NULL);
  gtk_widget_set_visible (self->update_button, inv->method == I_CAL_METHOD_REPLY && inv->existing != NULL);

  if (inv->method == I_CAL_METHOD_REPLY && state != NULL)
    /* Translators: status of the replying attendee, e.g. “Response: Accepted”. */
    status = g_strdup_printf (_("Response: %s"), state);
  else if (request && state != NULL && inv->existing != NULL)
    /* Translators: own response already in the calendar, e.g. “Accepted in “Calendar””. */
    status = g_strdup_printf (_("%s in “%s”"), state, e_source_get_display_name (inv->existing));
  if (inv->existing == NULL && inv->method != I_CAL_METHOD_REQUEST)
    {
      /* Übernommen aus Evolution 3.56.2 itip-view.c:4777. */
      g_autofree char *missing = g_strdup (_("Unable to find this meeting in any calendar"));

      g_free (status);
      status = status != NULL ? g_strdup_printf ("%s\n%s", status, missing) : g_steal_pointer (&missing);
    }
  else if (request && inv->address == NULL)
    {
      g_free (status);
      status = g_strdup (_("You are not an attendee of this meeting"));
    }
  else if (request && inv->calendars->len == 0)
    status = g_strdup (_("No writable calendar"));
  set_status (self, status);
}

static void
on_analyzed (GObject      *source_object,
             GAsyncResult *result,
             gpointer      user_data)
{
  RevolutionMailInvitation *self = REVOLUTION_MAIL_INVITATION (source_object);
  Invitation *inv = g_task_get_task_data (G_TASK (result));
  g_autoptr (GError) error = NULL;

  if (!g_task_propagate_boolean (G_TASK (result), &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Einladung: keine Karte (%s)", error->message);
      if (GPOINTER_TO_UINT (user_data) == self->generation)
        gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
      return;
    }
  if (GPOINTER_TO_UINT (user_data) != self->generation)
    return;

  self->inv = g_rc_box_acquire (inv);
  fill (self);
  set_busy (self, FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self), TRUE);
}

static void
on_reply_sent (GObject      *source G_GNUC_UNUSED,
               GAsyncResult *result,
               gpointer      user_data)
{
  g_autoptr (RevolutionMailInvitation) self = user_data;
  g_autoptr (GError) error = NULL;

  if (!revolution_mail_send_message_finish (result, &error))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        show_error (self, _("Could not send meeting response"), error->message);
      return;
    }
  g_debug ("Einladung: Antwort im Postausgang");
}

static void
on_responded (GObject      *source_object,
              GAsyncResult *result,
              gpointer      user_data)
{
  RevolutionMailInvitation *self = REVOLUTION_MAIL_INVITATION (source_object);
  Response *response = g_task_get_task_data (G_TASK (result));
  g_autoptr (GError) error = NULL;
  gboolean current = GPOINTER_TO_UINT (user_data) == self->generation;

  if (current)
    set_busy (self, FALSE);
  if (!g_task_propagate_boolean (G_TASK (result), &error))
    {
      if (response->google)
        show_error (self, _("Google has already added this invitation to your Google Calendar. Please respond there."),
                    error->message);
      else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        show_error (self, _("Unable to update calendar"), error->message);
      return;
    }

  g_debug ("Einladung: %s", response->status);
  if (current)
    {
      set_status (self, response->status);
      /* Nach dem Entfernen gibt es nichts mehr zu entfernen. */
      if (response->respond == RESPOND_REMOVE)
        gtk_widget_set_visible (self->remove_button, FALSE);
    }
  /* Antwort unabhängig von der angezeigten Mail senden (Postausgang, #362). */
  if (response->reply != NULL && response->inv->identity_uid != NULL)
    revolution_mail_send_message (response->reply, response->inv->identity_uid, NULL, on_reply_sent,
                                  g_object_ref (self));
  else if (response->reply != NULL)
    show_error (self, _("Could not send meeting response"), _("No identity for this account"));
}

static void
on_respond (GSimpleAction *action G_GNUC_UNUSED,
            GVariant      *parameter,
            gpointer       user_data)
{
  RevolutionMailInvitation *self = user_data;
  static const char * const names[] = { "accepted", "tentative", "declined", "remove", "update" };
  const char *name = g_variant_get_string (parameter, NULL);
  g_autoptr (GTask) task = NULL;
  Response *response;
  guint selected;

  if (self->inv == NULL)
    return;

  response = g_new0 (Response, 1);
  response->inv = g_rc_box_acquire (self->inv);
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    if (g_strcmp0 (name, names[i]) == 0)
      response->respond = i;
  selected = gtk_drop_down_get_selected (self->calendar_dropdown);
  if (response->respond >= RESPOND_REMOVE)
    response->calendar = self->inv->existing != NULL ? g_object_ref (self->inv->existing) : NULL;
  else if (selected < self->inv->calendars->len)
    response->calendar = g_object_ref (g_ptr_array_index (self->inv->calendars, selected));
  if (response->calendar == NULL)
    {
      response_free (response);
      return;
    }
  if (response->respond <= RESPOND_DECLINED && self->inv->address == NULL)
    {
      response_free (response);
      show_error (self, _("Unable to update calendar"), _("You are not an attendee of this meeting"));
      return;
    }

  g_debug ("Einladung: Antwort %s, Kalender %s", name, e_source_get_uid (response->calendar));
  set_busy (self, TRUE);
  task = g_task_new (self, self->cancellable, on_responded, GUINT_TO_POINTER (self->generation));
  g_task_set_source_tag (task, on_respond);
  g_task_set_task_data (task, response, (GDestroyNotify) response_free);
  g_task_run_in_thread (task, respond_thread);
}

static const GActionEntry invitation_actions[] = {
  { "respond", on_respond, "s", NULL, NULL, { 0 } },
};

/* Kalenderteil der angezeigten Mail (mail-reader.c), NULL blendet die Karte aus. account_uid: Konto der Mail, für
 * die eigene Adresse und die Identität der Antwort. */
void
revolution_mail_invitation_set (RevolutionMailInvitation *self,
                                const char               *account_uid,
                                const char               *ical)
{
  ESourceRegistry *registry;
  g_autoptr (GTask) task = NULL;
  Invitation *inv;

  g_return_if_fail (REVOLUTION_IS_MAIL_INVITATION (self));

  self->generation++;
  g_clear_pointer (&self->inv, invitation_unref);
  gtk_widget_set_visible (GTK_WIDGET (self), FALSE);
  set_busy (self, FALSE);

  registry = g_application_get_default () != NULL
               ? g_object_get_data (G_OBJECT (g_application_get_default ()), "registry") : NULL;
  if (ical == NULL || registry == NULL)
    return;

  inv = g_rc_box_new0 (Invitation);
  inv->registry = g_object_ref (registry);
  inv->account_uid = g_strdup (account_uid);
  inv->ical = g_strdup (ical);
  task = g_task_new (self, self->cancellable, on_analyzed, GUINT_TO_POINTER (self->generation));
  g_task_set_source_tag (task, revolution_mail_invitation_set);
  g_task_set_task_data (task, inv, (GDestroyNotify) invitation_unref);
  g_task_run_in_thread (task, analyze_thread);
}

static void
revolution_mail_invitation_dispose (GObject *object)
{
  RevolutionMailInvitation *self = REVOLUTION_MAIL_INVITATION (object);

  self->generation++;
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_pointer (&self->inv, invitation_unref);
  g_clear_object (&self->actions);
  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_INVITATION);

  G_OBJECT_CLASS (revolution_mail_invitation_parent_class)->dispose (object);
}

static void
revolution_mail_invitation_class_init (RevolutionMailInvitationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = revolution_mail_invitation_dispose;

  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/mail-invitation.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailInvitation, intro_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailInvitation, title_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailInvitation, when_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailInvitation, where_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailInvitation, organizer_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailInvitation, conflict_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailInvitation, status_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailInvitation, calendar_box);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailInvitation, calendar_dropdown);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailInvitation, accept_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailInvitation, tentative_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailInvitation, decline_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailInvitation, remove_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailInvitation, update_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailInvitation, spinner);
}

static void
revolution_mail_invitation_init (RevolutionMailInvitation *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->cancellable = g_cancellable_new ();
  self->actions = g_simple_action_group_new ();
  g_action_map_add_action_entries (G_ACTION_MAP (self->actions), invitation_actions, G_N_ELEMENTS (invitation_actions),
                                   self);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "invitation", G_ACTION_GROUP (self->actions));
}

/* ------------------------------------------------------------------ Eigene Einladungen (#377) */

/* Verhalten wie Evolution 3.56.2 src/calendar/gui/itip-utils.c (Vorlage, nicht übernommen):
 *   nur als Organisator (itip_organizer_is_user 227-280: cal-email-address oder eine Identität)
 *   Server plant (save-schedules), außer Absage bei retract-supported: keine Mail (itip_send_component_begin 2219-2224)
 *   sonst zuerst das Backend (e_cal_client_send_objects_sync, comp_server_send_sync), bei create-messages schickt es
 *   selbst (2266-2280; microsoft365: Graph „cancel“, e-cal-backend-m365.c:1737-1828)
 *   sonst Mail an alle Teilnehmer außer dem Organisator, ohne Erinnerungen, mit den VTIMEZONEs (foreach_tzid_callback
 *   686-760), Betreff comp_subject 1167-1300; Versand über den Postausgang (#362) statt Composer. */
typedef struct
{
  ECalClient         *client;
  ICalComponent      *comp;
  ICalPropertyMethod  method;
  gboolean            master;
  CamelMimeMessage   *message;
  char               *identity_uid;
} Send;

static void
send_free (Send *send)
{
  g_clear_object (&send->client);
  g_clear_object (&send->comp);
  g_clear_object (&send->message);
  g_free (send->identity_uid);
  g_free (send);
}

typedef struct
{
  ECalClient    *client;
  ICalComponent *top;
  GHashTable    *seen;    /* TZID bzw. Ort → Ort (NULL: schon als Ort eingetragen) */
  ICalTime      *from;
  ICalTime      *to;
} Zones;

/* Wie Evolution 3.56.2 itip-utils.c:686-760 (foreach_tzid_callback): TZID im Parameter und in der VTIMEZONE auf den
 * Ort („Europe/Berlin“ statt „/freeassociation.sourceforge.net/…“), VTIMEZONE auf den Zeitraum des Termins gekürzt
 * (e_cal_util_clamp_vtimezone, 748-749, Zeitraum 785-800). */
static void
add_zone (ICalParameter *param,
          gpointer       user_data)
{
  Zones *zones = user_data;
  g_autofree char *tzid = g_strdup (i_cal_parameter_get_tzid (param));
  ICalTimezone *zone = NULL;
  ICalComponent *vtimezone, *tzcomp;
  const char *location;
  gpointer known = NULL;

  if (tzid == NULL)
    return;
  if (g_hash_table_lookup_extended (zones->seen, tzid, NULL, &known))
    {
      if (known != NULL)
        i_cal_parameter_set_tzid (param, known);
      return;
    }
  zone = i_cal_timezone_get_builtin_timezone_from_tzid (tzid);
  if (zone == NULL && !e_cal_client_get_timezone_sync (zones->client, tzid, &zone, NULL, NULL))
    zone = NULL;
  vtimezone = zone != NULL ? i_cal_timezone_get_component (zone) : NULL;
  if (vtimezone == NULL)
    return;
  tzcomp = i_cal_component_clone (vtimezone);
  g_object_unref (vtimezone);
  location = i_cal_timezone_get_location (zone);
  if (location == NULL || *location == '\0')
    location = NULL;
  if (location != NULL)
    {
      g_autoptr (ICalProperty) prop = i_cal_component_get_first_property (tzcomp, I_CAL_TZID_PROPERTY);

      i_cal_parameter_set_tzid (param, location);
      /* Ort zuerst prüfen, die TZID erst am Ende eintragen (Evolution 731-737, 757); sonst ginge bei TZID == Ort die
       * VTIMEZONE verloren. */
      if (g_hash_table_contains (zones->seen, location))
        {
          g_hash_table_insert (zones->seen, g_strdup (tzid), g_strdup (location));
          g_object_unref (tzcomp);
          return;
        }
      if (prop != NULL)
        i_cal_property_set_tzid (prop, location);
      g_hash_table_insert (zones->seen, g_strdup (location), NULL);
    }
  if (zones->from != NULL)
    e_cal_util_clamp_vtimezone (tzcomp, zones->from, zones->to);
  g_debug ("Einladung: VTIMEZONE %s (%u Abschnitte)", location != NULL ? location : tzid,
           (guint) (i_cal_component_count_components (tzcomp, I_CAL_XSTANDARD_COMPONENT) +
                    i_cal_component_count_components (tzcomp, I_CAL_XDAYLIGHT_COMPONENT)));
  i_cal_component_take_component (zones->top, tzcomp);
  g_hash_table_insert (zones->seen, g_strdup (tzid), g_strdup (location));
}

/* Übernommen aus Evolution 3.56.2 itip-utils.c:1681-1735 (strip_x_microsoft_props, remove_schedule_status_parameters),
 * an ICalComponent angepasst; dazu X-LIC-ERROR wie e_cal_component_strip_errors (1839). */
static void
strip_for_sending (ICalComponent *comp)
{
  GSList *x = NULL;
  ICalPropertyKind kinds[] = { I_CAL_ORGANIZER_PROPERTY, I_CAL_ATTENDEE_PROPERTY };

  for (ICalProperty *prop = i_cal_component_get_first_property (comp, I_CAL_X_PROPERTY); prop != NULL;
       g_object_unref (prop), prop = i_cal_component_get_next_property (comp, I_CAL_X_PROPERTY))
    {
      const char *name = i_cal_property_get_x_name (prop);

      if (name != NULL && g_ascii_strncasecmp (name, "X-MICROSOFT-", 12) == 0)
        x = g_slist_prepend (x, g_object_ref (prop));
    }
  for (GSList *l = x; l != NULL; l = l->next)
    i_cal_component_remove_property (comp, l->data);
  g_slist_free_full (x, g_object_unref);
  i_cal_component_strip_errors (comp);
  for (gsize i = 0; i < G_N_ELEMENTS (kinds); i++)
    for (ICalProperty *prop = i_cal_component_get_first_property (comp, kinds[i]); prop != NULL;
         g_object_unref (prop), prop = i_cal_component_get_next_property (comp, kinds[i]))
      i_cal_property_remove_parameter_by_kind (prop, I_CAL_SCHEDULESTATUS_PARAMETER);
}

/* Zeilenumbrüche aus Betreff und Namen (Kopfzeilen der Mail). */
static char *
one_line (const char *text)
{
  return g_strdelimit (g_strdup (text != NULL ? text : ""), "\r\n", ' ');
}

static void
send_thread (GTask        *task,
             gpointer      source_object G_GNUC_UNUSED,
             gpointer      task_data,
             GCancellable *cancellable)
{
  Send *send = task_data;
  ESourceRegistry *registry = g_object_get_data (G_OBJECT (g_application_get_default ()), "registry");
  g_autoptr (ICalProperty) organizer = i_cal_component_get_first_property (send->comp, I_CAL_ORGANIZER_PROPERTY);
  g_autolist (ESource) identities = registry != NULL ? e_source_registry_list_enabled (registry, E_SOURCE_EXTENSION_MAIL_IDENTITY) : NULL;
  g_autoptr (CamelInternetAddress) from = camel_internet_address_new ();
  g_autoptr (CamelInternetAddress) to = camel_internet_address_new ();
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  g_autoptr (ICalComponent) master = NULL;
  g_autoptr (ICalTime) now = NULL;
  g_autoptr (ICalTime) dtstart = NULL;
  g_autoptr (ICalTime) dtend = NULL;
  g_autofree char *summary = NULL;
  ICalComponent *source = send->comp;
  g_autofree char *cal_address = NULL, *text = NULL, *subject = NULL, *content_type = NULL;
  g_autoptr (ICalComponent) top = NULL;
  const char *method = i_cal_property_method_to_string (send->method);
  const char *address = organizer != NULL ? e_cal_util_get_property_email (organizer) : NULL;
  ESource *identity = NULL;
  ICalComponent *comp;
  GError *error = NULL;
  Zones zones;

  if (address == NULL || i_cal_component_count_properties (send->comp, I_CAL_ATTENDEE_PROPERTY) == 0)
    {
      g_task_return_boolean (task, TRUE);
      return;
    }
  for (GList *l = identities; l != NULL && identity == NULL; l = l->next)
    if (e_cal_util_email_addresses_equal (e_source_mail_identity_get_address (
                                            e_source_get_extension (l->data, E_SOURCE_EXTENSION_MAIL_IDENTITY)), address))
      identity = l->data;
  if (identity == NULL &&
      !(e_client_get_backend_property_sync (E_CLIENT (send->client), E_CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS,
                                            &cal_address, cancellable, NULL) &&
        cal_address != NULL && e_cal_util_email_addresses_equal (cal_address, address)))
    {
      g_debug ("Einladung: %s nicht gesendet, nicht Organisator", method);
      g_task_return_boolean (task, TRUE);
      return;
    }
  if (e_cal_client_check_save_schedules (send->client) &&
      (send->method != I_CAL_METHOD_CANCEL ||
       !e_client_check_capability (E_CLIENT (send->client), E_CAL_STATIC_CAPABILITY_RETRACT_SUPPORTED)))
    {
      g_debug ("Einladung: %s Server plant", method);
      g_task_return_boolean (task, TRUE);
      return;
    }

  /* „Alle Termine“ einer Serie: Absage mit dem Hauptdatensatz ohne RECURRENCE-ID, wie Evolution
   * E_ITIP_SEND_COMPONENT_FLAG_ENSURE_MASTER_OBJECT (e-calendar-view.c:684-686, itip-utils.c:2226-2241). */
  if (send->master)
    {
      GSList *objects = NULL;

      if (e_cal_client_get_objects_for_uid_sync (send->client, i_cal_component_get_uid (send->comp), &objects,
                                                 cancellable, NULL))
        for (GSList *l = objects; l != NULL && master == NULL; l = l->next)
          if (!e_cal_component_is_instance (l->data))
            master = i_cal_component_clone (e_cal_component_get_icalcomponent (l->data));
      g_slist_free_full (objects, g_object_unref);
      if (master != NULL)
        source = master;
    }
  summary = one_line (i_cal_component_get_summary (source));

  comp = i_cal_component_clone (source);
  for (ICalComponent *alarm = i_cal_component_get_first_component (comp, I_CAL_VALARM_COMPONENT); alarm != NULL;
       alarm = i_cal_component_get_first_component (comp, I_CAL_VALARM_COMPONENT))
    {
      i_cal_component_remove_component (comp, alarm);
      g_object_unref (alarm);
    }
  if (send->method == I_CAL_METHOD_CANCEL)
    i_cal_component_set_status (comp, I_CAL_STATUS_CANCELLED);
  /* Wie Evolution comp_compliant_one (itip-utils.c:1749-1752, 1834-1837, 1873). */
  now = i_cal_time_new_current_with_zone (i_cal_timezone_get_utc_timezone ());
  i_cal_component_set_dtstamp (comp, now);
  strip_for_sending (comp);
  dtstart = i_cal_component_get_dtstart (comp);
  if (i_cal_component_count_properties (comp, I_CAL_RRULE_PROPERTY) == 0)
    {
      dtend = i_cal_component_get_dtend (comp);
      if (dtend == NULL || i_cal_time_is_null_time (dtend))
        g_set_object (&dtend, dtstart);
    }
  top = i_cal_component_new_vcalendar ();
  i_cal_component_take_property (top, i_cal_property_new_version ("2.0"));
  i_cal_component_take_property (top, i_cal_property_new_prodid ("-//Revolution//" PACKAGE_VERSION "//EN"));
  i_cal_component_set_method (top, send->method);
  zones = (Zones) { send->client, top, seen,
                    dtstart != NULL && !i_cal_time_is_null_time (dtstart) ? dtstart : NULL, dtend };
  i_cal_component_foreach_tzid (comp, add_zone, &zones);
  i_cal_component_take_component (top, comp);

  if (send->method == I_CAL_METHOD_CANCEL &&
      e_client_check_capability (E_CLIENT (send->client), E_CAL_STATIC_CAPABILITY_RETRACT_SUPPORTED))
    {
      GSList *users = NULL;
      ICalComponent *modified = NULL;

      if (!e_cal_client_send_objects_sync (send->client, top, E_CAL_OPERATION_FLAG_NONE, &users, &modified, cancellable,
                                           &error))
        {
          g_task_return_error (task, error);
          return;
        }
      g_slist_free_full (users, g_free);
      g_clear_object (&modified);
    }
  if (e_client_check_capability (E_CLIENT (send->client), E_CAL_STATIC_CAPABILITY_CREATE_MESSAGES))
    {
      g_debug ("Einladung: %s vom Server verschickt", method);
      g_task_return_boolean (task, TRUE);
      return;
    }
  if (identity == NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("No identity for %s"), address);
      return;
    }

  for (ICalProperty *prop = i_cal_component_get_first_property (source, I_CAL_ATTENDEE_PROPERTY); prop != NULL;
       g_object_unref (prop), prop = i_cal_component_get_next_property (source, I_CAL_ATTENDEE_PROPERTY))
    {
      const char *attendee = e_cal_util_get_property_email (prop);
      g_autoptr (ICalParameter) cn = i_cal_property_get_first_parameter (prop, I_CAL_CN_PARAMETER);
      g_autofree char *name = cn != NULL ? one_line (i_cal_parameter_get_cn (cn)) : NULL;

      if (attendee != NULL && !e_cal_util_email_addresses_equal (attendee, address))
        camel_internet_address_add (to, name, attendee);
    }
  if (camel_address_length (CAMEL_ADDRESS (to)) == 0)
    {
      g_task_return_boolean (task, TRUE);
      return;
    }

  camel_internet_address_add (from,
                              e_source_mail_identity_get_name (e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_IDENTITY)),
                              address);
  /* Übernommen aus Evolution 3.56.2 itip-utils.c:1274-1278 (Kontext „Meeting“, „Cancel“). */
  subject = send->method == I_CAL_METHOD_CANCEL ? g_strdup_printf ("%s: %s", C_("Meeting", "Cancel"), summary)
                                                : g_strdup (summary);
  text = i_cal_component_as_ical_string (top);
  content_type = g_strdup_printf ("text/calendar; charset=utf-8; method=%s", method);
  send->message = camel_mime_message_new ();
  camel_mime_message_set_from (send->message, from);
  camel_mime_message_set_recipients (send->message, CAMEL_RECIPIENT_TYPE_TO, to);
  camel_mime_message_set_subject (send->message, subject);
  camel_mime_message_set_date (send->message, CAMEL_MESSAGE_DATE_CURRENT, 0);
  camel_mime_message_set_message_id (send->message, NULL);
  /* Wie Evolution multipart/alternative aus Text und Kalenderteil (itip-utils.c:2330-2354,
   * e_msg_composer_set_alternative_body); Exchange trägt eine reine text/calendar-Mail nicht ein (gemessen 30.10.). */
  {
    g_autoptr (CamelMultipart) alternative = camel_multipart_new ();
    g_autoptr (CamelMimePart) body = camel_mime_part_new ();
    g_autoptr (CamelMimePart) calendar = camel_mime_part_new ();

    camel_data_wrapper_set_mime_type (CAMEL_DATA_WRAPPER (alternative), "multipart/alternative");
    camel_multipart_set_boundary (alternative, NULL);
    camel_mime_part_set_content (body, subject, strlen (subject), "text/plain; charset=utf-8");
    camel_mime_part_set_encoding (body, CAMEL_TRANSFER_ENCODING_QUOTEDPRINTABLE);
    camel_mime_part_set_content (calendar, text, strlen (text), content_type);
    camel_mime_part_set_encoding (calendar, CAMEL_TRANSFER_ENCODING_QUOTEDPRINTABLE);
    camel_multipart_add_part (alternative, body);
    camel_multipart_add_part (alternative, calendar);
    camel_medium_set_content (CAMEL_MEDIUM (send->message), CAMEL_DATA_WRAPPER (alternative));
  }
  send->identity_uid = g_strdup (e_source_get_uid (identity));
  g_debug ("Einladung: %s an %d Teilnehmer über den Postausgang", method, camel_address_length (CAMEL_ADDRESS (to)));
  g_task_return_boolean (task, TRUE);
}

static void
on_invitation_sent (GObject      *source G_GNUC_UNUSED,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  GError *error = NULL;

  if (!revolution_mail_send_message_finish (result, &error))
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);
}

static void
on_send_prepared (GObject      *source G_GNUC_UNUSED,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_autoptr (GTask) task = user_data;
  Send *send = g_task_get_task_data (task);
  GError *error = NULL;

  if (!g_task_propagate_boolean (G_TASK (result), &error))
    g_task_return_error (task, error);
  else if (send->message == NULL)
    g_task_return_boolean (task, TRUE);
  else
    revolution_mail_send_message (send->message, send->identity_uid, NULL, on_invitation_sent, g_steal_pointer (&task));
}

void
revolution_mail_invitation_send (ECalClient          *client,
                                 ICalComponent       *comp,
                                 ICalPropertyMethod   method,
                                 gboolean             master,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
  GTask *task = g_task_new (NULL, NULL, callback, user_data);
  g_autoptr (GTask) worker = NULL;
  Send *send = g_new0 (Send, 1);

  send->client = g_object_ref (client);
  send->comp = comp;
  send->method = method;
  send->master = master;
  g_task_set_source_tag (task, revolution_mail_invitation_send);
  g_task_set_task_data (task, send, (GDestroyNotify) send_free);
  worker = g_task_new (NULL, NULL, on_send_prepared, task);
  g_task_set_task_data (worker, send, NULL);
  g_task_run_in_thread (worker, send_thread);
}

gboolean
revolution_mail_invitation_send_finish (GAsyncResult *result,
                                        GError      **error)
{
  return g_task_propagate_boolean (G_TASK (result), error);
}
