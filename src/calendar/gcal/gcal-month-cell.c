/* gcal-month-cell.c
 *
 * Copyright (C) 2017 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
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
 */


#define G_LOG_DOMAIN "GcalMonthCell"

#include "config.h"
#include "gcal-context.h"
#include "gcal-clock.h"
#include "gcal-debug.h"
#include "gcal-event-widget.h"
#include "gcal-month-cell.h"
#include "gcal-utils.h"

struct _GcalMonthCell
{
  GtkWidget           parent;

  GDateTime          *date;
  guint               n_overflow;

  GtkLabel           *day_label;
  GtkWidget          *header_box;
  GtkWidget          *breakpoint_bin;

  GtkWidget          *overflow_button;
  GtkInscription     *overflow_inscription;
  GtkWidget          *overlay;

  gboolean            different_month;
  gboolean            selected;

  GcalContext        *context;

};

G_DEFINE_TYPE (GcalMonthCell, gcal_month_cell, GTK_TYPE_WIDGET)

enum
{
  SHOW_OVERFLOW,
  ACTIVATE,
  N_SIGNALS
};

enum
{
  PROP_0,
  PROP_CONTEXT,
  N_PROPS
};

static guint signals[N_SIGNALS] = { 0, };
static GParamSpec *properties[N_PROPS] = { 0, };


/*
 * Auxiliary methods
 */

static void
update_style_flags (GcalMonthCell *self)
{
  g_autoptr (GDateTime) today = NULL;
  gint weekday;

  /* Today */
  today = g_date_time_new_now_local ();

  if (gcal_date_time_compare_date (self->date, today) == 0)
    gtk_widget_add_css_class (GTK_WIDGET (self), "today");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "today");

  weekday = g_date_time_get_day_of_week (self->date);
  if (is_workday (weekday))
    gtk_widget_add_css_class (GTK_WIDGET (self), "workday");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "workday");
}

static void
add_month_separators (GcalMonthCell *self)
{
  gint day_of_month;

  gtk_widget_remove_css_class (GTK_WIDGET (self), "separator-top");
  gtk_widget_remove_css_class (GTK_WIDGET (self), "separator-side");

  day_of_month = g_date_time_get_day_of_month (self->date);
  if (day_of_month > 1 && day_of_month <= N_WEEKDAYS)
    {
      gtk_widget_add_css_class (GTK_WIDGET (self), "separator-top");
    }
  else if (day_of_month == 1)
    {
      gtk_widget_add_css_class (GTK_WIDGET (self), "separator-side");
      gtk_widget_add_css_class (GTK_WIDGET (self), "separator-top");
    }
}


/*
 * Callbacks
 */

static void
day_changed_cb (GcalClock     *clock,
                GcalMonthCell *self)
{
  update_style_flags (self);
}

static void
overflow_button_clicked_cb (GtkWidget     *button,
                            GcalMonthCell *self)
{
  g_signal_emit (self, signals[SHOW_OVERFLOW], 0, button);
}


/*
 * GtkWidget overrides
 */

static gboolean
gcal_month_cell_focus (GtkWidget        *widget,
                       GtkDirectionType  direction)
{
  GtkRoot *root = gtk_widget_get_root (widget);

  if (gtk_root_get_focus (root) == NULL)
    return FALSE;

  return GTK_WIDGET_CLASS (gcal_month_cell_parent_class)->focus (widget, direction);
}


/*
 * GObject overrides
 */

static void
gcal_month_cell_dispose (GObject *object)
{
  GcalMonthCell *self = (GcalMonthCell *)object;

  gcal_clear_date_time (&self->date);
  g_clear_object (&self->context);
  g_clear_pointer (&self->breakpoint_bin, gtk_widget_unparent);

  G_OBJECT_CLASS (gcal_month_cell_parent_class)->dispose (object);
}


