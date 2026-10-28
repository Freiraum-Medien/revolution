/* contacts-book.h – Adressbuch in der Seitenleiste der Kontakte (#368)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <libedataserver/libedataserver.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_CONTACTS_BOOK (revolution_contacts_book_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionContactsBook, revolution_contacts_book, REVOLUTION, CONTACTS_BOOK, GObject)

RevolutionContactsBook *revolution_contacts_book_new          (ESourceRegistry        *registry,
                                                               ESource                *source);
ESource                *revolution_contacts_book_get_source   (RevolutionContactsBook *self);
const char             *revolution_contacts_book_get_group    (RevolutionContactsBook *self);
void                    revolution_contacts_book_set_readonly (RevolutionContactsBook *self,
                                                               gboolean                readonly);

G_END_DECLS
