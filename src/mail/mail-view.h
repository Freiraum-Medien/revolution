/* mail-view.h – Mail-Ansicht (Prototyp #336)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>
#include <camel/camel.h>
#include <libedataserver/libedataserver.h>

#include "mail-folder.h"
#include "mail-message.h"

G_BEGIN_DECLS

#define REVOLUTION_TYPE_MAIL_VIEW (revolution_mail_view_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionMailView, revolution_mail_view, REVOLUTION, MAIL_VIEW, AdwBreakpointBin)

void revolution_mail_view_set_registry (RevolutionMailView *self,
                                        ESourceRegistry    *registry);
void revolution_mail_view_set_session (RevolutionMailView *self,
                                       CamelSession       *session);
void revolution_mail_view_show_error   (RevolutionMailView *self,
                                        const char         *title,
                                        const char         *detail,
                                        gboolean            per_account);
void revolution_mail_view_set_folders  (RevolutionMailView *self,
                                        const char         *uid,
                                        CamelFolderInfo    *info);
void revolution_mail_view_watch_folder (RevolutionMailView *self,
                                        CamelFolder        *folder,
                                        gboolean            hold,
                                        gboolean            refreshed);
void revolution_mail_view_set_offline  (RevolutionMailView *self,
                                        gboolean            offline);
void revolution_mail_view_update_folders (RevolutionMailView *self,
                                          const char         *uid,
                                          CamelFolderInfo    *info);
void revolution_mail_view_refresh_account (RevolutionMailView *self,
                                           const char         *uid);
RevolutionMailFolder *revolution_mail_view_find_place (RevolutionMailView    *self,
                                                       RevolutionMailMessage *message);

G_END_DECLS
