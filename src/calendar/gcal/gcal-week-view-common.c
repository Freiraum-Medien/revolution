/* Übernommen aus gnome-calendar 50.0 src/gui/views/gcal-week-view-common.c:1-90 (ff4d7f7d, GPL-3.0-or-later).
 * Revolution #374: Spaltenzahl n_days statt N_WEEKDAYS, Beginn und Ende des Bereichs (Tagesansicht). */
/* gcal-week-view-common.c
 *
 * Copyright 2022 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
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

#include "gcal-week-view-common.h"
#include "gcal-utils.h"

void
gcal_week_view_common_snapshot_hour_lines (GtkWidget      *widget,
                                           GtkSnapshot    *snapshot,
                                           GtkOrientation  orientation,
                                           gint            width,
                                           gint            height,
                                           gint            n_days)
{
  GdkRGBA color;
  gboolean ltr;
  gdouble column_width;
  guint i;

  ltr = gtk_widget_get_direction (widget) != GTK_TEXT_DIR_RTL;

  gtk_snapshot_save (snapshot);
  gtk_widget_get_color (widget, &color);
  color.alpha = 0.15;
  gtk_snapshot_restore (snapshot);

  column_width = width / (float) n_days;

  switch (orientation)
    {
    case GTK_ORIENTATION_HORIZONTAL:
      for (i = 0; i < n_days; i++)
        {
          gdouble x;

          if (ltr)
            x = column_width * i;
          else
            x = width - column_width * i;

          gtk_snapshot_append_color (snapshot,
                                     &color,
                                     &GRAPHENE_RECT_INIT (x, 0.f, 1.0, height));
        }
      break;


    case GTK_ORIENTATION_VERTICAL:
      /* Main lines */
      for (i = 1; i < 24; i++)
        {
          gtk_snapshot_append_color (snapshot,
                                     &color,
                                     &GRAPHENE_RECT_INIT (0.f,
                                                          ALIGNED ((height / 24.0) * i),
                                                          width,
                                                          1.0));
        }

      /* In-between lines */
      color.alpha /= 2.0;
      for (i = 0; i < 24; i++)
        {
          gdouble half_cell_height = (height / 24.0) / 2.0;
          gtk_snapshot_append_color (snapshot,
                                     &color,
                                     &GRAPHENE_RECT_INIT (0.f,
                                                          ALIGNED ((height / 24.0) * i + half_cell_height),
                                                          width,
                                                          1.0));
        }
      break;
    }
}

/* Revolution #374: Beginn (einschließlich) und Ende (ausschließlich) des Bereichs, den die Woche bzw. der Tag
 * (n_days 1) zeigt, 0:00 Ortszeit wie gcal_date_time_get_start_of_week. */
GDateTime*
gcal_week_view_common_get_start (GDateTime *date,
                                 gint       n_days)
{
  if (n_days == N_WEEKDAYS)
    return gcal_date_time_get_start_of_week (date);

  return g_date_time_new_local (g_date_time_get_year (date),
                                g_date_time_get_month (date),
                                g_date_time_get_day_of_month (date),
                                0, 0, 0);
}

GDateTime*
gcal_week_view_common_get_end (GDateTime *date,
                               gint       n_days)
{
  g_autoptr (GDateTime) start = gcal_week_view_common_get_start (date, n_days);

  return g_date_time_add_days (start, n_days);
}
