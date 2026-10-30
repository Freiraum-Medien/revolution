/* Übernommen aus gnome-calendar 50.0 src/core/gcal-manager.c:1-1394 (ff4d7f7d, GPL-3.0-or-later). Revolution
 * #373: ohne Credential-Prompter (500-620), ohne Quellen-Commit beim Laden (296-310), ohne add/save_source,
 * refresh und Standardkalender; Registry der App statt e_source_registry_new_sync (1299); Sichtbarkeit in den
 * Einstellungen der App („hidden-calendars“). #375: Schreib-API 1033-1230 (create/update/remove_event asynchron wie
 * Vorlage, move_event_to_source im GTask-Worker statt *_sync im UI-Thread), Standardkalender 835-850 nur gelesen
 * (ohne gcal_manager_set_default_calendar: schriebe die Evolution-Einstellung), Fehler als Signal „error“ für das
 * Banner der Ansicht statt g_warning (241, 381, 417, 452; Monitor über GcalCalendar::error), Log für die Tests. */
/* gcal-manager.c
 *
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

#define G_LOG_DOMAIN "GcalManager"

#include "gcal-context.h"
#include "gcal-debug.h"
#include "gcal-manager.h"
#include "gcal-timeline.h"
#include "gcal-timeline-subscriber.h"
#include "gcal-utils.h"

#include <glib/gi18n.h>

/**
 * SECTION:gcal-manager
 * @short_description: The backend of GNOME Calendar
 * @title:GcalManager
 * @stability:unstable
 *
 * #GcalManager is the backend of GNOME Calendar. It sets everything
 * up, connects to the Online Accounts daemon, and manages the events
 * and calendars.
 */

struct _GcalManager
{
  GObject             parent;

  /**
   * The list of clients we are managing.
   * Each value is of type GCalStoreUnit
   * And each key is the source uid
   */
  GHashTable         *clients;
  GListStore         *calendars_model;

  ESourceRegistry    *source_registry;

  gint                clients_synchronizing;

  GcalTimeline       *timeline;

  GcalContext        *context;
};

G_DEFINE_TYPE (GcalManager, gcal_manager, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_CONTEXT,
  PROP_SYNCHRONIZING,
  NUM_PROPS
};

enum
{
  CALENDAR_ADDED,
  CALENDAR_CHANGED,
  CALENDAR_REMOVED,
  ERROR,
  NUM_SIGNALS
};

static guint       signals[NUM_SIGNALS] = { 0, };
static GParamSpec *properties[NUM_PROPS] = { NULL, };

/* Revolution #375: Fehler für das Sammel-Banner der Ansicht (Architektur 3) und das Log der Tests. */
static void
report_error (GcalManager *self,
              const gchar *title,
              const gchar *detail)
{
  g_log ("revolution", G_LOG_LEVEL_DEBUG, "Kalender: Fehler: %s: %s", title, detail);
  g_signal_emit (self, signals[ERROR], 0, title, detail);
}

static void
on_calendar_error_cb (GcalCalendar *calendar,
                      const gchar  *detail,
                      GcalManager  *self)
{
  g_autofree gchar *title = g_strdup_printf (_("Could not load calendar “%s”"), gcal_calendar_get_name (calendar));

  report_error (self, title, detail);
}

static void
remove_source (GcalManager  *self,
               ESource      *source)
{
  g_autoptr (GcalCalendar) calendar = NULL;
  guint position;

  GCAL_ENTRY;

  g_return_if_fail (GCAL_IS_MANAGER (self));
  g_return_if_fail (E_IS_SOURCE (source));

  calendar = g_hash_table_lookup (self->clients, source);

  if (!calendar)
    return;

  GCAL_TRACE_MSG ("Removing calendar %s (%s)",
                  e_source_get_display_name (source),
                  e_source_get_uid (source));

  g_object_ref (calendar);

  gcal_timeline_remove_calendar (self->timeline, calendar);
  g_hash_table_remove (self->clients, source);

  g_list_store_find (self->calendars_model, calendar, &position);
  g_list_store_remove (self->calendars_model, position);

  g_signal_emit (self, signals[CALENDAR_REMOVED], 0, calendar);

  GCAL_EXIT;
}

static void
source_changed (GcalManager *self,
                ESource     *source)
{
  GcalCalendar *calendar;

  GCAL_ENTRY;

  calendar = g_hash_table_lookup (self->clients, source);

  if (!calendar)
    GCAL_RETURN ();

  g_signal_emit (self, signals[CALENDAR_CHANGED], 0, calendar);

  GCAL_EXIT;
}

