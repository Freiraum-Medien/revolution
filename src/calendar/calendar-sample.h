/* calendar-sample.h – statische Beispieldaten für den Prototyp (#339)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_SAMPLE_CALENDAR (revolution_sample_calendar_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionSampleCalendar, revolution_sample_calendar, REVOLUTION, SAMPLE_CALENDAR, GObject)

/* Ein Termin. Tag 0 ist Montag, 28.09.2026 (erste Zeile der Monatsansicht
 * Oktober 2026); die Wochenansicht zeigt die Tage 21 … 27 (19.–25.10.). */
typedef struct
{
  guint       day;
  guint       days;      /* Dauer in Tagen, nur ganztägig */
  int         start;     /* Minuten ab 0:00; -1 = ganztägig */
  int         end;
  guint       slot;      /* Zeile in der Monatszelle (0 … 2) */
  guint       calendar;  /* Index der Kalenderfarbe */
  const char *summary;
  const char *location;
  const char *notes;
} RevolutionSampleEvent;

#define REVOLUTION_SAMPLE_FIRST_DAY 2026, 9, 28
#define REVOLUTION_SAMPLE_TODAY     23   /* Mittwoch, 21.10.2026 */
#define REVOLUTION_SAMPLE_WEEK      21   /* Montag, 19.10.2026 */

GListModel                  *revolution_calendar_sample_calendars (void);
const RevolutionSampleEvent *revolution_calendar_sample_events    (guint *n_events);

G_END_DECLS
