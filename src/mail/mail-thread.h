/* mail-thread.h – Konversationen: Threads der Nachrichtenliste (#357)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <camel/camel.h>

G_BEGIN_DECLS

char        *revolution_mail_thread_folder_name (const char    *uid,
                                                 const char    *full_name);
CamelFolder *revolution_mail_thread_folder_sync (CamelFolder   *folder,
                                                 const char    *sent_full_name,
                                                 CamelFolder  **sent_out,
                                                 GCancellable  *cancellable,
                                                 GError       **error);
gboolean     revolution_mail_thread_refresh_sync (CamelFolder  *folder,
                                                  GCancellable *cancellable,
                                                  GError      **error);
GPtrArray   *revolution_mail_thread_build       (CamelFolder   *folder,
                                                 GPtrArray     *infos,
                                                 GDateTime     *now);

G_END_DECLS
