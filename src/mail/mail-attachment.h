/* mail-attachment.h – Anhänge einer Nachricht: Eintrag und Anhangleiste (#356)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>
#include <camel/camel.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_MAIL_ATTACHMENT (revolution_mail_attachment_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionMailAttachment, revolution_mail_attachment, REVOLUTION, MAIL_ATTACHMENT, GObject)

RevolutionMailAttachment *revolution_mail_attachment_new (CamelMimePart *part,
                                                          GCancellable  *cancellable);

#define REVOLUTION_TYPE_MAIL_ATTACHMENT_BAR (revolution_mail_attachment_bar_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionMailAttachmentBar, revolution_mail_attachment_bar, REVOLUTION, MAIL_ATTACHMENT_BAR, AdwBin)

void revolution_mail_attachment_bar_set (RevolutionMailAttachmentBar *self,
                                         GPtrArray                   *attachments);

G_END_DECLS
