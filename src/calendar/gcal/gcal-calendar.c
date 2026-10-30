/* Übernommen aus gnome-calendar 50.0 src/core/gcal-calendar.c:1-801 (ff4d7f7d, GPL-3.0-or-later). Revolution
 * #373: ohne e_source_write (save_calendar, set_color, set_name), Sichtbarkeit nur im Speicher (Einstellungen
 * der App in gcal-manager.c), Farbe nur gelesen. #375: Signal „error“ (Fehler des ECalClientView aus
 * gcal-calendar-monitor.c, im UI-Thread) für das Banner. */
/* gcal-calendar.c
 *
 * Copyright 2019 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "GcalCalendar"

#include "gcal-calendar.h"
#include "gcal-core-macros.h"
#include "gcal-debug.h"
#include "gcal-utils.h"

typedef struct
{
  GdkRGBA             color;
  ESource            *source;
  ESource            *parent_source;
  ECalClient         *client;

  gboolean            read_only;
  gboolean            visible;

  gulong              color_changed_handler_id;
  gulong              name_changed_handler_id;
  gulong              readonly_changed_handler_id;
  gboolean            initialized;

  struct {
    GMutex            mutex;
    guint             update_idle_id;
    gboolean          color_changed;
    gboolean          name_changed;
    gboolean          readonly_changed;
  } shared;
} GcalCalendarPrivate;

static void          g_initable_iface_init                       (GInitableIface     *iface);

static gboolean      update_idle_cb                              (gpointer            data);

G_DEFINE_TYPE_WITH_CODE (GcalCalendar, gcal_calendar, G_TYPE_OBJECT,
                         G_ADD_PRIVATE (GcalCalendar)
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, g_initable_iface_init)
                         G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE, NULL))

G_DEFINE_QUARK (GcalCalendar, gcal_calendar_error);

enum
{
  PROP_0,
  PROP_CLIENT,
  PROP_COLOR,
  PROP_ID,
  PROP_NAME,
  PROP_PARENT_SOURCE,
  PROP_READ_ONLY,
  PROP_SOURCE,
  PROP_VISIBLE,
  N_PROPS
};

static GParamSpec *properties [N_PROPS];

/* Revolution #375 */
enum { ERROR, N_SIGNALS };
static guint signals [N_SIGNALS];


/*
 * Auxiliary methods
 */

static void
schedule_idle_update_unlocked (GcalCalendar *self)
{
  GcalCalendarPrivate *priv = gcal_calendar_get_instance_private (self);

  if (priv->shared.update_idle_id == 0)
    priv->shared.update_idle_id = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
                                                   update_idle_cb,
                                                   g_object_ref (self),
                                                   g_object_unref);
}

static void
update_color (GcalCalendar *self)
{
  GcalCalendarPrivate *priv = gcal_calendar_get_instance_private (self);
  ESourceSelectable *selectable_extension;
  const gchar *color;

  selectable_extension = e_source_get_extension (priv->source, E_SOURCE_EXTENSION_CALENDAR);
  color = e_source_selectable_get_color (selectable_extension);

  if (!color || !gdk_rgba_parse (&priv->color, color))
    gdk_rgba_parse (&priv->color, "#ffffff");
}


/*
 * Callbacks
 */

static gboolean
update_idle_cb (gpointer data)
{
  GcalCalendarPrivate *priv;
  GcalCalendar *self;

  GCAL_ENTRY;

  g_assert (GCAL_IS_MAIN_THREAD ());

  self = GCAL_CALENDAR (data);
  priv = gcal_calendar_get_instance_private (self);

  G_MUTEX_AUTO_LOCK (&priv->shared.mutex, locker);

  if (priv->shared.name_changed)
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_NAME]);
      priv->shared.name_changed = FALSE;
    }

  if (priv->shared.color_changed)
    {
      update_color (self);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_COLOR]);
      priv->shared.color_changed = FALSE;
    }

  if (priv->shared.readonly_changed)
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_READ_ONLY]);
      priv->shared.readonly_changed = FALSE;
    }

  priv->shared.update_idle_id = 0;

  GCAL_RETURN (G_SOURCE_REMOVE);
}

static void
on_source_color_changed_cb (ESourceSelectable *source,
                            GParamSpec        *pspec,
                            GcalCalendar      *self)
{
  GcalCalendarPrivate *priv;

  priv = gcal_calendar_get_instance_private (self);
  G_MUTEX_AUTO_LOCK (&priv->shared.mutex, locker);

  priv->shared.color_changed = TRUE;
  schedule_idle_update_unlocked (self);
}

