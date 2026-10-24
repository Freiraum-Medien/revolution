/* mail-favorites.h – Favoriten über alle Konten (#351)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <camel/camel.h>

#include "mail-account.h"
#include "mail-view.h"

G_BEGIN_DECLS

/* UID des CamelVeeStore in der Session und des Abschnitts „Favoriten“ der Seitenleiste. */
#define REVOLUTION_MAIL_FAVORITES_UID "revolution-favoriten"
/* Ausdruck-Teil: Gelöschte und Spam ausblenden (mail-favorites.c; #357 mail-thread.c). */
#define REVOLUTION_MAIL_FAVORITES_VISIBLE "(not (or (system-flag \"Deleted\") (system-flag \"Junk\")))"

gboolean revolution_mail_favorites_setup     (CamelSession          *session,
                                              GError               **error);
void     revolution_mail_favorites_fill      (GListStore            *store,
                                              RevolutionMailAccount *account);
void     revolution_mail_favorites_sync_session (CamelSession      *session);
void     revolution_mail_favorites_add_store (RevolutionMailView    *view,
                                              CamelStore            *store,
                                              CamelFolderInfo       *info,
                                              GCancellable          *cancellable);

G_END_DECLS
