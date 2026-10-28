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

/* #416: Verschieben/Kopieren und Papierkorb mit demselben Echo-Schutz (eds-compat.c). */
gboolean     revolution_eds_folder_transfer_messages_to_sync (CamelFolder   *source,
                                                              GPtrArray     *uids,
                                                              CamelFolder   *destination,
                                                              gboolean       delete_originals,
                                                              GPtrArray    **transferred_uids,
                                                              GCancellable  *cancellable,
                                                              GError       **error);
CamelFolder *revolution_eds_store_get_trash_folder_sync      (CamelStore    *store,
                                                              GCancellable  *cancellable,
                                                              GError       **error);

/* #364: Ordner löschen/umbenennen, bei microsoft365 ohne verwaiste Summary (eds-compat.c). */
gboolean     revolution_eds_store_delete_folder_sync         (CamelStore    *store,
                                                              const char    *full_name,
                                                              GCancellable  *cancellable,
                                                              GError       **error);
gboolean     revolution_eds_store_rename_folder_sync         (CamelStore    *store,
                                                              const char    *old_name,
                                                              const char    *new_name,
                                                              GCancellable  *cancellable,
                                                              GError       **error);

/* #409: Sperre je Ordner (eds-compat.c). */
void                   revolution_eds_folder_lock           (CamelFolder           *folder);
void                   revolution_eds_folder_unlock         (CamelFolder           *folder);

/* #357: Threads (eds-compat.c). */
CamelFolderThread     *revolution_eds_thread_new            (CamelFolder           *folder,
                                                             GPtrArray             *uids);
void                   revolution_eds_thread_free           (CamelFolderThread     *thread);
CamelFolderThreadNode *revolution_eds_thread_get_tree       (CamelFolderThread     *thread);
CamelFolderThreadNode *revolution_eds_thread_node_get_next  (CamelFolderThreadNode *node);
CamelFolderThreadNode *revolution_eds_thread_node_get_child (CamelFolderThreadNode *node);
CamelMessageInfo      *revolution_eds_thread_node_get_item  (CamelFolderThreadNode *node);

G_END_DECLS
