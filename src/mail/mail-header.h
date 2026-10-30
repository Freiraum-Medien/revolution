/* mail-header.h – Nachrichtenkopf für Einzelansicht und Konversation (#455)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>
#include <camel/camel.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_MAIL_HEADER (revolution_mail_header_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionMailHeader, revolution_mail_header, REVOLUTION, MAIL_HEADER, GtkWidget)

void revolution_mail_header_set_summary    (RevolutionMailHeader *self,
                                            const char           *sender,
                                            const char           *avatar,
                                            const char           *subject,
                                            const char           *icon,
                                            const char           *place);
const char *revolution_mail_header_get_subject (RevolutionMailHeader *self);
void revolution_mail_header_set_source     (RevolutionMailHeader *self,
                                            CamelSession         *session,
                                            const char           *store_uid,
                                            const char           *folder_name,
                                            const char           *message_uid,
                                            const char           *account_uid);
void revolution_mail_header_set_date       (RevolutionMailHeader *self,
                                            gint64                date);
void revolution_mail_header_set_addresses  (RevolutionMailHeader *self,
                                            CamelInternetAddress *from,
                                            CamelInternetAddress *to,
                                            CamelInternetAddress *cc,
                                            CamelInternetAddress *bcc);
void revolution_mail_header_clear          (RevolutionMailHeader *self);
void revolution_mail_header_set_compact    (RevolutionMailHeader *self,
                                            gboolean              compact);
void revolution_mail_header_set_collapsed  (RevolutionMailHeader *self,
                                            gboolean              collapsed,
                                            const char           *preview);

G_END_DECLS
