/* calendar-view.h – Kalender-Ansicht (Prototyp #339, #373)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>
#include <libedataserver/libedataserver.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_CALENDAR_VIEW (revolution_calendar_view_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionCalendarView, revolution_calendar_view, REVOLUTION, CALENDAR_VIEW, AdwBreakpointBin)

void revolution_calendar_view_set_registry (RevolutionCalendarView *self,
                                            ESourceRegistry        *registry);

G_END_DECLS