static void
gcal_month_cell_set_property (GObject       *object,
                              guint          property_id,
                              const GValue  *value,
                              GParamSpec    *pspec)
{
  GcalMonthCell *self = (GcalMonthCell *) object;

  switch (property_id)
    {
    case PROP_CONTEXT:
      gcal_month_cell_set_context (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
gcal_month_cell_get_property (GObject       *object,
                              guint          property_id,
                              GValue        *value,
                              GParamSpec    *pspec)
{
  GcalMonthCell *self = (GcalMonthCell *) object;

  switch (property_id)
    {
    case PROP_CONTEXT:
      g_value_set_object (value, self->context);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
gcal_month_cell_class_init (GcalMonthCellClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = gcal_month_cell_dispose;
  object_class->set_property = gcal_month_cell_set_property;
  object_class->get_property = gcal_month_cell_get_property;

  widget_class->focus = gcal_month_cell_focus;

  signals[SHOW_OVERFLOW] = g_signal_new ("show-overflow",
                                         GCAL_TYPE_MONTH_CELL,
                                         G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL,
                                         G_TYPE_NONE,
                                         1,
                                         GTK_TYPE_WIDGET);

  signals[ACTIVATE] = g_signal_new ("activate",
                                    GCAL_TYPE_MONTH_CELL,
                                    G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION,
                                    0,
                                    NULL, NULL,
                                    g_cclosure_marshal_VOID__VOID,
                                    G_TYPE_NONE,
                                    0);

  gtk_widget_class_set_activate_signal (widget_class, signals[ACTIVATE]);

  {
    g_autoptr (GtkShortcutAction) activate_action = NULL;
    const guint activate_keyvals[] = {
      GDK_KEY_space,
      GDK_KEY_KP_Space,
      GDK_KEY_Return,
      GDK_KEY_ISO_Enter,
      GDK_KEY_KP_Enter,
    };

    activate_action = gtk_signal_action_new ("activate");

    for (size_t i = 0; i < G_N_ELEMENTS (activate_keyvals); i++)
      {
        g_autoptr (GtkShortcut) activate_shortcut = NULL;

        activate_shortcut = gtk_shortcut_new (gtk_keyval_trigger_new (activate_keyvals[i], 0),
                                              g_object_ref (activate_action));

        gtk_widget_class_add_shortcut (widget_class, activate_shortcut);
      }
  }

  properties[PROP_CONTEXT] = g_param_spec_object ("context",
                                                  "Context",
                                                  "The GcalContext of the application",
                                                  GCAL_TYPE_CONTEXT,
                                                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, N_PROPS, properties);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/calendar/ui/views/gcal-month-cell.ui");

  gtk_widget_class_bind_template_child (widget_class, GcalMonthCell, breakpoint_bin);
  gtk_widget_class_bind_template_child (widget_class, GcalMonthCell, day_label);
  gtk_widget_class_bind_template_child (widget_class, GcalMonthCell, header_box);
  gtk_widget_class_bind_template_child (widget_class, GcalMonthCell, overflow_button);
  gtk_widget_class_bind_template_child (widget_class, GcalMonthCell, overflow_inscription);
  gtk_widget_class_bind_template_child (widget_class, GcalMonthCell, overlay);

  gtk_widget_class_bind_template_callback (widget_class, overflow_button_clicked_cb);

  gtk_widget_class_set_css_name (widget_class, "monthcell");

  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gcal_month_cell_init (GcalMonthCell *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}

GtkWidget*
gcal_month_cell_new (void)
{
  return g_object_new (GCAL_TYPE_MONTH_CELL, NULL);
}


GDateTime*
gcal_month_cell_get_date (GcalMonthCell *self)
{
  g_return_val_if_fail (GCAL_IS_MONTH_CELL (self), NULL);

  return self->date;
}

void
gcal_month_cell_set_date (GcalMonthCell *self,
                          GDateTime     *date)
{
  gint day_of_month;

  g_return_if_fail (GCAL_IS_MONTH_CELL (self));

  if (self->date && date && gcal_date_time_compare_date (self->date, date) == 0)
    return;

  gcal_clear_date_time (&self->date);
  self->date = g_date_time_ref (date);
  day_of_month = g_date_time_get_day_of_month (date);

  update_style_flags (self);
  add_month_separators (self);

  if (day_of_month == 1)
    {
      g_autofree gchar *month_name = g_date_time_format (date, "%B");
      gtk_label_set_text (self->day_label, month_name);
      gtk_widget_add_css_class (GTK_WIDGET (self->day_label), "first-day");
    }
  else
    {
      g_autofree gchar *date_number = g_strdup_printf ("%d", day_of_month);
      gtk_label_set_text (self->day_label, date_number);
      gtk_widget_remove_css_class (GTK_WIDGET (self->day_label), "first-day");
    }
}

GcalContext*
gcal_month_cell_get_context (GcalMonthCell *self)
{
  g_return_val_if_fail (GCAL_IS_MONTH_CELL (self), NULL);

  return self->context;
}

void
gcal_month_cell_set_context (GcalMonthCell *self,
                             GcalContext   *context)
{
  g_return_if_fail (GCAL_IS_MONTH_CELL (self));

  if (!g_set_object (&self->context, context))
    return;

  g_signal_connect_object (gcal_context_get_clock (context),
                           "day-changed",
                           G_CALLBACK (day_changed_cb),
                           self,
                           0);

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_CONTEXT]);
}

guint
gcal_month_cell_get_overflow (GcalMonthCell *self)
{
  g_return_val_if_fail (GCAL_IS_MONTH_CELL (self), 0);

  return self->n_overflow;
}

void
gcal_month_cell_set_overflow (GcalMonthCell *self,
                              guint          n_overflow)
{
  g_return_if_fail (GCAL_IS_MONTH_CELL (self));

  if (self->n_overflow == n_overflow)
    return;

  self->n_overflow = n_overflow;

  gtk_widget_set_sensitive (self->overflow_button, n_overflow > 0);

  if (n_overflow > 0)
    {
      g_autofree gchar *text = g_strdup_printf ("+%d", n_overflow);
      gtk_inscription_set_text (self->overflow_inscription, text);
    }
  else
    {
      gtk_inscription_set_text (self->overflow_inscription, "");
    }
}

gint
gcal_month_cell_get_content_space (GcalMonthCell *self)
{
  g_return_val_if_fail (GCAL_IS_MONTH_CELL (self), -1);

  return gtk_widget_get_height (GTK_WIDGET (self)) -
         gcal_month_cell_get_header_height (self);
}

gint
gcal_month_cell_get_header_height (GcalMonthCell *self)
{
  g_return_val_if_fail (GCAL_IS_MONTH_CELL (self), -1);

  return gtk_widget_get_height (self->header_box) +
         gtk_widget_get_margin_top (self->header_box) +
         gtk_widget_get_margin_bottom (self->header_box);
}

gint
gcal_month_cell_get_overflow_height (GcalMonthCell *self)
{
  g_return_val_if_fail (GCAL_IS_MONTH_CELL (self), -1);

  return gtk_widget_get_height (self->overflow_button);
}

gboolean
gcal_month_cell_get_selected (GcalMonthCell *self)
{
  g_return_val_if_fail (GCAL_IS_MONTH_CELL (self), FALSE);

  return self->selected;
}

void
gcal_month_cell_set_selected (GcalMonthCell *self,
                              gboolean       selected)
{
  GtkStateFlags flags;

  g_return_if_fail (GCAL_IS_MONTH_CELL (self));

  if (self->selected == selected)
    return;

  self->selected = selected;
  gtk_accessible_update_state (GTK_ACCESSIBLE (self),
                               GTK_ACCESSIBLE_STATE_SELECTED, selected,
                               -1);

  /* Update style context state */
  flags = gtk_widget_get_state_flags (GTK_WIDGET (self));

  if (selected)
    flags |= GTK_STATE_FLAG_SELECTED;
  else
    flags &= ~GTK_STATE_FLAG_SELECTED;

  gtk_widget_set_state_flags (GTK_WIDGET (self), flags, TRUE);

  gtk_widget_queue_resize (GTK_WIDGET (self));
}

GtkWidget *
gcal_month_cell_get_overflow_button (GcalMonthCell *self)
{
  g_assert (GCAL_IS_MONTH_CELL (self));

  return self->overflow_button;
}

