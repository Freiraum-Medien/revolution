/* contacts-book.c – Adressbuch in der Seitenleiste der Kontakte (#368)
 *
 * Ein Eintrag je aktivierter ESource mit E_SOURCE_EXTENSION_ADDRESS_BOOK; Abschnitt ist das Konto
 * (Eltern-Source, bei GOA die Sammlung, lokal „Auf diesem Rechner“), wie Evolutions Adressbuch-Auswahl
 * (e-addressbook-selector.c, Gruppierung über e_source_registry_build_display_tree nach Eltern-Source).
 * Vorlage: quelltext/stamp/src/views/contact/book-list/stamp-book-item.c (ESource/EClient im Eintrag).
 * „Nur lesend“ meldet erst der verbundene EBookClient (e_client_is_readonly), daher nachträglich gesetzt.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "contacts-book.h"

struct _RevolutionContactsBook
{
  GObject  parent_instance;
  ESource *source;
  char    *group;
  char    *section;
  gboolean readonly;
};

G_DEFINE_FINAL_TYPE (RevolutionContactsBook, revolution_contacts_book, G_TYPE_OBJECT)

enum { PROP_0, PROP_TITLE, PROP_GROUP, PROP_SECTION, PROP_READONLY, N_PROPS };

static GParamSpec *props[N_PROPS];

static void
revolution_contacts_book_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  RevolutionContactsBook *self = REVOLUTION_CONTACTS_BOOK (object);

  switch (prop_id)
    {
    case PROP_TITLE: g_value_set_string (value, e_source_get_display_name (self->source)); break;
    case PROP_GROUP: g_value_set_string (value, self->group); break;
    case PROP_SECTION: g_value_set_string (value, self->section); break;
    case PROP_READONLY: g_value_set_boolean (value, self->readonly); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
revolution_contacts_book_finalize (GObject *object)
{
  RevolutionContactsBook *self = REVOLUTION_CONTACTS_BOOK (object);

  g_clear_object (&self->source);
  g_free (self->group);
  g_free (self->section);

  G_OBJECT_CLASS (revolution_contacts_book_parent_class)->finalize (object);
}

static void
revolution_contacts_book_class_init (RevolutionContactsBookClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamFlags flags = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY;

  object_class->get_property = revolution_contacts_book_get_property;
  object_class->finalize = revolution_contacts_book_finalize;

  props[PROP_TITLE] = g_param_spec_string ("title", NULL, NULL, NULL, flags);
  props[PROP_GROUP] = g_param_spec_string ("group", NULL, NULL, NULL, flags);
  /* Sortierschlüssel der Abschnitte: „Auf diesem Rechner“ zuerst (Prototyp kontakte-hell.png), dann Konten. */
  props[PROP_SECTION] = g_param_spec_string ("section", NULL, NULL, NULL, flags);
  props[PROP_READONLY] = g_param_spec_boolean ("readonly", NULL, NULL, FALSE, flags);
  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
revolution_contacts_book_init (RevolutionContactsBook *self G_GNUC_UNUSED)
{
}

RevolutionContactsBook *
revolution_contacts_book_new (ESourceRegistry *registry,
                              ESource         *source)
{
  RevolutionContactsBook *self = g_object_new (REVOLUTION_TYPE_CONTACTS_BOOK, NULL);
  const char *parent_uid = e_source_get_parent (source);
  g_autoptr (ESource) parent = parent_uid != NULL ? e_source_registry_ref_source (registry, parent_uid) : NULL;

  self->source = g_object_ref (source);
  self->group = g_strdup (e_source_get_display_name (parent != NULL ? parent : source));
  self->section = g_strdup_printf ("%d%s/%s", g_strcmp0 (parent_uid, "local-stub") == 0 ? 0 : 1, self->group,
                                   parent_uid != NULL ? parent_uid : "");
  return self;
}

ESource *
revolution_contacts_book_get_source (RevolutionContactsBook *self)
{
  return self->source;
}

const char *
revolution_contacts_book_get_group (RevolutionContactsBook *self)
{
  return self->group;
}

void
revolution_contacts_book_set_readonly (RevolutionContactsBook *self,
                                       gboolean                readonly)
{
  if (self->readonly == readonly)
    return;
  self->readonly = readonly;
  g_object_notify_by_pspec (G_OBJECT (self), props[PROP_READONLY]);
}
