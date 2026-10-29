/* Übernommen aus gnome-calendar 50.0 src/gui/views/gcal-week-grid.c:1-1007 (ff4d7f7d, GPL-3.0-or-later).
 * Revolution #374: ohne Ablegen per Ziehen (Drop-Ziel, move_event_to_cell; Schreiben folgt mit #375);
 * Tagesansicht: Spaltenzahl n_days (1 oder 7) statt N_WEEKDAYS, Beginn über gcal_week_view_common_get_start. */
/* gcal-week-grid.c
 *
 * Copyright (C) 2016-2020 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *                    Vamsi Krishna Gollapudi <pandu.sonu@yahoo.com>
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

#define G_LOG_DOMAIN "GcalWeekGrid"

#include "gcal-clock.h"
#include "gcal-context.h"
#include "gcal-debug.h"
#include "gcal-gui-utils.h"
#include "gcal-week-grid.h"
#include "gcal-week-view.h"
#include "gcal-week-view-common.h"
#include "gcal-utils.h"
#include "gcal-view-private.h"
#include "gcal-event-widget.h"
#include "gcal-range-tree.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <string.h>
#include <math.h>

typedef struct
{
  GtkWidget          *widget;
  GcalEvent          *event;
} ChildData;

struct _GcalWeekGrid
{
  GtkWidget           parent;

  GtkWidget          *hours_sidebar;
  GtkWidget          *now_strip;
  GtkEventController *motion_controller;

  GDateTime          *active_date;

  GcalRangeTree      *events;

  /*
   * These fields are "cells" rather than minutes. Each cell
   * correspond to 30 minutes.
   */
  struct {
    gint              start;
    gint              end;
    GtkWidget        *widget;
  } selection;

  struct {
    gint              cell;
    GtkWidget        *widget;
    gint              event_minutes;
  } dnd;

  GcalContext        *context;

  gint                n_days; /* Revolution #374 */
};

G_DEFINE_TYPE (GcalWeekGrid, gcal_week_grid, GTK_TYPE_WIDGET);


