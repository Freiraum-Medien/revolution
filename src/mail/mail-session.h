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

#define REVOLUTION_TYPE_MAIL_SESSION (revolution_mail_session_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionMailSession, revolution_mail_session, REVOLUTION, MAIL_SESSION, CamelSession)

RevolutionMailSession *revolution_mail_session_new (ESourceRegistry *registry);

G_END_DECLS
