/* contact-completion.h – Adressvervollständigung in An/Kopie/Blindkopie (#361)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>
#include <libedataserver/libedataserver.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_CONTACT_COMPLETION (revolution_contact_completion_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionContactCompletion, revolution_contact_completion, REVOLUTION, CONTACT_COMPLETION, GObject)

/* Verbindet im Worker alle aktivierten Adressbücher mit „Autovervollständigen“. Vor dem Ende des Composers
 * g_object_run_dispose (bricht ab, löst das Popover vom Feld). */
RevolutionContactCompletion *revolution_contact_completion_new       (ESourceRegistry             *registry);
void                         revolution_contact_completion_add_entry (RevolutionContactCompletion *self,
                                                                      GtkEntry                    *entry);

G_END_DECLS