static void
on_client_refreshed (GObject      *source_object,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  GcalManager *self = GCAL_MANAGER (user_data);
  GError *error = NULL;

  GCAL_ENTRY;

  if (e_client_refresh_finish (E_CLIENT (source_object), result, &error))
    {
      ESource *source = e_client_get_source (E_CLIENT (source_object));
      /* FIXME: add notification to UI */
      g_debug ("Client of source: %s refreshed successfully", e_source_get_uid (source));
    }
  else
    {
      /* FIXME: do something when there was some error */
      g_warning ("Error synchronizing client");
    }

  self->clients_synchronizing--;
  if (self->clients_synchronizing == 0)
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SYNCHRONIZING]);

  GCAL_EXIT;
}

static gint
sort_calendar_by_name_cb (gconstpointer a,
                          gconstpointer b,
                          gpointer      user_data)
{
  GcalCalendar *calendar_a = GCAL_CALENDAR ((gpointer) a);
  GcalCalendar *calendar_b = GCAL_CALENDAR ((gpointer) b);
  g_autofree gchar *parent_name_a = NULL;
  g_autofree gchar *parent_name_b = NULL;
  gint retval;

  retval = g_utf8_collate (gcal_calendar_get_name (calendar_a), gcal_calendar_get_name (calendar_b));
  if (retval != 0)
    return retval;

  parent_name_a = e_source_dup_display_name (gcal_calendar_get_parent_source (calendar_a));
  parent_name_b = e_source_dup_display_name (gcal_calendar_get_parent_source (calendar_b));

  return g_utf8_collate (parent_name_a, parent_name_b);
}

static void
on_calendar_name_changed_cb (GcalCalendar *calendar,
                             GParamSpec   *pspec,
                             GcalManager  *self)
{
  g_list_store_sort (self->calendars_model, sort_calendar_by_name_cb, NULL);
}

/* Revolution #373: Sichtbarkeit in den Einstellungen der App (Schlüssel „hidden-calendars“),
 * nie in der ESource (GNOME Calendar schreibt „selected“ der ESource, gcal-calendar.c:98). */
static void
on_calendar_visible_changed_cb (GcalCalendar *calendar,
                                GParamSpec   *pspec,
                                GcalManager  *self)
{
  GSettings *settings = gcal_context_get_settings (self->context);
  g_auto (GStrv) hidden = g_settings_get_strv (settings, "hidden-calendars");
  g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();
  g_auto (GStrv) result = NULL;
  const gchar *id = gcal_calendar_get_id (calendar);

  for (gsize i = 0; hidden[i] != NULL; i++)
    {
      if (g_strcmp0 (hidden[i], id) != 0)
        g_strv_builder_add (builder, hidden[i]);
    }

  if (!gcal_calendar_get_visible (calendar))
    g_strv_builder_add (builder, id);

  result = g_strv_builder_end (builder);
  g_settings_set_strv (settings, "hidden-calendars", (const gchar * const *) result);
}

static void
on_calendar_created_cb (GObject      *source_object,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  g_autoptr (GError) error = NULL;
  GcalCalendar *calendar;
  GcalManager *self;
  ECalClient *client;
  ESource *source;
  gboolean visible;

  self = GCAL_MANAGER (user_data);
  calendar = gcal_calendar_new_finish (result, &error);

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
          !g_error_matches (error, GCAL_CALENDAR_ERROR, GCAL_CALENDAR_ERROR_NOT_CALENDAR))
        {
          ESource *source = gcal_calendar_get_source (GCAL_CALENDAR (source_object));
          g_autofree gchar *title = g_strdup_printf (_("Could not connect to calendar “%s”"),
                                                     e_source_get_display_name (source));

          report_error (self, title, error->message);
        }
      return;
    }

  g_debug ("Source %s (%s) connected",
           gcal_calendar_get_name (calendar),
           gcal_calendar_get_id (calendar));

  {
    g_auto (GStrv) hidden = g_settings_get_strv (gcal_context_get_settings (self->context), "hidden-calendars");

    gcal_calendar_set_visible (calendar, !g_strv_contains ((const gchar * const *) hidden, gcal_calendar_get_id (calendar)));
    g_signal_connect (calendar, "notify::visible", G_CALLBACK (on_calendar_visible_changed_cb), self);
  }

  visible = gcal_calendar_get_visible (calendar);
  client = gcal_calendar_get_client (calendar);
  source = gcal_calendar_get_source (calendar);

  g_hash_table_insert (self->clients, g_object_ref (source), calendar);
  g_list_store_insert_sorted (self->calendars_model, calendar, sort_calendar_by_name_cb, self);
  g_signal_connect (calendar, "notify::name", G_CALLBACK (on_calendar_name_changed_cb), self);
  g_signal_connect (calendar, "error", G_CALLBACK (on_calendar_error_cb), self);

  gcal_timeline_add_calendar (self->timeline, calendar);

  /* refresh client when it's added */
  if (visible && e_client_check_refresh_supported (E_CLIENT (client)))
    {
      e_client_refresh (E_CLIENT (client), NULL, on_client_refreshed, user_data);

      self->clients_synchronizing++;
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SYNCHRONIZING]);
    }

  g_signal_emit (self, signals[CALENDAR_ADDED], 0, calendar);
}

