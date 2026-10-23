/* calendar-sample.h – Listeneintrag des Prototyps (#339), Daten entfernt (#342)
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

G_END_DECLS
