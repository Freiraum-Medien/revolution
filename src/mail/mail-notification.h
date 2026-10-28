/* mail-notification.h – Benachrichtigung bei neuer Mail im Posteingang (#366)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <camel/camel.h>

G_BEGIN_DECLS

void revolution_mail_notification_watch (CamelFolder *inbox);
void revolution_mail_notification_reset (void);

G_END_DECLS
