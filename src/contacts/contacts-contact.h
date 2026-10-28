/* contacts-contact.h – Eintrag der Kontaktliste (#368)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>
#include <libebook/libebook.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_CONTACTS_CONTACT (revolution_contacts_contact_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionContactsContact, revolution_contacts_contact, REVOLUTION, CONTACTS_CONTACT, GObject)

RevolutionContactsContact *revolution_contacts_contact_new         (EContact                  *contact);
void                       revolution_contacts_contact_set_contact (RevolutionContactsContact *self,
                                                                    EContact                  *contact);
const char                *revolution_contacts_contact_get_uid     (RevolutionContactsContact *self);
const char                *revolution_contacts_contact_get_name    (RevolutionContactsContact *self);
const char                *revolution_contacts_contact_get_initial (RevolutionContactsContact *self);
EContact                  *revolution_contacts_contact_get_contact (RevolutionContactsContact *self);

G_END_DECLS
