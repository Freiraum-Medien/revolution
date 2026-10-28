/* contacts-editor.h – Bearbeitungsformular eines Kontakts (#370)
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>
#include <libebook/libebook.h>

G_BEGIN_DECLS

#define REVOLUTION_TYPE_CONTACTS_EDITOR (revolution_contacts_editor_get_type ())
G_DECLARE_FINAL_TYPE (RevolutionContactsEditor, revolution_contacts_editor, REVOLUTION, CONTACTS_EDITOR, AdwBin)

void      revolution_contacts_editor_edit     (RevolutionContactsEditor *self,
                                               EContact                 *contact,
                                               GListModel               *books,
                                               guint                     selected);
EContact *revolution_contacts_editor_build    (RevolutionContactsEditor *self);
guint     revolution_contacts_editor_get_book (RevolutionContactsEditor *self);
gboolean  revolution_contacts_editor_is_valid (RevolutionContactsEditor *self);

G_END_DECLS