static void
load_source (GcalManager *self,
             ESource     *source)
{
  g_autoptr (ESource) parent = NULL;

  GCAL_ENTRY;

  if (g_hash_table_contains (self->clients, source))
    {
      g_warning ("%s: Skipping already loaded source: %s",
                 G_STRFUNC,
                 e_source_get_uid (source));
      return;
    }

  parent = e_source_registry_ref_source (self->source_registry, e_source_get_parent (source));

  gcal_calendar_new (source, parent, NULL, on_calendar_created_cb, self);

  GCAL_EXIT;
}

static void
gcal_manager_finalize (GObject *object)
{
  GcalManager *self =GCAL_MANAGER (object);

  GCAL_ENTRY;

  g_clear_object (&self->timeline);

  if (self->context)
    {
      g_object_remove_weak_pointer (G_OBJECT (self->context), (gpointer *)&self->context);
      self->context = NULL;
    }

  g_clear_object (&self->calendars_model);
  g_clear_pointer (&self->clients, g_hash_table_destroy);
  g_clear_object (&self->source_registry);

  G_OBJECT_CLASS (gcal_manager_parent_class)->finalize (object);

  GCAL_EXIT;
}

static void
gcal_manager_set_property (GObject      *object,
                           guint         property_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  GcalManager *self = GCAL_MANAGER (object);

  GCAL_ENTRY;

  switch (property_id)
    {
    case PROP_CONTEXT:
      g_assert (self->context == NULL);
      self->context = g_value_get_object (value);
      g_object_add_weak_pointer (G_OBJECT (self->context), (gpointer *)&self->context);
      break;

    case PROP_SYNCHRONIZING:
      g_assert_not_reached ();
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }

  GCAL_EXIT;
}

static void
gcal_manager_get_property (GObject    *object,
                           guint       property_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  GcalManager *self = GCAL_MANAGER (object);

  GCAL_ENTRY;

  switch (property_id)
    {
    case PROP_CONTEXT:
      g_value_set_object (value, self->context);
      break;

    case PROP_SYNCHRONIZING:
      g_value_set_boolean (value, self->clients_synchronizing != 0);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }

  GCAL_EXIT;
}

