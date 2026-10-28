/* mail-search.h – Suche in der Nachrichtenliste (#365)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <camel/camel.h>

G_BEGIN_DECLS

/* Name des Suchordners „Alle Ordner“ im Suchordner-Store der Favoriten (revolution-favoriten). */
#define REVOLUTION_MAIL_SEARCH_NAME "suche"

char        *revolution_mail_search_expression  (const char    *text,
                                                 const char    *filter);
CamelFolder *revolution_mail_search_folder_sync (CamelSession  *session,
                                                 const char    *expression,
                                                 GCancellable  *cancellable,
                                                 GError       **error);
void         revolution_mail_search_clear_sync  (CamelSession  *session);

G_END_DECLS