enum
{
  EVENT_ACTIVATED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

/*
 * Auxiliary methods
 */

static ChildData*
child_data_new (GtkWidget *widget,
                GcalEvent *event)
{
  ChildData *data;

  data = g_new (ChildData, 1);
  data->widget = widget;
  data->event = g_object_ref (event);

  return data;
}

static void
child_data_free (gpointer data)
{
  ChildData *child_data = data;

  if (!child_data)
    return;

  g_clear_pointer (&child_data->widget, gtk_widget_unparent);
  g_clear_object (&child_data->event);
  g_free (child_data);
}

static inline gint
uint16_compare (gconstpointer a,
                gconstpointer b)
{
  return GPOINTER_TO_UINT (*(gint*)a) - GPOINTER_TO_UINT (*(gint*)b);
}

static inline guint
get_event_index (GcalRangeTree *tree,
                 GcalRange     *range)
{
  g_autoptr (GPtrArray) array = NULL;
  gint idx, i;

  idx = 0;
  array = gcal_range_tree_get_data_at_range (tree, range);

  if (!array)
    return 0;

  g_ptr_array_sort (array, uint16_compare);

  for (i = 0; array && i < array->len; i++)
    {
      if (idx == GPOINTER_TO_INT (g_ptr_array_index (array, i)))
        idx++;
      else
        break;
    }

  return idx;
}

static guint
count_overlaps_at_range (GcalRangeTree *self,
                         GcalRange     *range)
{
  g_autoptr (GDateTime) start = NULL;
  g_autoptr (GDateTime) end = NULL;
  guint64 counter;
  gint minutes;
  gint i;

  g_return_val_if_fail (self, 0);

  counter = 0;
  start = gcal_range_get_start (range);
  end = gcal_range_get_end (range);
  minutes = g_date_time_difference (end, start) / G_TIME_SPAN_MINUTE;

  for (i = 0; i < minutes; i++)
    {
      g_autoptr (GcalRange) minute_range = NULL;
      guint n_events;

      minute_range = gcal_range_new_take (g_date_time_add_minutes (start, i),
                                          g_date_time_add_minutes (start, i + 1),
                                          GCAL_RANGE_DEFAULT);
      n_events = gcal_range_tree_count_entries_at_range (self, minute_range);

      if (n_events == 0)
        break;

      counter = MAX (counter, n_events);
    }

  return counter;
}

/*
 * Callbacks
 */

static void
on_click_gesture_pressed_cb (GtkGestureClick *click_gesture,
                             gint             n_press,
                             gdouble          x,
                             gdouble          y,
                             GcalWeekGrid    *self)
{
  gdouble minute_height;
  gint column_width;
  gint column;
  gint minute;

  g_assert (self->selection.start == -1);
  g_assert (self->selection.end == -1);

  minute_height = (gdouble) gtk_widget_get_height (GTK_WIDGET (self)) / MINUTES_PER_DAY;
  column_width = floor (gtk_widget_get_width (GTK_WIDGET (self)) / self->n_days);
  column = (gint) x / column_width;
  minute = y / minute_height;
  minute = minute - (minute % 30);

  self->selection.start = (column * MINUTES_PER_DAY + minute) / 30;
  self->selection.end = self->selection.start;

  gtk_widget_set_visible (self->selection.widget, TRUE);

  gtk_event_controller_set_propagation_phase (self->motion_controller, GTK_PHASE_BUBBLE);
}

static void
on_event_widget_activated_cb (GcalEventWidget *widget,
                              GcalWeekGrid    *self)
{
  g_signal_emit (self, signals[EVENT_ACTIVATED], 0, widget);
}

static void
on_motion_controller_motion_cb (GtkEventControllerMotion *motion_controller,
                                gdouble                   x,
                                gdouble                   y,
                                GcalWeekGrid             *self)
{
  gdouble minute_height;
  gint column;
  gint minute;

  minute_height = (gdouble) gtk_widget_get_height (GTK_WIDGET (self)) / MINUTES_PER_DAY;
  column = self->selection.start * 30 / MINUTES_PER_DAY;
  minute = y / minute_height;
  minute = minute - (minute % 30);

  self->selection.end = (column * MINUTES_PER_DAY + minute) / 30;

  gtk_widget_queue_allocate (GTK_WIDGET (self));
}

static void
on_click_gesture_released_cb (GtkGestureClick *click_gesture,
                              gint             n_press,
                              gdouble          x,
                              gdouble          y,
                              GcalWeekGrid    *self)
{
  g_autoptr (GDateTime) week_start = NULL;
  g_autoptr (GcalRange) range = NULL;
  g_autoptr (GDateTime) start = NULL;
  g_autoptr (GDateTime) end = NULL;
  graphene_point_t out;
  GtkWidget *weekview;
  gboolean ltr;
  gdouble minute_height;
  gdouble local_x;
  gdouble local_y;
  gint column;
  gint minute;
  gint start_cell;
  gint end_cell;

  ltr = gtk_widget_get_direction (GTK_WIDGET (self)) != GTK_TEXT_DIR_RTL;

  minute_height = (gdouble) gtk_widget_get_height (GTK_WIDGET (self)) / MINUTES_PER_DAY;
  column = self->selection.start * 30 / MINUTES_PER_DAY;
  minute = y / minute_height;
  minute = minute - (minute % 30);

  self->selection.end = (column * MINUTES_PER_DAY + minute) / 30;

  start_cell = self->selection.start;
  end_cell = self->selection.end;

  if (start_cell > end_cell)
    {
      start_cell = start_cell + end_cell;
      end_cell = start_cell - end_cell;
      start_cell = start_cell - end_cell;
    }

  gtk_widget_queue_allocate (GTK_WIDGET (self));

  /* Fake the week view's event so we can control the X and Y values */
  weekview = gtk_widget_get_ancestor (GTK_WIDGET (self), GCAL_TYPE_WEEK_VIEW);
  week_start = gcal_week_view_common_get_start (self->active_date, self->n_days);

  if (ltr)
    {
      start = gcal_date_time_add_floating_minutes (week_start, start_cell * 30);
      end = gcal_date_time_add_floating_minutes (week_start, (end_cell + 1) * 30);
    }
  else
    {
      guint rtl_start_cell, rtl_end_cell, rtl_column;

      /* Fix the minute */
      rtl_column = self->n_days - 1 - column;
      rtl_start_cell = start_cell + (rtl_column - column) * 48;
      rtl_end_cell = (rtl_column * MINUTES_PER_DAY + minute) / 30;

      start = gcal_date_time_add_floating_minutes (week_start, rtl_start_cell * 30);
      end = gcal_date_time_add_floating_minutes (week_start, (rtl_end_cell + 1) * 30);
    }

  local_x = round ((column + 0.5) * (gtk_widget_get_width (GTK_WIDGET (self)) / (float) self->n_days));
  local_y = (minute + 15) * minute_height;

  if (!gtk_widget_compute_point (GTK_WIDGET (self),
                                 weekview,
                                 &GRAPHENE_POINT_INIT (local_x, local_y),
                                 &out))
    g_assert_not_reached ();

  range = gcal_range_new (start, end, GCAL_RANGE_DEFAULT);
  gcal_view_create_event (GCAL_VIEW (weekview), range, out.x, out.y);

  gtk_event_controller_set_propagation_phase (self->motion_controller, GTK_PHASE_NONE);
}

static void
gcal_week_grid_dispose (GObject *object)
{
  GcalWeekGrid *self = GCAL_WEEK_GRID (object);

  g_clear_pointer (&self->dnd.widget, gtk_widget_unparent);
  g_clear_pointer (&self->events, gcal_range_tree_unref);
  g_clear_pointer (&self->now_strip, gtk_widget_unparent);
  g_clear_pointer (&self->selection.widget, gtk_widget_unparent);

  G_OBJECT_CLASS (gcal_week_grid_parent_class)->dispose (object);
}

static void
gcal_week_grid_finalize (GObject *object)
{
  GcalWeekGrid *self = GCAL_WEEK_GRID (object);

  gcal_clear_date_time (&self->active_date);

  G_OBJECT_CLASS (gcal_week_grid_parent_class)->finalize (object);
}

static void
gcal_week_grid_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
  G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
gcal_week_grid_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
  G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static inline gint
get_today_column (GcalWeekGrid *self)
{
  g_autoptr(GDateTime) week_start = NULL;
  g_autoptr(GDateTime) today = NULL;
  gint days_diff;

  today = g_date_time_new_now_local ();
  week_start = gcal_week_view_common_get_start (self->active_date, self->n_days);
  days_diff = g_date_time_difference (today, week_start) / G_TIME_SPAN_DAY;

  /* Today is out of range */
  if (g_date_time_compare (today, week_start) < 0 || days_diff >= self->n_days)
    return -1;

  return days_diff;
}

static void
gcal_week_grid_measure (GtkWidget      *widget,
                        GtkOrientation  orientation,
                        gint            for_size,
                        gint           *minimum,
                        gint           *natural,
                        gint           *minimum_baseline,
                        gint           *natural_baseline)
{
  if (minimum)
    *minimum = 1;
  if (natural)
    *natural = 1;
}

static void
gcal_week_grid_snapshot (GtkWidget   *widget,
                         GtkSnapshot *snapshot)
{
  g_autoptr (GPtrArray) widgets = NULL;
  GcalWeekGrid *self;
  gint width, height;

  self = GCAL_WEEK_GRID (widget);
  width = gtk_widget_get_width (widget);
  height = gtk_widget_get_height (widget);

  gcal_week_view_common_snapshot_hour_lines (widget, snapshot, GTK_ORIENTATION_HORIZONTAL, width, height, self->n_days);
  gcal_week_view_common_snapshot_hour_lines (widget, snapshot, GTK_ORIENTATION_VERTICAL, width, height, self->n_days);

  /* First, draw the selection */
  gtk_widget_snapshot_child (widget, self->selection.widget, snapshot);
  gtk_widget_snapshot_child (widget, self->dnd.widget, snapshot);

  widgets = gcal_range_tree_get_all_data (self->events);
  for (guint i = 0; i < widgets->len; i++)
    {
      ChildData *child_data = g_ptr_array_index (widgets, i);

      gtk_widget_snapshot_child (widget, child_data->widget, snapshot);
    }

  gtk_widget_snapshot_child (widget, self->now_strip, snapshot);
}

static void
gcal_week_grid_size_allocate (GtkWidget *widget,
                              gint       width,
                              gint       height,
                              gint       baseline)
{
  GcalWeekGrid *self = GCAL_WEEK_GRID (widget);
  g_autoptr (GDateTime) week_start = NULL;
  GcalRangeTree *overlaps;
  gboolean ltr;
  gdouble minutes_height;
  gdouble column_width;
  gint today_column;

  ltr = gtk_widget_get_direction (widget) != GTK_TEXT_DIR_RTL;

  /* Preliminary calculations */
  minutes_height = (gdouble) height / MINUTES_PER_DAY;
  column_width = (gdouble) width / (float) self->n_days;

  /* Selection */
  if (gtk_widget_should_layout (self->selection.widget))
    {
      gint selection_height;
      gint start_row;
      gint end_row;
      gint column;
      gint start;
      gint end;
      gint x;
      gint y;

      g_assert (self->selection.start != -1);
      g_assert (self->selection.end != -1);

      start = self->selection.start;
      end = self->selection.end;

      /* Swap cells if needed */
      if (start > end)
        {
          start = start + end;
          end = start - end;
          start = start - end;
        }

      column = (int) floor (start * 30 / (double) MINUTES_PER_DAY);

#define Y_POSITION(row_) ((gint) round (((row_) * 30) * minutes_height))

      start_row = start - column * 48;
      end_row = (end + 1) - column * 48;

      x = column * column_width;
      y = Y_POSITION (start_row);
      selection_height = Y_POSITION (end_row) - y;

#undef Y_POSITION

      gtk_widget_size_allocate (self->selection.widget,
                                &(GtkAllocation) {
                                  .x = x,
                                  .y = y,
                                  .width = column_width - 1,
                                  .height = selection_height,
                                },
                                baseline);
    }

  if (gtk_widget_should_layout (self->dnd.widget))
    {
      gdouble cell_height, event_height;
      gint column, row;

      g_assert (self->dnd.cell != -1);

      cell_height = minutes_height * 30;
      event_height = minutes_height * self->dnd.event_minutes;
      column = self->dnd.cell / (MINUTES_PER_DAY / 30);
      row = self->dnd.cell - column * 48;

      gtk_widget_size_allocate (self->dnd.widget,
                                &(GtkAllocation) {
                                  .x = column * column_width,
                                  .y = row * cell_height,
                                  .width = column_width,
                                  .height = event_height,
                                },
                                baseline);
    }

  /* Temporary range tree to hold positioned events' indexes */
  overlaps = gcal_range_tree_new ();

  week_start = gcal_week_view_common_get_start (self->active_date, self->n_days);

  /*
   * Iterate through weekdays; we don't have to worry about events that
   * jump between days because they're already handled by GcalWeekHeader.
   */
  for (gint i = 0; i < self->n_days; i++)
    {
      g_autoptr (GcalRange) day_range = NULL;
      GPtrArray *widgets_data;
      guint j;

      day_range = gcal_range_new_take (g_date_time_add_days (week_start, i),
                                       g_date_time_add_days (week_start, i + 1),
                                       GCAL_RANGE_DEFAULT);
      widgets_data = gcal_range_tree_get_data_at_range (self->events, day_range);

      for (j = 0; widgets_data && j < widgets_data->len; j++)
        {
          g_autoptr (GDateTime) event_start = NULL;
          g_autoptr (GDateTime) event_end = NULL;
          GtkAllocation child_allocation;
          GtkWidget *event_widget;
          GcalRange *event_range;
          ChildData *data;
          guint64 events_at_range;
          gint event_minutes;
          gint minimum_width;
          gint minimum_height;
          gint widget_index;
          gint offset;
          gint event_height;
          gint event_width;
          gint x;
          gint y;

          data =  g_ptr_array_index (widgets_data, j);
          event_widget = data->widget;

          if (!gtk_widget_should_layout (event_widget))
            continue;

          event_range = gcal_event_get_range (data->event);
          event_start = g_date_time_to_local (gcal_event_get_date_start (data->event));
          event_end = g_date_time_to_local (gcal_event_get_date_end (data->event));

          /* The total number of events available in this range */
          events_at_range = count_overlaps_at_range (self->events, event_range);

          /* The real horizontal position of this event */
          widget_index = get_event_index (overlaps, event_range);

          event_minutes = g_date_time_difference (event_end, event_start) / G_TIME_SPAN_MINUTE;

          /* Compute the height of the widget */
          gtk_widget_measure (event_widget, GTK_ORIENTATION_VERTICAL, -1, &minimum_height, NULL, NULL, NULL);
          event_height = event_minutes * minutes_height;
          event_height = MAX (minimum_height, event_height);

          /* Compute the width of the widget */
          gtk_widget_measure (event_widget, GTK_ORIENTATION_HORIZONTAL, event_height, &minimum_width, NULL, NULL, NULL);
          event_width = column_width / events_at_range;
          event_width = MAX (minimum_width, event_width);

          offset = event_width * widget_index;
          y = (g_date_time_get_hour (event_start) * 60 + g_date_time_get_minute (event_start)) * minutes_height;

          if (ltr)
            x = column_width * i + offset + 1;
          else
            x = width - event_width - (column_width * i + offset + 1);

          /* Setup the child position and size */
          child_allocation.x = x;
          child_allocation.y = y;
          child_allocation.width = event_width;
          child_allocation.height = event_height;

          gtk_widget_size_allocate (event_widget, &child_allocation, baseline);

          /*
           * Add the current event to the temporary overlaps tree so we have a way to
           * know how many events are already positioned in the current column.
           */
          gcal_range_tree_add_range (overlaps, event_range, GUINT_TO_POINTER (widget_index));
        }

      g_clear_pointer (&widgets_data, g_ptr_array_unref);
    }

  g_clear_pointer (&overlaps, gcal_range_tree_unref);

  /* Today column */
  today_column = get_today_column (GCAL_WEEK_GRID (widget));

  gtk_widget_set_child_visible (self->now_strip, today_column != -1);

  if (today_column != -1)
    {
      g_autoptr (GDateTime) now = NULL;
      GtkAllocation allocation;
      guint minutes_from_midnight;
      gint now_strip_height;
      gint x;

      now = g_date_time_new_now_local ();
      minutes_from_midnight = g_date_time_get_hour (now) * 60 + g_date_time_get_minute (now);

      gtk_widget_measure (self->now_strip,
                          GTK_ORIENTATION_VERTICAL,
                          -1,
                          &now_strip_height,
                          NULL,
                          NULL,
                          NULL);

      if (ltr)
        x = today_column * column_width;
      else
        x = width - (today_column * column_width) - column_width;

      allocation.x = x;
      allocation.y = round (minutes_from_midnight * ((gdouble) height / MINUTES_PER_DAY));
      allocation.width = column_width;
      allocation.height = MAX (1, now_strip_height);

      gtk_widget_size_allocate (self->now_strip, &allocation, baseline);
    }
}

static void
gcal_week_grid_class_init (GcalWeekGridClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gcal_week_grid_dispose;
  object_class->finalize = gcal_week_grid_finalize;
  object_class->get_property = gcal_week_grid_get_property;
  object_class->set_property = gcal_week_grid_set_property;

  widget_class->measure = gcal_week_grid_measure;
  widget_class->size_allocate = gcal_week_grid_size_allocate;
  widget_class->snapshot = gcal_week_grid_snapshot;

  signals[EVENT_ACTIVATED] = g_signal_new ("event-activated",
                                           GCAL_TYPE_WEEK_GRID,
                                           G_SIGNAL_RUN_FIRST,
                                           0,  NULL, NULL, NULL,
                                           G_TYPE_NONE,
                                           1,
                                           GCAL_TYPE_EVENT_WIDGET);

  gtk_widget_class_set_css_name (widget_class, "weekgrid");
}

static void
gcal_week_grid_init (GcalWeekGrid *self)
{
  GtkGesture *click_gesture;

  self->selection.start = -1;
  self->selection.end = -1;
  self->selection.widget = gcal_create_selection_widget ();
  gtk_widget_set_visible (self->selection.widget, FALSE);
  gtk_widget_set_parent (self->selection.widget, GTK_WIDGET (self));

  self->dnd.cell = -1;
  self->dnd.widget = gcal_create_drop_target_widget ();
  gtk_widget_set_visible (self->dnd.widget, FALSE);
  gtk_widget_set_parent (self->dnd.widget, GTK_WIDGET (self));

  self->events = gcal_range_tree_new_with_free_func (child_data_free);
  self->n_days = N_WEEKDAYS;

  self->now_strip = adw_bin_new ();
  gtk_widget_add_css_class (self->now_strip, "now-strip");
  gtk_widget_set_can_target (self->now_strip, FALSE);
  gtk_widget_set_parent (self->now_strip, GTK_WIDGET (self));

  click_gesture = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click_gesture), GDK_BUTTON_PRIMARY);
  g_signal_connect (click_gesture, "pressed", G_CALLBACK (on_click_gesture_pressed_cb), self);
  g_signal_connect (click_gesture, "released", G_CALLBACK (on_click_gesture_released_cb), self);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click_gesture));

  self->motion_controller = gtk_event_controller_motion_new ();
  g_signal_connect (self->motion_controller, "motion", G_CALLBACK (on_motion_controller_motion_cb), self);
  gtk_event_controller_set_propagation_phase (self->motion_controller, GTK_PHASE_NONE);
  gtk_widget_add_controller (GTK_WIDGET (self), self->motion_controller);
}

