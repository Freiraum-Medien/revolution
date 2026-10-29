/* gcal-context.c
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

#define G_LOG_DOMAIN "GcalContext"

#include "gcal-context.h"
#include "gcal-time-zone-monitor.h"

struct _GcalContext
{
  GObject             parent;

  GcalClock          *clock;
  GcalManager        *manager;
  GSettings          *settings;
  GcalTimeFormat      time_format;

  GcalTimeZoneMonitor   *timezone_monitor;
};

G_DEFINE_TYPE (GcalContext, gcal_context, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_CLOCK,
  PROP_MANAGER,
  PROP_SETTINGS,
  PROP_TIME_FORMAT,
  PROP_TIMEZONE,
  N_PROPS
};

static GParamSpec *properties [N_PROPS];


/*
 * Callbacks
 */

static void
on_timezone_changed_cb (GcalTimeZoneMonitor *timezone_monitor,
                        GParamSpec          *pspec,
                        GcalContext         *self)
{
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TIMEZONE]);
}


/*
 * GObject overrides
 */

static void
gcal_context_constructed (GObject *object)
{
  GcalContext *self = (GcalContext *)object;

  G_OBJECT_CLASS (gcal_context_parent_class)->constructed (object);

  self->manager = gcal_manager_new (self);
}

static void
gcal_context_finalize (GObject *object)
{
  GcalContext *self = (GcalContext *)object;

  g_clear_object (&self->clock);
  g_clear_object (&self->settings);
  g_clear_object (&self->manager);
  g_clear_object (&self->timezone_monitor);

  G_OBJECT_CLASS (gcal_context_parent_class)->finalize (object);
}

static void
gcal_context_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  GcalContext *self = GCAL_CONTEXT (object);

  switch (prop_id)
    {
    case PROP_CLOCK:
      g_value_set_object (value, self->clock);
      break;

    case PROP_MANAGER:
      g_value_set_object (value, self->manager);
      break;

    case PROP_SETTINGS:
      g_value_set_object (value, self->settings);
      break;

    case PROP_TIME_FORMAT:
      g_value_set_enum (value, self->time_format);
      break;

    case PROP_TIMEZONE:
      g_value_set_boxed (value, gcal_time_zone_monitor_get_timezone (self->timezone_monitor));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gcal_context_set_property (GObject      *object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  switch (prop_id)
    {
    case PROP_CLOCK:
    case PROP_MANAGER:
    case PROP_SETTINGS:
    case PROP_TIME_FORMAT:
    case PROP_TIMEZONE:
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gcal_context_class_init (GcalContextClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = gcal_context_constructed;
  object_class->finalize = gcal_context_finalize;
  object_class->get_property = gcal_context_get_property;
  object_class->set_property = gcal_context_set_property;

  properties[PROP_CLOCK] = g_param_spec_object ("clock",
                                                "Wall clock",
                                                "Main clock driving the calendar timeline",
                                                GCAL_TYPE_CLOCK,
                                                G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_MANAGER] = g_param_spec_object ("manager",
                                                  "Data manager",
                                                  "Data manager of the application",
                                                  GCAL_TYPE_MANAGER,
                                                  G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_SETTINGS] = g_param_spec_object ("settings",
                                                   "Revolution settings",
                                                   "Revolution settings",
                                                   G_TYPE_SETTINGS,
                                                   G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_TIME_FORMAT] = g_param_spec_enum ("time-format",
                                                    "Time format",
                                                    "System time format",
                                                    GCAL_TYPE_TIME_FORMAT,
                                                    GCAL_TIME_FORMAT_24H,
                                                    G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_TIMEZONE] = g_param_spec_boxed ("timezone",
                                                  "Timezone",
                                                  "System timezone",
                                                  G_TYPE_TIME_ZONE,
                                                  G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
gcal_context_init (GcalContext *self)
{
  self->clock = gcal_clock_new ();
  /* Revolution #373: eigenes Schema statt org.gnome.calendar (Sichtbarkeit der Kalender,
   * gcal-manager.c). Zeitformat fest 24 Stunden wie die Mail-Liste (mail-message.c „%H:%M“),
   * ohne Abfrage des Settings-Portals. */
  self->settings = g_settings_new ("de.freiraum_medien.Revolution");
  self->time_format = GCAL_TIME_FORMAT_24H;

  self->timezone_monitor = gcal_time_zone_monitor_new ();
  g_signal_connect_object (self->timezone_monitor,
                           "notify::timezone",
                           G_CALLBACK (on_timezone_changed_cb),
                           self,
                           0);

}

/**
 * gcal_context_new: (skip)
 *
 * Create a new GcalContext. Only one context must be
 * available during the application lifetime.
 *
 * Returns: (transfer full): a #GcalContext
 */
GcalContext*
gcal_context_new (void)
{
  return g_object_new (GCAL_TYPE_CONTEXT, NULL);
}

/**
 * gcal_context_get_clock:
 *
 * Retrieves the #GcalClock from @self.
 *
 * Returns: (transfer none): a #GcalClock
 */
GcalClock*
gcal_context_get_clock (GcalContext *self)
{
  g_return_val_if_fail (GCAL_IS_CONTEXT (self), NULL);

  return self->clock;
}

/**
 * gcal_context_get_manager:
 *
 * Retrieves the #GcalManager from @self.
 *
 * Returns: (transfer none): a #GcalManager
 */
GcalManager*
gcal_context_get_manager (GcalContext *self)
{
  g_return_val_if_fail (GCAL_IS_CONTEXT (self), NULL);

  return self->manager;
}

/**
 * gcal_context_get_settings:
 *
 * Retrieves the #GSettings from @self.
 *
 * Returns: (transfer none): a #GSettings
 */
GSettings*
gcal_context_get_settings (GcalContext *self)
{
  g_return_val_if_fail (GCAL_IS_CONTEXT (self), NULL);

  return self->settings;
}

/**
 * gcal_context_get_time_format:
 *
 * Retrieves the #GcalTimeFormat from @self.
 *
 * Returns: a #GcalTimeFormat
 */
GcalTimeFormat
gcal_context_get_time_format (GcalContext *self)
{
  g_return_val_if_fail (GCAL_IS_CONTEXT (self), 0);

  return self->time_format;
}

/**
 * gcal_context_get_timezone:
 *
 * Retrieves the system timezone.
 *
 * Returns: (transfer none): a #GTimeZone
 */
GTimeZone*
gcal_context_get_timezone (GcalContext *self)
{
  g_return_val_if_fail (GCAL_IS_CONTEXT (self), NULL);

  return gcal_time_zone_monitor_get_timezone (self->timezone_monitor);
}

void
gcal_context_startup (GcalContext     *self,
                      ESourceRegistry *registry)
{
  g_return_if_fail (GCAL_IS_CONTEXT (self));

  gcal_manager_startup (self->manager, registry);
}
