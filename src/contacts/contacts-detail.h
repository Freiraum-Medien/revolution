/* contacts-detail.h – Detailansicht eines Kontakts (#369)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "contacts-contact.h"

G_BEGIN_DECLS

#define REVOLUTION_TYPE_CONTACTS_DETAIL (revolution_contacts_detail_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionContactsDetail, revolution_contacts_detail, REVOLUTION, CONTACTS_DETAIL, AdwBin)

void revolution_contacts_detail_set_contact (RevolutionContactsDetail  *self,
                                             RevolutionContactsContact *item);

G_END_DECLS