static void
on_source_name_changed_cb (ESource      *source,
                           GParamSpec   *pspec,
                           GcalCalendar *self)
{
  GcalCalendarPrivate *priv;

  priv = gcal_calendar_get_instance_private (self);
  G_MUTEX_AUTO_LOCK (&priv->shared.mutex, locker);

  priv->shared.name_changed = TRUE;
  schedule_idle_update_unlocked (self);
}

static void
on_client_readonly_changed_cb (EClient      *client,
                               GParamSpec   *pspec,
                               GcalCalendar *self)
{
  GcalCalendarPrivate *priv;

  priv = gcal_calendar_get_instance_private (self);
  G_MUTEX_AUTO_LOCK (&priv->shared.mutex, locker);

  priv->shared.readonly_changed = TRUE;
  schedule_idle_update_unlocked (self);
}


/*
 * GInitable iface
 */

static gboolean
gcal_calendar_initable_init (GInitable     *initable,
                             GCancellable  *cancellable,
                             GError       **error)
{
  GcalCalendarPrivate *priv;
  g_autoptr (EClient) client = NULL;
  g_autoptr (GError) local_error = NULL;
  ESourceSelectable *selectable_extension;
  GcalCalendar *self;

  GCAL_ENTRY;

  self = GCAL_CALENDAR (initable);
  priv = gcal_calendar_get_instance_private (self);

  g_assert (!priv->initialized);
  priv->initialized = TRUE;

  if (!e_source_has_extension (priv->source, E_SOURCE_EXTENSION_CALENDAR))
    {
      g_set_error (error,
                   GCAL_CALENDAR_ERROR,
                   GCAL_CALENDAR_ERROR_NOT_CALENDAR,
                   "ESource is not a calendar");
      GCAL_RETURN (FALSE);
    }

  /* Revolution #373: sichtbar bis der Manager die Einstellung der App anwendet
   * (gcal-manager.c on_calendar_created_cb); „selected“ der ESource wird nicht gelesen. */
  selectable_extension = e_source_get_extension (priv->source, E_SOURCE_EXTENSION_CALENDAR);
  priv->visible = TRUE;

  update_color (self);
  priv->color_changed_handler_id = g_signal_connect (selectable_extension,
                                                     "notify::color",
                                                     G_CALLBACK (on_source_color_changed_cb),
                                                     self);

  priv->name_changed_handler_id = g_signal_connect (priv->source,
                                                    "notify::display-name",
                                                    G_CALLBACK (on_source_name_changed_cb),
                                                    self);

  client = e_cal_client_connect_sync (priv->source,
                                      E_CAL_CLIENT_SOURCE_TYPE_EVENTS,
                                      (guint32) -1,
                                      cancellable,
                                      &local_error);

  if (local_error)
    {
      g_propagate_error (error, g_steal_pointer (&local_error));
      GCAL_RETURN (FALSE);
    }

  priv->readonly_changed_handler_id = g_signal_connect (client,
                                                        "notify::readonly",
                                                        G_CALLBACK (on_client_readonly_changed_cb),
                                                        self);

  g_assert (E_IS_CAL_CLIENT (client));
  priv->client = (ECalClient*) g_steal_pointer (&client);

  GCAL_RETURN (TRUE);
}

static void
g_initable_iface_init (GInitableIface *iface)
{
  iface->init = gcal_calendar_initable_init;
}


/*
 * GObject overrides
 */

static void
gcal_calendar_finalize (GObject *object)
{
  GcalCalendar *self = (GcalCalendar *)object;
  GcalCalendarPrivate *priv = gcal_calendar_get_instance_private (self);
  ESourceSelectable *selectable_extension;

  selectable_extension = e_source_get_extension (priv->source, E_SOURCE_EXTENSION_CALENDAR);

  gcal_clear_signal_handler (&priv->color_changed_handler_id, selectable_extension);
  gcal_clear_signal_handler (&priv->name_changed_handler_id, priv->source);
  gcal_clear_signal_handler (&priv->readonly_changed_handler_id, priv->client);

  g_mutex_lock (&priv->shared.mutex);
  g_clear_handle_id (&priv->shared.update_idle_id, g_source_remove);
  g_mutex_unlock (&priv->shared.mutex);

  g_clear_object (&priv->client);
  g_clear_object (&priv->source);
  g_clear_object (&priv->parent_source);

  g_mutex_clear (&priv->shared.mutex);

  G_OBJECT_CLASS (gcal_calendar_parent_class)->finalize (object);
}

