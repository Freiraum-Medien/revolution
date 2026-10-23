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

const char *revolution_mail_folder_get_full_name (RevolutionMailFolder  *self);
GListModel *revolution_mail_folder_get_children  (RevolutionMailFolder  *self);
guint32     revolution_mail_folder_get_flags     (RevolutionMailFolder  *self);
guint       revolution_mail_folder_append_tree   (GListStore            *store,
                                                  CamelFolderInfo       *first,
                                                  RevolutionMailAccount *account);

G_END_DECLS
