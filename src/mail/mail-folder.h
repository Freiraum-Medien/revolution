/* mail-folder.h – Mail-Ordner als Eintrag der Seitenleiste (#348)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <camel/camel.h>

#include "mail-account.h"

G_BEGIN_DECLS

#define REVOLUTION_TYPE_MAIL_FOLDER (revolution_mail_folder_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionMailFolder, revolution_mail_folder, REVOLUTION, MAIL_FOLDER, GObject)

RevolutionMailFolder *revolution_mail_folder_new (const char            *full_name,
                                                  const char            *display_name,
                                                  const char            *icon_name,
                                                  RevolutionMailAccount *account);
const char *revolution_mail_folder_get_full_name (RevolutionMailFolder  *self);
GListModel *revolution_mail_folder_get_children  (RevolutionMailFolder  *self);
guint32     revolution_mail_folder_get_flags     (RevolutionMailFolder  *self);
guint       revolution_mail_folder_append_tree   (GListStore            *store,
                                                  CamelFolderInfo       *first,
                                                  RevolutionMailAccount *account);
void        revolution_mail_folder_merge_tree    (GListStore            *store,
                                                  CamelFolderInfo       *first,
                                                  RevolutionMailAccount *account,
                                                  guint                 *added,
                                                  guint                 *removed);
RevolutionMailFolder *revolution_mail_folder_find (GListModel            *model,
                                                  const char            *full_name);
void        revolution_mail_folder_watch         (RevolutionMailFolder  *self,
                                                  CamelFolder           *folder,
                                                  gboolean               hold,
                                                  gboolean               refreshed);
void        revolution_mail_folder_mark_special  (CamelStore            *store,
                                                  ESourceRegistry       *registry,
                                                  CamelFolderInfo       *info,
                                                  GCancellable          *cancellable);
void        revolution_mail_folder_forget_unread (CamelFolderInfo       *fi);
gboolean    revolution_mail_folder_create_sync   (CamelStore            *store,
                                                  const char            *parent,
                                                  const char            *name,
                                                  char                 **created,
                                                  GCancellable          *cancellable,
                                                  GError               **error);
gboolean    revolution_mail_folder_rename_sync   (CamelStore            *store,
                                                  const char            *full_name,
                                                  const char            *name,
                                                  char                 **new_full_name,
                                                  GCancellable          *cancellable,
                                                  GError               **error);
gboolean    revolution_mail_folder_delete_sync   (CamelStore            *store,
                                                  const char            *full_name,
                                                  GCancellable          *cancellable,
                                                  GError               **error);

G_END_DECLS
