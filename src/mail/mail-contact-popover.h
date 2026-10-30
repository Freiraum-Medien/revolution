/* mail-contact-popover.h – Kontakt-Popover im Nachrichtenkopf (#455)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>
#include <libedataserver/libedataserver.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_MAIL_CONTACT_POPOVER (revolution_mail_contact_popover_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionMailContactPopover, revolution_mail_contact_popover, REVOLUTION, MAIL_CONTACT_POPOVER, GtkPopover)

void revolution_mail_contact_popover_open   (RevolutionMailContactPopover *self,
                                             ESourceRegistry              *registry,
                                             const char                   *account_uid,
                                             const char                   *name,
                                             const char                   *address);
void revolution_mail_contact_popover_cancel (RevolutionMailContactPopover *self);

G_END_DECLS