static void
gcal_calendar_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  GcalCalendar *self = GCAL_CALENDAR (object);
  GcalCalendarPrivate *priv = gcal_calendar_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_CLIENT:
      g_value_set_object (value, priv->client);
      break;

    case PROP_COLOR:
      g_value_set_boxed (value, &priv->color);
      break;

    case PROP_ID:
      g_value_set_string (value, gcal_calendar_get_id (self));
      break;

    case PROP_NAME:
      g_value_set_string (value, e_source_get_display_name (priv->source));
      break;

    case PROP_PARENT_SOURCE:
      g_value_set_object (value, priv->parent_source);
      break;

    case PROP_READ_ONLY:
      g_value_set_boolean (value, gcal_calendar_is_read_only (self));
      break;

    case PROP_SOURCE:
      g_value_set_object (value, priv->source);
      break;

    case PROP_VISIBLE:
      g_value_set_boolean (value, gcal_calendar_get_visible (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gcal_calendar_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  GcalCalendar *self = GCAL_CALENDAR (object);
  GcalCalendarPrivate *priv = gcal_calendar_get_instance_private (self);

  switch (prop_id)
    {
    case PROP_CLIENT:
      g_assert_not_reached ();
      break;

    case PROP_COLOR:
      g_assert_not_reached ();
      break;

    case PROP_ID:
      g_assert_not_reached ();
      break;

    case PROP_NAME:
      g_assert_not_reached ();
      break;

    case PROP_PARENT_SOURCE:
      g_assert (priv->parent_source == NULL);
      priv->parent_source = g_value_dup_object (value);
      break;

    case PROP_READ_ONLY:
      g_assert_not_reached ();
      break;

    case PROP_SOURCE:
      g_assert (priv->source == NULL);
      priv->source = g_value_dup_object (value);
      break;

    case PROP_VISIBLE:
      gcal_calendar_set_visible (self, g_value_get_boolean (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gcal_calendar_class_init (GcalCalendarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gcal_calendar_finalize;
  object_class->get_property = gcal_calendar_get_property;
  object_class->set_property = gcal_calendar_set_property;

  properties[PROP_CLIENT] = g_param_spec_object ("client",
                                                 "Calendar client",
                                                 "Calendar client",
                                                 E_TYPE_CAL_CLIENT,
                                                 G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_COLOR] = g_param_spec_boxed ("color",
                                               "Color",
                                               "Color of the calendar",
                                               GDK_TYPE_RGBA,
                                               G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_ID] = g_param_spec_string ("id",
                                             "Identifier",
                                             "Unique identifier of the calendar",
                                             NULL,
                                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_NAME] = g_param_spec_string ("name",
                                               "Name",
                                               "Name of the calendar",
                                               NULL,
                                               G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_PARENT_SOURCE] = g_param_spec_object ("parent-source", NULL, NULL,
                                                        E_TYPE_SOURCE,
                                                        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_READ_ONLY] = g_param_spec_boolean ("read-only",
                                                     "Read-only",
                                                     "Whether the calendar is read-only or not",
                                                     FALSE,
                                                     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_SOURCE] = g_param_spec_object ("source",
                                                 "Source",
                                                 "Source",
                                                 E_TYPE_SOURCE,
                                                 G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  properties[PROP_VISIBLE] = g_param_spec_boolean ("visible",
                                                   "Visible",
                                                   "Whether the calendar is visible",
                                                   FALSE,
                                                   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  /* Revolution #375: Fehler der Termin-Ansicht (gcal-calendar-monitor.c), im UI-Thread ausgelöst. */
  signals[ERROR] = g_signal_new ("error", GCAL_TYPE_CALENDAR, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                 G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gcal_calendar_init (GcalCalendar *self)
{
  GcalCalendarPrivate *priv = gcal_calendar_get_instance_private (self);

  gdk_rgba_parse (&priv->color, "#ffffff");

  g_mutex_init (&priv->shared.mutex);
}

/**
 * gcal_calendar_new:
 * @source: an #ESource
 * @cancellable: (nullable): a cancellable
 * @callback:
 * @user_data:
 *
 * Asynchronously creates and initializes a #GcalCalendar
 * from @source.
 */
void
gcal_calendar_new (ESource             *source,
                   ESource             *parent_source,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
  return g_async_initable_new_async (GCAL_TYPE_CALENDAR,
                                     G_PRIORITY_DEFAULT,
                                     cancellable,
                                     callback,
                                     user_data,
                                     "source", source,
                                     "parent-source", parent_source,
                                     NULL);
}

/**
 * @result:
 * @error: (nullable): return location for a #GError
 *
 * Finishes the operation started by gcal_calendar_new().
 *
 * Returns: (transfer full)(nullable): a #GcalCalendar
 */
GcalCalendar*
gcal_calendar_new_finish (GAsyncResult  *result,
                          GError       **error)
{
  g_autoptr (GObject) source_object = NULL;
  g_autoptr (GObject) result_object = NULL;

  g_return_val_if_fail (G_IS_TASK (result), NULL);
  g_return_val_if_fail (!error || !*error, NULL);

  source_object = g_async_result_get_source_object (result);
  g_assert (source_object != NULL);

  result_object = g_async_initable_new_finish (G_ASYNC_INITABLE (source_object),
                                               result,
                                               error);

  return (GcalCalendar*) g_steal_pointer (&result_object);
}

/**
 * gcal_calendar_get_color:
 * @self: a #GcalCalendar
 *
 * Retrieves the color of @self.
 *
 * Returns: (transfer none): a #GdkRGBA
 */
const GdkRGBA*
gcal_calendar_get_color (GcalCalendar *self)
{
  GcalCalendarPrivate *priv = gcal_calendar_get_instance_private (self);

  g_return_val_if_fail (GCAL_IS_CALENDAR (self), NULL);

  return &priv->color;
}

/**
 * gcal_calendar_get_id:
 * @self: a #GcalCalendar
 *
 * Retrieves the unique identifier of @self.
 *
 * Returns: (transfer none): a string
 */
const gchar*
gcal_calendar_get_id (GcalCalendar *self)
{
  GcalCalendarPrivate *priv = gcal_calendar_get_instance_private (self);

  g_return_val_if_fail (GCAL_IS_CALENDAR (self), NULL);

  return e_source_get_uid (priv->source);
}

/**
 * gcal_calendar_get_name:
 * @self: a #GcalCalendar
 *
 * Retrieves the user-visible name of @self.
 *
 * Returns: (transfer none)(nullable): a string
 */
const gchar*
gcal_calendar_get_name (GcalCalendar *self)
{
  GcalCalendarPrivate *priv = gcal_calendar_get_instance_private (self);

  g_return_val_if_fail (GCAL_IS_CALENDAR (self), NULL);

  return e_source_get_display_name (priv->source);
}

/**
 * gcal_calendar_get_parent_source:
 * @self: a #GcalCalendar
 *
 * Retrieves the parent #ESource of @self.
 *
 * Returns: (transfer none): an #ESource
 */
ESource*
gcal_calendar_get_parent_source (GcalCalendar *self)
{
  GcalCalendarPrivate *priv = gcal_calendar_get_instance_private (self);

  g_return_val_if_fail (GCAL_IS_CALENDAR (self), NULL);

  return priv->parent_source;
}

/**
 * gcal_calendar_is_read_only:
 * @self: a #GcalCalendar
 *
 * Retrieves whether @self is read-only or not.
 *
 * Returns: %TRUE if @self is read-only, %FALSE otherwise
 */
gboolean
gcal_calendar_is_read_only (GcalCalendar *self)
{
  GcalCalendarPrivate *priv = gcal_calendar_get_instance_private (self);

  g_return_val_if_fail (GCAL_IS_CALENDAR (self), FALSE);

  return e_client_is_readonly (E_CLIENT (priv->client));
}

/**
 * gcal_calendar_get_source:
 * @self: a #GcalCalendar
 *
 * Retrieves the #ESource that is backing @self.
 *
 * Returns: (transfer none): an #ESource
 */
ESource*
gcal_calendar_get_source (GcalCalendar *self)
{
  GcalCalendarPrivate *priv = gcal_calendar_get_instance_private (self);

  g_return_val_if_fail (GCAL_IS_CALENDAR (self), NULL);

  return priv->source;
}

/**
 * gcal_calendar_get_client:
 * @self: a #GcalCalendar
 *
 * Retrieves the #ECalClient that is backing @self.
 *
 * Returns: (transfer none): an #ECalClient
 */
ECalClient*
gcal_calendar_get_client (GcalCalendar *self)
{
  GcalCalendarPrivate *priv = gcal_calendar_get_instance_private (self);

  g_return_val_if_fail (GCAL_IS_CALENDAR (self), NULL);

  return priv->client;
}

/**
 * gcal_calendar_get_visible:
 * @self: a #GcalCalendar
 *
 * Retrieves whether @self is visible to the user or not.
 *
 * Returns: %TRUE if @self is visible, %FALSE otherwise
 */
gboolean
gcal_calendar_get_visible (GcalCalendar *self)
{
  GcalCalendarPrivate *priv = gcal_calendar_get_instance_private (self);

  g_return_val_if_fail (GCAL_IS_CALENDAR (self), FALSE);

  return priv->visible;
}

/**
 * gcal_calendar_set_visible:
 * @self: a #GcalCalendar
 * @visible: visibility of the calendar
 *
 * Sets whether @self is visible or not.
 */
void
gcal_calendar_set_visible (GcalCalendar *self,
                           gboolean      visible)
{
  GcalCalendarPrivate *priv = gcal_calendar_get_instance_private (self);

  g_return_if_fail (GCAL_IS_CALENDAR (self));

  if (priv->visible == visible)
    return;

  /* Revolution #373: nur im Speicher, nie e_source_write (Vorlage gcal-calendar.c:785-798). */
  priv->visible = visible;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_VISIBLE]);
}

