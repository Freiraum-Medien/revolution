/* mail-conversation.h – Lesebereich einer Konversation (#357)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>
#include <camel/camel.h>

#include "mail-folder.h"
#include "mail-message.h"

G_BEGIN_DECLS

#define REVOLUTION_TYPE_MAIL_CONVERSATION (revolution_mail_conversation_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionMailConversation, revolution_mail_conversation, REVOLUTION, MAIL_CONVERSATION, AdwBin)

void revolution_mail_conversation_show  (RevolutionMailConversation *self,
                                         CamelSession               *session,
                                         RevolutionMailFolder       *folder,
                                         RevolutionMailMessage      *conversation);
void revolution_mail_conversation_clear (RevolutionMailConversation *self);

G_END_DECLS
