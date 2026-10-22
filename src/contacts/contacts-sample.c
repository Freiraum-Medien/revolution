/* contacts-sample.c – statische Beispieldaten für den Prototyp (#338)
 *
 * Nur Befüllen: Blueprint kann keine Objekte in ein GListStore legen.
 * Ein Eintrag hat Titel und Abschnitt; Adressbücher sind nach Konto,
 * Kontakte nach Anfangsbuchstabe gruppiert. Alle Namen sind erfunden.
 * Entfällt, sobald echte Daten aus EDS kommen.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "contacts-sample.h"

typedef struct {
  const char *group;
  const char *title;
  const char *icon_name;
} SampleRow;

/* „Auf diesem Rechner“ / „Persönlich“: Namen des lokalen Adressbuchs in EDS
 * (evolution-data-server-3.56.2/po/de.po:9313, 9320). */
/* #402 Punkt 5: Gruppen/Verteilerlisten als eigener Abschnitt (Beispieldaten). */
static const SampleRow books[] = {
  { N_("Auf diesem Rechner"), N_("Persönlich"),             "x-office-address-book-symbolic" },
  { N_("Privat"),             N_("Kontakte"),               "x-office-address-book-symbolic" },
  { N_("Arbeit"),             N_("Kontakte"),               "x-office-address-book-symbolic" },
  { N_("Arbeit"),             N_("Globale Adressliste"),    "x-office-address-book-symbolic" },
  { N_("Gruppen"),            N_("Projekt Nordlicht"),      "system-users-symbolic" },
  { N_("Gruppen"),            N_("Vorstand Gartenfreunde"), "system-users-symbolic" },
};

/* Sortiert nach Vorname wie GNOME Contacts (sort-on-surname = false,
 * /usr/share/glib-2.0/schemas/org.gnome.Contacts.gschema.xml). */
static const SampleRow contacts[] = {
  { "A", N_("Anna Behrens"), NULL },
  { "A", N_("Aylin Demir"), NULL },
  { "B", N_("Bernd Hoffmann"), NULL },
  { "C", N_("Clara Neumann"), NULL },
  { "F", N_("Felix Brandt"), NULL },
  { "H", N_("Hausverwaltung Kerner"), NULL },
  { "J", N_("Jana Albrecht"), NULL },
  { "L", N_("Lukas Weber"), NULL },
  { "M", N_("Miriam Okafor"), NULL },
  { "S", N_("Sophie Lange"), NULL },
  { "T", N_("Tobias Krämer"), NULL },
  { "T", N_("Theresa Wolff"), NULL },
};

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

/* Je Abschnitt ein GListStore; das GtkFlattenListModel macht daraus
 * Abschnitte (GtkSectionModel) für die Abschnittsköpfe der GtkListView,
 * wie revolution_mail_sample_folders (mail-sample.c). */
static GListModel *
sectioned (const SampleRow *rows,
           guint            n_rows)
{
  GListStore *sections = g_list_store_new (G_TYPE_LIST_MODEL);
  g_autoptr (GListStore) section = NULL;

  for (guint i = 0; i < n_rows; i++)
    {
      g_autoptr (RevolutionContactsSampleEntry) entry = g_object_new (REVOLUTION_TYPE_CONTACTS_SAMPLE_ENTRY, NULL);

      if (i == 0 || g_strcmp0 (rows[i].group, rows[i - 1].group) != 0)
        {
          g_clear_object (&section);
          section = g_list_store_new (REVOLUTION_TYPE_CONTACTS_SAMPLE_ENTRY);
          g_list_store_append (sections, section);
        }

      entry->title = _(rows[i].title);
      entry->group = _(rows[i].group);
      entry->icon_name = rows[i].icon_name;
      g_list_store_append (section, entry);
    }

  return G_LIST_MODEL (gtk_flatten_list_model_new (G_LIST_MODEL (sections)));
}

GListModel *
revolution_contacts_sample_books (void)
{
  return sectioned (books, G_N_ELEMENTS (books));
}

GListModel *
revolution_contacts_sample_contacts (void)
{
  return sectioned (contacts, G_N_ELEMENTS (contacts));
}
