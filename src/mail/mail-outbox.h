/* mail-outbox.h – Postausgang und Senden (#362)
 *
 * Gegenstück zu Evolution 3.56.2 src/libemail-engine (e-mail-session.h, e-mail-session-utils.h,
 * mail-ops.h), nur die in mail-outbox.c übernommenen Teile. Ohne GTK.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <camel/camel.h>

G_BEGIN_DECLS

/* Ergebnis eines Sendelaufs (revolution_mail_outbox_flush_sync), für das Banner in main.c. */
typedef struct
{
  guint sent;
  guint left;
  /* Je Versanddienst mit Fehler: UID des Dienstes, Banner-Titel (statisch), Einzelheiten. */
  GPtrArray *failed_uids;
  GPtrArray *failed_titles;
  GPtrArray *failed_details;
  /* Versanddienste, über die in diesem Lauf etwas ohne Fehler hinausging. */
  GPtrArray *sent_uids;
} RevolutionMailOutboxFlush;

gboolean    revolution_mail_outbox_setup         (CamelSession         *session,
                                                  GError              **error);
CamelFolder *revolution_mail_outbox_ref_folder   (CamelSession         *session);
RevolutionMailOutboxFlush *revolution_mail_outbox_flush_sync (CamelSession *session,
                                                              gboolean      all,
                                                              GCancellable *cancellable);
void        revolution_mail_outbox_flush_free    (RevolutionMailOutboxFlush *flush);

void        revolution_mail_send_message         (CamelMimeMessage     *message,
                                                  const char           *identity_uid,
                                                  GCancellable         *cancellable,
                                                  GAsyncReadyCallback   callback,
                                                  gpointer              user_data);
gboolean    revolution_mail_send_message_finish  (GAsyncResult         *result,
                                                  GError              **error);

G_END_DECLS
