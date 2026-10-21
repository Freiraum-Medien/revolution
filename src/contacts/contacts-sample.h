/* contacts-sample.h – statische Beispieldaten für den Prototyp (#338)
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

GListModel *revolution_contacts_sample_books    (void);
GListModel *revolution_contacts_sample_contacts (void);

G_END_DECLS