static void
gcal_manager_class_init (GcalManagerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gcal_manager_finalize;
  object_class->set_property = gcal_manager_set_property;
  object_class->get_property = gcal_manager_get_property;

  /**
   * GcalManager:context:
   *
   * The #GcalContext.
   */
  properties[PROP_CONTEXT] = g_param_spec_object ("context",
                                                  "Data context",
                                                  "Data context",
                                                  GCAL_TYPE_CONTEXT,
                                                  G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  /**
   * GcalManager:refreshing:
   *
   * Whether there are any sources refreshing or not.
   */
  properties[PROP_SYNCHRONIZING] = g_param_spec_boolean ("synchronizing",
                                                         "Synchronizing",
                                                         "Whether there are any sources synchronizing or not",
                                                         FALSE,
                                                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, NUM_PROPS, properties);

  /* signals */
  signals[CALENDAR_ADDED] = g_signal_new ("calendar-added",
                                          GCAL_TYPE_MANAGER,
                                          G_SIGNAL_RUN_LAST,
                                          0, NULL, NULL, NULL,
                                          G_TYPE_NONE,
                                          1,
                                          GCAL_TYPE_CALENDAR);

  signals[CALENDAR_CHANGED] = g_signal_new ("calendar-changed",
                                            GCAL_TYPE_MANAGER,
                                            G_SIGNAL_RUN_LAST,
                                            0, NULL, NULL, NULL,
                                            G_TYPE_NONE,
                                            1,
                                            GCAL_TYPE_CALENDAR);

  signals[CALENDAR_REMOVED] = g_signal_new ("calendar-removed",
                                            GCAL_TYPE_MANAGER,
                                            G_SIGNAL_RUN_LAST,
                                            0, NULL, NULL, NULL,
                                            G_TYPE_NONE,
                                            1,
                                            GCAL_TYPE_CALENDAR);

  /* Revolution #375: Titel und Einzelheiten eines Fehlers beim Verbinden oder Schreiben (Banner). */
  signals[ERROR] = g_signal_new ("error",
                                 GCAL_TYPE_MANAGER,
                                 G_SIGNAL_RUN_LAST,
                                 0, NULL, NULL, NULL,
                                 G_TYPE_NONE,
                                 2,
                                 G_TYPE_STRING,
                                 G_TYPE_STRING);
}

static void
gcal_manager_init (GcalManager *self)
{
  self->calendars_model = g_list_store_new (GCAL_TYPE_CALENDAR);
}

/* Public API */
/**
 * gcal_manager_new:
 * @context: a #GcalContext
 *
 * Creates a new #GcalManager.
 *
 * Returns: (transfer full): a newly created #GcalManager
 */
GcalManager*
gcal_manager_new (GcalContext *context)
{
  return g_object_new (GCAL_TYPE_MANAGER,
                       "context", context,
                       NULL);
}

/**
 * gcal_manager_get_source:
 * @self: a #GcalManager
 * @uid: the unique identifier of the source
 *
 * Retrieve a source according to it's UID. The source
 * is referenced for thread-safety and must be unreferenced
 * after user.
 *
 * Returns: (nullable)(transfer full): an #ESource, or %NULL.
 */
ESource*
gcal_manager_get_source (GcalManager *self,
                         const gchar *uid)
{
  g_return_val_if_fail (GCAL_IS_MANAGER (self), NULL);

  return e_source_registry_ref_source (self->source_registry, uid);
}

GList*
gcal_manager_get_calendars (GcalManager *self)
{
  g_return_val_if_fail (GCAL_IS_MANAGER (self), NULL);

  return g_hash_table_get_values (self->clients);
}

/**
 * gcal_manager_get_calendars_model:
 * @self: a #GcalManager
 *
 * Retrieves a model with all available #GcalCalendar. This is useful
 * for binding to combo rows.
 *
 * Returns: (transfer none): a #GListModel with all available #GcalCalendar
 */
GListModel*
gcal_manager_get_calendars_model (GcalManager *self)
{
  g_return_val_if_fail (GCAL_IS_MANAGER (self), NULL);

  return G_LIST_MODEL (self->calendars_model);
}

/**
 * gcal_manager_get_timeline:
 * @self: a #GcalManager
 *
 * Retrieves the default timeline of GNOME Calendar.
 *
 * Returns: (transfer none): a #GcalTimeline
 */
GcalTimeline*
gcal_manager_get_timeline (GcalManager *self)
{
  g_return_val_if_fail (GCAL_IS_MANAGER (self), NULL);

  return self->timeline;
}

/**
 * gcal_manager_get_default_calendar:
 * @self: a #GcalManager
 *
 * Returns: (transfer none): a #GcalCalendar.
 */
GcalCalendar*
gcal_manager_get_default_calendar (GcalManager *self)
{
  g_autoptr (ESource) default_source = NULL;
  GcalCalendar *calendar;
  guint n;

  g_return_val_if_fail (GCAL_IS_MANAGER (self), NULL);

  default_source = e_source_registry_ref_default_calendar (self->source_registry);
  calendar = default_source != NULL ? g_hash_table_lookup (self->clients, default_source) : NULL;

  /* Revolution #375: Standardkalender nur gelesen; ist er nicht schreibbar oder (noch) nicht verbunden, der erste
   * schreibbare der Liste (Evolution: e_cal_ops_new_component_editor nimmt ebenso den ersten schreibbaren). */
  n = g_list_model_get_n_items (G_LIST_MODEL (self->calendars_model));
  for (guint i = 0; (calendar == NULL || gcal_calendar_is_read_only (calendar)) && i < n; i++)
    {
      g_autoptr (GcalCalendar) candidate = g_list_model_get_item (G_LIST_MODEL (self->calendars_model), i);

      if (!gcal_calendar_is_read_only (candidate))
        calendar = candidate;
    }

  return calendar;
}

static void
on_event_created (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_autoptr (GcalManager) self = user_data;
  g_autofree gchar *new_uid = NULL;
  g_autoptr (GError) error = NULL;
  ESource *source = e_client_get_source (E_CLIENT (source_object));

  if (!e_cal_client_create_object_finish (E_CAL_CLIENT (source_object), result, &new_uid, &error))
    {
      g_log ("revolution", G_LOG_LEVEL_DEBUG, "Kalender: anlegen %s Fehler", e_source_get_uid (source));
      report_error (self, _("Could not create event"), error->message);
      return;
    }

  g_log ("revolution", G_LOG_LEVEL_DEBUG, "Kalender: anlegen %s fertig %s", e_source_get_uid (source), new_uid);
}

/**
 * gcal_manager_create_event:
 * @self: a #GcalManager
 * @event: a #GcalEvent
 *
 * Creates @event.
 */
void
gcal_manager_create_event (GcalManager *self,
                           GcalEvent   *event)
{
  ECalComponent *component;
  GcalCalendar *calendar;

  GCAL_ENTRY;

  g_return_if_fail (GCAL_IS_MANAGER (self));
  g_return_if_fail (GCAL_IS_EVENT (event));

  component = gcal_event_get_component (event);
  calendar = gcal_event_get_calendar (event);

  g_log ("revolution", G_LOG_LEVEL_DEBUG, "Kalender: anlegen %s „%s“", gcal_calendar_get_id (calendar),
         gcal_event_get_summary (event));
  e_cal_client_create_object (gcal_calendar_get_client (calendar),
                              e_cal_component_get_icalcomponent (component),
                              E_CAL_OPERATION_FLAG_NONE,
                              NULL,
                              on_event_created,
                              g_object_ref (self));

  GCAL_EXIT;
}

static void
on_event_updated (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_autoptr (GcalManager) self = user_data;
  g_autoptr (GError) error = NULL;
  ESource *source = e_client_get_source (E_CLIENT (source_object));

  if (!e_cal_client_modify_object_finish (E_CAL_CLIENT (source_object), result, &error))
    {
      g_log ("revolution", G_LOG_LEVEL_DEBUG, "Kalender: ändern %s Fehler", e_source_get_uid (source));
      report_error (self, _("Could not save event"), error->message);
      return;
    }

  g_log ("revolution", G_LOG_LEVEL_DEBUG, "Kalender: ändern %s fertig", e_source_get_uid (source));
}

/**
 * gcal_manager_update_event:
 * @self: a #GcalManager
 * @event: a #GcalEvent
 * @mod: an #GcalRecurrenceModType
 *
 * Saves all changes made to @event persistently.
 */
void
gcal_manager_update_event (GcalManager           *self,
                           GcalEvent             *event,
                           GcalRecurrenceModType  mod)
{
  ECalComponent *component;
  GcalCalendar *calendar;

  GCAL_ENTRY;

  g_return_if_fail (GCAL_IS_MANAGER (self));
  g_return_if_fail (GCAL_IS_EVENT (event));

  calendar = gcal_event_get_calendar (event);
  component = gcal_event_get_component (event);

  g_log ("revolution", G_LOG_LEVEL_DEBUG, "Kalender: ändern %s %s mod %d „%s“", gcal_calendar_get_id (calendar),
         e_cal_component_get_uid (component), mod, gcal_event_get_summary (event));
  e_cal_client_modify_object (gcal_calendar_get_client (calendar),
                              e_cal_component_get_icalcomponent (component),
                              (ECalObjModType) mod,
                              E_CAL_OPERATION_FLAG_NONE,
                              NULL,
                              on_event_updated,
                              g_object_ref (self));

  GCAL_EXIT;
}

static void
on_event_removed (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  GTask *task = user_data;
  g_autoptr (GError) error = NULL;
  GcalManager *self = g_task_get_source_object (task);
  ESource *source = e_client_get_source (E_CLIENT (source_object));

  if (!e_cal_client_remove_object_finish (E_CAL_CLIENT (source_object), result, &error))
    {
      g_log ("revolution", G_LOG_LEVEL_DEBUG, "Kalender: löschen %s Fehler", e_source_get_uid (source));
      report_error (self, _("Could not delete event"), error->message);
      g_task_return_error (task, g_steal_pointer (&error));
    }
  else
    {
      g_log ("revolution", G_LOG_LEVEL_DEBUG, "Kalender: löschen %s fertig", e_source_get_uid (source));
      g_task_return_boolean (task, TRUE);
    }
  g_object_unref (task);
}

/**
 * gcal_manager_remove_event:
 * @self: a #GcalManager
 * @event: a #GcalEvent
 * @mod: an #GcalRecurrenceModType
 * @callback: (nullable): Revolution #375: wird nach dem Schreiben gerufen (Beenden wartet darauf)
 * @user_data: Daten für @callback
 *
 * Deletes @event.
 */
void
gcal_manager_remove_event (GcalManager           *self,
                           GcalEvent             *event,
                           GcalRecurrenceModType  mod,
                           GAsyncReadyCallback    callback,
                           gpointer               user_data)
{
  ECalComponent *component;
  GcalCalendar *calendar;
  g_autofree gchar *rid = NULL;
  const gchar *uid;

  GCAL_ENTRY;

  g_return_if_fail (GCAL_IS_MANAGER (self));
  g_return_if_fail (GCAL_IS_EVENT (event));

  component = gcal_event_get_component (event);
  calendar = gcal_event_get_calendar (event);

  uid = e_cal_component_get_uid (component);

  if (gcal_event_has_recurrence (event))
    rid = e_cal_component_get_recurid_as_string (component);

  g_log ("revolution", G_LOG_LEVEL_DEBUG, "Kalender: löschen %s %s rid %s mod %d", gcal_calendar_get_id (calendar), uid,
         mod == GCAL_RECURRENCE_MOD_ALL || rid == NULL ? "-" : rid, mod);
  e_cal_client_remove_object (gcal_calendar_get_client (calendar),
                              uid,
                              mod == GCAL_RECURRENCE_MOD_ALL ? NULL : rid,
                              (ECalObjModType) mod,
                              E_CAL_OPERATION_FLAG_NONE,
                              NULL,
                              on_event_removed,
                              g_task_new (self, NULL, callback, user_data));

  GCAL_EXIT;
}

/* Revolution #375: Verschieben in einen anderen Kalender im Worker (Vorlage: create/remove_object_sync im
 * UI-Thread, gcal-manager.c:1194, 1218); Reihenfolge wie Vorlage: erst anlegen, dann im alten löschen. */
typedef struct
{
  ECalClient      *from;
  ECalClient      *to;
  ECalComponent   *component;
} MoveData;

static void
move_data_free (MoveData *data)
{
  g_clear_object (&data->from);
  g_clear_object (&data->to);
  g_clear_object (&data->component);
  g_free (data);
}

static void
move_event_thread (GTask        *task,
                   gpointer      source_object,
                   gpointer      task_data,
                   GCancellable *cancellable)
{
  MoveData *data = task_data;
  g_autoptr (ECalComponent) clone = e_cal_component_clone (data->component);
  ECalComponentId *id = e_cal_component_get_id (data->component);
  g_autofree gchar *new_uid = NULL;
  GError *error = NULL;

  if (e_cal_client_create_object_sync (data->to, e_cal_component_get_icalcomponent (clone), E_CAL_OPERATION_FLAG_NONE,
                                       &new_uid, cancellable, &error) &&
      e_cal_client_remove_object_sync (data->from, e_cal_component_id_get_uid (id), e_cal_component_id_get_rid (id),
                                       E_CAL_OBJ_MOD_THIS, E_CAL_OPERATION_FLAG_NONE, cancellable, &error))
    g_task_return_pointer (task, g_steal_pointer (&new_uid), g_free);
  else
    g_task_return_error (task, error);
  e_cal_component_id_free (id);
}

static void
on_event_moved (GObject      *source_object,
                GAsyncResult *result,
                gpointer      user_data)
{
  GcalManager *self = GCAL_MANAGER (source_object);
  g_autoptr (GError) error = NULL;
  g_autofree gchar *new_uid = g_task_propagate_pointer (G_TASK (result), &error);

  if (new_uid == NULL)
    {
      g_log ("revolution", G_LOG_LEVEL_DEBUG, "Kalender: verschieben Fehler");
      report_error (self, _("Could not move event"), error != NULL ? error->message : "");
      return;
    }
  g_log ("revolution", G_LOG_LEVEL_DEBUG, "Kalender: verschieben fertig %s", new_uid);
}

/**
 * gcal_manager_move_event_to_source:
 * @self: a #GcalManager
 * @event: a #GcalEvent
 * @dest: the destination calendar
 *
 * Moves @event to @dest calendar. This is a fail-safe operation:
 * worst case, the user will have two duplicated events, and we
 * guarantee to never loose any data.
 */
void
gcal_manager_move_event_to_source (GcalManager *self,
                                   GcalEvent   *event,
                                   ESource     *dest)
{
  g_autoptr (GTask) task = NULL;
  GcalCalendar *calendar;
  MoveData *data;

  GCAL_ENTRY;

  g_return_if_fail (GCAL_IS_MANAGER (self));
  g_return_if_fail (GCAL_IS_EVENT (event));
  g_return_if_fail (E_IS_SOURCE (dest));

  calendar = g_hash_table_lookup (self->clients, dest);
  g_return_if_fail (calendar != NULL);

  data = g_new0 (MoveData, 1);
  data->to = g_object_ref (gcal_calendar_get_client (calendar));
  data->from = g_object_ref (gcal_calendar_get_client (gcal_event_get_calendar (event)));
  data->component = g_object_ref (gcal_event_get_component (event));
  g_log ("revolution", G_LOG_LEVEL_DEBUG, "Kalender: verschieben %s %s nach %s",
         gcal_calendar_get_id (gcal_event_get_calendar (event)), gcal_event_get_uid (event), e_source_get_uid (dest));
  task = g_task_new (self, NULL, on_event_moved, NULL);
  g_task_set_task_data (task, data, (GDestroyNotify) move_data_free);
  g_task_run_in_thread (task, move_event_thread);

  GCAL_EXIT;
}

/**
 * gcal_manager_get_events:
 * @self: a #GcalManager
 * @start_date: the start of the dete range
 * @end_date: the end of the dete range
 *
 * Returns a list with #GcalEvent objects owned by the caller,
 * the list and the objects. The components inside the list are
 * owned by the caller as well.
 *
 * Returns: (nullable)(transfer full)(content-type GcalEvent):a #GList
 */
GPtrArray*
gcal_manager_get_events (GcalManager *self,
                         GDateTime   *start_date,
                         GDateTime   *end_date)
{
  g_autoptr (GPtrArray) events_at_range = NULL;

  GCAL_ENTRY;

  g_return_val_if_fail (GCAL_IS_MANAGER (self), NULL);

  events_at_range = gcal_timeline_get_events_at_range (self->timeline, start_date, end_date);

  GCAL_RETURN (g_steal_pointer (&events_at_range));
}

/**
 * gcal_manager_get_synchronizing:
 * @self: a #GcalManager
 *
 * Retrieves whether @self is refreshing the calendars or not.
 *
 * Returns: %TRUE if any calendars are being synchronizing.
 */
gboolean
gcal_manager_get_synchronizing (GcalManager *self)
{
  g_return_val_if_fail (GCAL_IS_MANAGER (self), FALSE);

  return self->clients_synchronizing != 0;
}

void
gcal_manager_startup (GcalManager     *self,
                      ESourceRegistry *registry)
{
  GList *sources, *l;

  GCAL_ENTRY;

  self->timeline = gcal_timeline_new (self->context);
  self->clients = g_hash_table_new_full ((GHashFunc) e_source_hash,
                                         (GEqualFunc) e_source_equal,
                                         g_object_unref,
                                         g_object_unref);

  /* Revolution #373: die Registry der Anwendung (main.c, im Worker geladen) statt
   * e_source_registry_new_sync im UI-Thread; ohne Credential-Prompter (Anmeldung über GOA). */
  self->source_registry = g_object_ref (registry);

  g_signal_connect_object (self->source_registry, "source-added", G_CALLBACK (load_source), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->source_registry, "source-removed", G_CALLBACK (remove_source), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->source_registry, "source-changed", G_CALLBACK (source_changed), self, G_CONNECT_SWAPPED);

  sources = e_source_registry_list_enabled (self->source_registry, E_SOURCE_EXTENSION_CALENDAR);

  for (l = sources; l != NULL; l = l->next)
    load_source (self, l->data);

  g_list_free_full (sources, g_object_unref);

  GCAL_EXIT;
}
