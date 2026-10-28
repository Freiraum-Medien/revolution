/* mail-account.h – Mail-Konto als Eintrag der Seitenleiste (#345)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>
#include <libedataserver/libedataserver.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_MAIL_ACCOUNT (revolution_mail_account_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionMailAccount, revolution_mail_account, REVOLUTION, MAIL_ACCOUNT, GObject)

RevolutionMailAccount *revolution_mail_account_new              (const char            *uid,
                                                                 const char            *display_name);
const char            *revolution_mail_account_get_uid          (RevolutionMailAccount *self);
void                   revolution_mail_account_set_display_name (RevolutionMailAccount *self,
                                                                 const char            *display_name);
const char            *revolution_mail_account_hidden_reason    (ESourceRegistry       *registry,
                                                                 ESource               *source);
void                   revolution_mail_account_update_note      (RevolutionMailAccount *self,
                                                                 ESource               *source);

G_END_DECLS
