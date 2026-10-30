/* mail-invitation.h – Einladungskarte im Lesebereich (#376)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_MAIL_INVITATION (revolution_mail_invitation_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionMailInvitation, revolution_mail_invitation, REVOLUTION, MAIL_INVITATION, AdwBin)

void revolution_mail_invitation_set (RevolutionMailInvitation *self,
                                     const char               *account_uid,
                                     const char               *ical);

G_END_DECLS
