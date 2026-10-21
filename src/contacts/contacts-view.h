/* contacts-view.h – Kontakte-Ansicht (Prototyp #338)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_CONTACTS_VIEW (revolution_contacts_view_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionContactsView, revolution_contacts_view, REVOLUTION, CONTACTS_VIEW, AdwBreakpointBin)

G_END_DECLS
