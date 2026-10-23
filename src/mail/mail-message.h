/* mail-message.h – Nachricht als Eintrag der Nachrichtenliste (#352)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <camel/camel.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_MAIL_MESSAGE (revolution_mail_message_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionMailMessage, revolution_mail_message, REVOLUTION, MAIL_MESSAGE, GObject)

RevolutionMailMessage *revolution_mail_message_new         (CamelMessageInfo      *info,
                                                            GDateTime             *now);
const char            *revolution_mail_message_get_uid     (RevolutionMailMessage *self);
gboolean               revolution_mail_message_update      (RevolutionMailMessage *self,
                                                            RevolutionMailMessage *from);
void                   revolution_mail_message_set_flags   (RevolutionMailMessage *self,
                                                            gboolean               unread,
                                                            gboolean               starred);
gboolean               revolution_mail_message_get_unread  (RevolutionMailMessage *self);
gboolean               revolution_mail_message_get_starred (RevolutionMailMessage *self);
gint64                 revolution_mail_message_get_timestamp (RevolutionMailMessage *self);
gboolean               revolution_mail_message_has_preview (RevolutionMailMessage *self);

G_END_DECLS
