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

G_END_DECLS