/* Public API */
void
gcal_week_grid_set_context (GcalWeekGrid *self,
                            GcalContext  *context)
{
  g_return_if_fail (GCAL_IS_WEEK_GRID (self));

  self->context = context;

  g_signal_connect_object (gcal_context_get_clock (context),
                           "minute-changed",
                           G_CALLBACK (gtk_widget_queue_resize),
                           self->now_strip,
                           G_CONNECT_SWAPPED);
}

void
gcal_week_grid_add_event (GcalWeekGrid *self,
                          GcalEvent    *event)
{
  GtkWidget *widget;

  g_return_if_fail (GCAL_IS_WEEK_GRID (self));

  g_object_ref (event);

  widget = g_object_new (GCAL_TYPE_EVENT_WIDGET,
                         "context", self->context,
                         "event", event,
                         "orientation", GTK_ORIENTATION_VERTICAL,
                         "timestamp-policy", GCAL_TIMESTAMP_POLICY_START,
                         NULL);

  gcal_range_tree_add_range (self->events,
                             gcal_event_get_range (event),
                             child_data_new (widget, event));

  g_signal_connect (widget, "activate", G_CALLBACK (on_event_widget_activated_cb), self);

  gtk_widget_set_parent (widget, GTK_WIDGET (self));
}

void
gcal_week_grid_remove_event (GcalWeekGrid *self,
                             const gchar  *uid)
{
  g_autoptr (GPtrArray) widgets = NULL;
  guint i;

  g_return_if_fail (GCAL_IS_WEEK_GRID (self));

  widgets = gcal_range_tree_get_all_data (self->events);

  for (i = 0; widgets && i < widgets->len; i++)
    {
      ChildData *data;
      GcalEvent *event;

      data = g_ptr_array_index (widgets, i);
      event = gcal_event_widget_get_event (GCAL_EVENT_WIDGET (data->widget));

      if (g_strcmp0 (gcal_event_get_uid (event), uid) != 0)
        continue;

      gcal_range_tree_remove_range (self->events, gcal_event_get_range (event), data);
      gtk_widget_queue_allocate (GTK_WIDGET (self));
    }
}

GList*
gcal_week_grid_get_children_by_uuid (GcalWeekGrid          *self,
                                     GcalRecurrenceModType  mod,
                                     const gchar           *uid)
{
  g_autoptr (GList) result = NULL;

  GCAL_ENTRY;

  result = filter_children_by_uid_and_modtype (GTK_WIDGET (self), mod, uid);

  GCAL_RETURN (g_steal_pointer (&result));
}

void
gcal_week_grid_clear_marks (GcalWeekGrid *self)
{
  g_return_if_fail (GCAL_IS_WEEK_GRID (self));

  self->selection.start = -1;
  self->selection.end = -1;
  gtk_widget_set_visible (self->selection.widget, FALSE);
}

void
gcal_week_grid_set_date (GcalWeekGrid *self,
                         GDateTime    *date)
{
  gcal_set_date_time (&self->active_date, date);

  gtk_widget_queue_resize (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* Revolution #374: Tagesansicht (1 Spalte) */
void
gcal_week_grid_set_n_days (GcalWeekGrid *self,
                           gint          n_days)
{
  g_return_if_fail (GCAL_IS_WEEK_GRID (self));

  self->n_days = n_days;
  gtk_widget_queue_resize (GTK_WIDGET (self));
}
