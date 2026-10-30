/* mail-invitation.h – Einladungskarte im Lesebereich (#376)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>
#include <libecal/libecal.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_MAIL_INVITATION (revolution_mail_invitation_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionMailInvitation, revolution_mail_invitation, REVOLUTION, MAIL_INVITATION, AdwBin)

void revolution_mail_invitation_set (RevolutionMailInvitation *self,
                                     const char               *account_uid,
                                     const char               *ical);

/* #377: iTIP REQUEST/CANCEL des Organisators an die Teilnehmer, wenn der Server nicht selbst einlädt. Nimmt comp.
 * master: mit dem Hauptdatensatz der Serie statt der Instanz („alle Termine“). */
void     revolution_mail_invitation_send        (ECalClient          *client,
                                                 ICalComponent       *comp,
                                                 ICalPropertyMethod   method,
                                                 gboolean             master,
                                                 GAsyncReadyCallback  callback,
                                                 gpointer             user_data);
gboolean revolution_mail_invitation_send_finish (GAsyncResult        *result,
                                                 GError             **error);

G_END_DECLS
