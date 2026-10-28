/* composer.h – Composer-Fenster: Neu, Antworten, Allen antworten (#337, #359), Weiterleiten, Entwurf (#360)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>
#include <camel/camel.h>
#include <libedataserver/libedataserver.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_COMPOSER (revolution_composer_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionComposer, revolution_composer, REVOLUTION, COMPOSER, AdwApplicationWindow)

/* Typen wie Stamp (stamp-composer.h:33-39); #360: Weiterleiten und Entwurf weiter bearbeiten. */
typedef enum
{
  REVOLUTION_COMPOSER_NEW,
  REVOLUTION_COMPOSER_REPLY,
  REVOLUTION_COMPOSER_REPLY_ALL,
  REVOLUTION_COMPOSER_FORWARD,
  REVOLUTION_COMPOSER_DRAFT,
} RevolutionComposerType;

GtkWindow *revolution_composer_new (GtkApplication         *application,
                                    ESourceRegistry        *registry,
                                    CamelSession           *session,
                                    RevolutionComposerType  type,
                                    const char             *account_uid,
                                    const char             *store_uid,
                                    const char             *folder_name,
                                    const char             *message_uid);
GtkWindow *revolution_composer_new_mailto (GtkApplication  *application,
                                           ESourceRegistry *registry,
                                           CamelSession    *session,
                                           const char      *mailto);

/* #360, Haken für #362 (Senden): Die gebaute Nachricht trägt wie bei Evolution X-Evolution-Identity (UID der
 * gewählten Identität, Wahl des Transports) und nach einem gespeicherten Entwurf X-Evolution-Draft-Folder/
 * -Message. Nach erfolgreichem Versand im Worker aufrufen (Evolution mail-ops.c:845): markiert den Entwurf
 * als gelöscht. Vor dem Versand X-Evolution-* entfernen (Evolution mail_tool_remove_xevolution_headers). */
gboolean revolution_composer_handle_draft_headers_sync (CamelSession      *session,
                                                        CamelMimeMessage  *message,
                                                        GCancellable      *cancellable,
                                                        GError           **error);

G_END_DECLS
