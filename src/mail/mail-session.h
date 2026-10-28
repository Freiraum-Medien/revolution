/* mail-session.h – CamelSession der Mail (#346)
 *
 * Gegenstück zu Evolution 3.56.2 src/libemail-engine/e-mail-session.h, nur die in
 * mail-session.c übernommenen Teile.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <camel/camel.h>
#include <libedataserver/libedataserver.h>

G_BEGIN_DECLS

/* #362: lokaler Store des Postausgangs (mail-outbox.c), ohne ESource. */
#define REVOLUTION_MAIL_LOCAL_UID "revolution-local"

#define REVOLUTION_TYPE_MAIL_SESSION (revolution_mail_session_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionMailSession, revolution_mail_session, REVOLUTION, MAIL_SESSION, CamelSession)

RevolutionMailSession *revolution_mail_session_new         (ESourceRegistry       *registry);
gchar                **revolution_mail_session_take_errors (RevolutionMailSession *session);
void                   revolution_mail_session_flush_outbox (RevolutionMailSession *session);

G_END_DECLS
