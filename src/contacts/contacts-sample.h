/* contacts-sample.h – Listeneintrag des Prototyps (#338), Daten entfernt (#342)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_CONTACTS_SAMPLE_ENTRY (revolution_contacts_sample_entry_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionContactsSampleEntry, revolution_contacts_sample_entry, REVOLUTION, CONTACTS_SAMPLE_ENTRY, GObject)

G_END_DECLS
