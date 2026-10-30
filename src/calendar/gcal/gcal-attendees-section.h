/* gcal-attendees-section.h – Teilnehmer im Termin-Editor (#377)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "gcal-calendar.h"

G_BEGIN_DECLS

#define GCAL_TYPE_ATTENDEES_SECTION (gcal_attendees_section_get_type ())
G_DECLARE_FINAL_TYPE (GcalAttendeesSection, gcal_attendees_section, GCAL, ATTENDEES_SECTION, AdwBin)

void gcal_attendees_section_set_calendar (GcalAttendeesSection *self,
                                     GcalCalendar         *calendar);

G_END_DECLS
