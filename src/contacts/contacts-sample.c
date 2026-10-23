/* contacts-sample.c – Listeneintrag des Prototyps (#338): Adressbuch oder Kontakt
 *
 * Die Blueprint-Zeilenvorlagen (contacts-view.blp) binden an diese Eigenschaften.
 * Die statischen Beispieldaten sind entfernt (#342, Stand: Tag m2-prototyp);
 * echte Einträge folgen in M6.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "contacts-sample.h"

struct _RevolutionContactsSampleEntry
{
  GObject     parent_instance;
  const char *title;
  const char *group;
  const char *icon_name;
};

G_DEFINE_FINAL_TYPE (RevolutionContactsSampleEntry, revolution_contacts_sample_entry, G_TYPE_OBJECT)

enum { PROP_0, PROP_TITLE, PROP_GROUP, PROP_ICON_NAME, N_PROPS };

static void
revolution_contacts_sample_entry_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  RevolutionContactsSampleEntry *self = REVOLUTION_CONTACTS_SAMPLE_ENTRY (object);

  switch (prop_id)
    {
    case PROP_TITLE: g_value_set_static_string (value, self->title); break;
    case PROP_GROUP: g_value_set_static_string (value, self->group); break;
    case PROP_ICON_NAME: g_value_set_static_string (value, self->icon_name); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
revolution_contacts_sample_entry_class_init (RevolutionContactsSampleEntryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamSpec *props[N_PROPS] = { NULL, };
  GParamFlags flags = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;

  object_class->get_property = revolution_contacts_sample_entry_get_property;

  props[PROP_TITLE] = g_param_spec_string ("title", NULL, NULL, NULL, flags);
  props[PROP_GROUP] = g_param_spec_string ("group", NULL, NULL, NULL, flags);
  props[PROP_ICON_NAME] = g_param_spec_string ("icon-name", NULL, NULL, NULL, flags);
  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
revolution_contacts_sample_entry_init (G_GNUC_UNUSED RevolutionContactsSampleEntry *self)
{
}
