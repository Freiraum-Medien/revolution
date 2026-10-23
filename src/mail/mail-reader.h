/* mail-reader.h – Lesebereich: Nachrichtenkopf und Inhalt in WebKitGTK (#354)
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

#define REVOLUTION_TYPE_MAIL_READER (revolution_mail_reader_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionMailReader, revolution_mail_reader, REVOLUTION, MAIL_READER, AdwBin)

void revolution_mail_reader_show  (RevolutionMailReader  *self,
                                   CamelSession          *session,
                                   RevolutionMailFolder  *folder,
                                   RevolutionMailMessage *message);
void revolution_mail_reader_clear (RevolutionMailReader  *self);

G_END_DECLS
