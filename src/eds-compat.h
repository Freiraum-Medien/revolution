/* eds-compat.h – versionsabhängige Camel-Aufrufe (Architektur-Festlegung 6)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <camel/camel.h>

G_BEGIN_DECLS

gboolean revolution_eds_folder_refresh_info_sync (CamelFolder   *folder,
                                                  GCancellable  *cancellable,
                                                  GError       **error);

/* #357: Threads (eds-compat.c). */
CamelFolderThread     *revolution_eds_thread_new            (CamelFolder           *folder,
                                                             GPtrArray             *uids);
void                   revolution_eds_thread_free           (CamelFolderThread     *thread);
CamelFolderThreadNode *revolution_eds_thread_get_tree       (CamelFolderThread     *thread);
CamelFolderThreadNode *revolution_eds_thread_node_get_next  (CamelFolderThreadNode *node);
CamelFolderThreadNode *revolution_eds_thread_node_get_child (CamelFolderThreadNode *node);
CamelMessageInfo      *revolution_eds_thread_node_get_item  (CamelFolderThreadNode *node);

G_END_DECLS
