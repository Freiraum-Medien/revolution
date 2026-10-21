/* mail-sample.c – statische Beispieldaten für den Prototyp (#336)
 *
 * Nur Befüllen: Blueprint kann weder Objekte in ein GListStore legen noch
 * die create_func des GtkTreeListModel ausdrücken. Alle Namen und Adressen
 * sind erfunden. Entfällt, sobald echte Daten aus EDS kommen.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "mail-sample.h"

/* Ordner: icon NULL beginnt eine neue Gruppe (Favoriten bzw. Konto). */
static const struct {
  const char *icon_name;
  const char *title;
  const char *unread;
  guint       depth;
} folders[] = {
  { NULL,                      N_("Favoriten"),     NULL, 0 },
  { "inbox-symbolic",          N_("Alle Eingänge"), "4",  0 },
  { "starred-symbolic",        N_("Markiert"),      "3",  0 },
  { "drafts-symbolic",         N_("Entwürfe"),      "2",  0 },
  { "mail-send-symbolic",      N_("Gesendet"),      "",   0 },
  { NULL,                      N_("Privat"),        NULL, 0 },
  { "inbox-symbolic",          N_("Eingang"),       "3",  0 },
  { "drafts-symbolic",         N_("Entwürfe"),      "1",  0 },
  { "mail-send-symbolic",      N_("Gesendet"),      "",   0 },
  { "mail-mark-junk-symbolic", N_("Spam"),          "2",  0 },
  { "user-trash-symbolic",     N_("Papierkorb"),    "",   0 },
  { "archive-symbolic",        N_("Archiv"),        "",   0 },
  { "folder-symbolic",         N_("Reisen"),        "",   0 },
  { "folder-symbolic",         N_("Lissabon"),      "1",  1 },
  { "folder-symbolic",         N_("Norwegen"),      "",   1 },
  { NULL,                      N_("Arbeit"),        NULL, 0 },
  { "inbox-symbolic",          N_("Eingang"),       "1",  0 },
  { "drafts-symbolic",         N_("Entwürfe"),      "1",  0 },
  { "mail-send-symbolic",      N_("Gesendet"),      "",   0 },
  { "mail-mark-junk-symbolic", N_("Spam"),          "",   0 },
  { "user-trash-symbolic",     N_("Papierkorb"),    "",   0 },
  { "archive-symbolic",        N_("Archiv"),        "",   0 },
  { "folder-symbolic",         N_("Projekte"),      "",   0 },
  { "folder-symbolic",         N_("Angebote"),      "2",  1 },
  { "folder-symbolic",         N_("Rechnungen"),    "",   1 },
};

static const struct {
  const char *sender;
  const char *subject;
  const char *preview;
  const char *date;
  gboolean    unread;
  gboolean    starred;
  gboolean    attachment;
} messages[] = {
  { N_("Jana Albrecht"), N_("Unterlagen für das Projekt Nordlicht"),
    N_("Hallo Lena, anbei die überarbeiteten Pläne und die Kostenschätzung. Kannst du bis Freitag einen Blick darauf werfen? Die Abstimmung mit dem Bauamt ist für nächste Woche angesetzt."),
    N_("09:14"), FALSE, TRUE, TRUE },
  { N_("Tobias Krämer"), N_("Kurze Frage zum Termin am Donnerstag"),
    N_("Passt dir 14 Uhr statt 10 Uhr? Ich hänge morgens noch in einer Besprechung fest und würde sonst nur halb vorbereitet kommen."),
    N_("08:52"), TRUE, FALSE, FALSE },
  { N_("Stadtwerke Lindenau"), N_("Ihre Jahresabrechnung 2026"),
    N_("Sehr geehrte Kundin, Ihre Jahresabrechnung steht ab sofort im Kundenportal bereit. Den Abschlag für das kommende Jahr haben wir angepasst."),
    N_("07:30"), TRUE, FALSE, TRUE },
  { N_("Miriam Okafor"), N_("Re: Fotos vom Sommerfest"),
    N_("Die Bilder sind wirklich schön geworden! Ich habe dir die Auswahl für die Vereinszeitung in den gemeinsamen Ordner gelegt."),
    N_("Gestern"), TRUE, TRUE, FALSE },
  { N_("Reisebüro Nordwind"), N_("Buchungsbestätigung Lissabon"),
    N_("Vielen Dank für Ihre Buchung. Im Anhang finden Sie Ihre Reiseunterlagen mit Flugzeiten, Hotelanschrift und den Hinweisen zur Anreise."),
    N_("Gestern"), FALSE, FALSE, TRUE },
  { N_("Dr. Felix Brandt"), N_("Protokoll der Vorstandssitzung"),
    N_("Liebe Vorstandsmitglieder, hier das Protokoll vom Montag. Bitte gebt Korrekturen bis Ende der Woche durch, danach geht es an alle Mitglieder."),
    N_("Mo."), FALSE, FALSE, TRUE },
  { N_("Sophie Lange"), N_("Abendessen am Samstag?"),
    N_("Wir haben spontan einen Tisch beim Italiener an der Ecke reserviert. Habt ihr Lust mitzukommen? Gegen 19 Uhr würde gut passen."),
    N_("Mo."), TRUE, FALSE, FALSE },
  { N_("Bücherstube am Markt"), N_("Neuerscheinungen im Oktober"),
    N_("Der Herbst ist da und mit ihm unsere Lieblingsromane der Saison. Dazu eine Lesung am 30. Oktober, zu der wir Sie herzlich einladen."),
    N_("So."), FALSE, FALSE, FALSE },
  { N_("Hausverwaltung Kerner"), N_("Ablesung der Heizkörper"),
    N_("Am 4. November werden die Heizkostenverteiler abgelesen. Bitte sorgen Sie dafür, dass alle Räume zwischen 8 und 12 Uhr zugänglich sind."),
    N_("17.10.26"), FALSE, TRUE, FALSE },
  { N_("Lukas Weber"), N_("Angebot Webseite Gartenfreunde"),
    N_("Wie besprochen schicke ich dir das Angebot für die neue Vereinsseite. Die Pflege durch den Vorstand ist im Preis enthalten."),
    N_("15.10.26"), FALSE, FALSE, TRUE },
};

struct _RevolutionSampleFolder
{
  GObject     parent_instance;
  const char *title;
  const char *icon_name;
  const char *unread;
  const char *group;
  GListStore *children;
};

G_DEFINE_FINAL_TYPE (RevolutionSampleFolder, revolution_sample_folder, G_TYPE_OBJECT)

enum { FOLDER_PROP_0, FOLDER_PROP_TITLE, FOLDER_PROP_ICON_NAME, FOLDER_PROP_UNREAD, FOLDER_PROP_GROUP, FOLDER_N_PROPS };

static void
revolution_sample_folder_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  RevolutionSampleFolder *self = REVOLUTION_SAMPLE_FOLDER (object);

  switch (prop_id)
    {
    case FOLDER_PROP_TITLE:     g_value_set_static_string (value, self->title); break;
    case FOLDER_PROP_ICON_NAME: g_value_set_static_string (value, self->icon_name); break;
    case FOLDER_PROP_UNREAD:    g_value_set_static_string (value, self->unread); break;
    case FOLDER_PROP_GROUP:     g_value_set_static_string (value, self->group); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
revolution_sample_folder_finalize (GObject *object)
{
  g_clear_object (&REVOLUTION_SAMPLE_FOLDER (object)->children);

  G_OBJECT_CLASS (revolution_sample_folder_parent_class)->finalize (object);
}

static void
revolution_sample_folder_class_init (RevolutionSampleFolderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamSpec *props[FOLDER_N_PROPS] = { NULL, };
  GParamFlags flags = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;

  object_class->get_property = revolution_sample_folder_get_property;
  object_class->finalize = revolution_sample_folder_finalize;

  props[FOLDER_PROP_TITLE] = g_param_spec_string ("title", NULL, NULL, NULL, flags);
  props[FOLDER_PROP_ICON_NAME] = g_param_spec_string ("icon-name", NULL, NULL, NULL, flags);
  props[FOLDER_PROP_UNREAD] = g_param_spec_string ("unread", NULL, NULL, NULL, flags);
  props[FOLDER_PROP_GROUP] = g_param_spec_string ("group", NULL, NULL, NULL, flags);
  g_object_class_install_properties (object_class, FOLDER_N_PROPS, props);
}

static void
revolution_sample_folder_init (G_GNUC_UNUSED RevolutionSampleFolder *self)
{
}

struct _RevolutionSampleMessage
{
  GObject     parent_instance;
  const char *sender;
  const char *subject;
  const char *preview;
  const char *date;
  gboolean    unread;
  gboolean    starred;
  gboolean    attachment;
};

G_DEFINE_FINAL_TYPE (RevolutionSampleMessage, revolution_sample_message, G_TYPE_OBJECT)

enum { MSG_PROP_0, MSG_PROP_SENDER, MSG_PROP_SUBJECT, MSG_PROP_PREVIEW, MSG_PROP_DATE, MSG_PROP_UNREAD, MSG_PROP_STARRED, MSG_PROP_ATTACHMENT, MSG_N_PROPS };

static void
revolution_sample_message_get_property (GObject    *object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  RevolutionSampleMessage *self = REVOLUTION_SAMPLE_MESSAGE (object);

  switch (prop_id)
    {
    case MSG_PROP_SENDER:     g_value_set_static_string (value, self->sender); break;
    case MSG_PROP_SUBJECT:    g_value_set_static_string (value, self->subject); break;
    case MSG_PROP_PREVIEW:    g_value_set_static_string (value, self->preview); break;
    case MSG_PROP_DATE:       g_value_set_static_string (value, self->date); break;
    case MSG_PROP_UNREAD:     g_value_set_boolean (value, self->unread); break;
    case MSG_PROP_STARRED:    g_value_set_boolean (value, self->starred); break;
    case MSG_PROP_ATTACHMENT: g_value_set_boolean (value, self->attachment); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
revolution_sample_message_class_init (RevolutionSampleMessageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamSpec *props[MSG_N_PROPS] = { NULL, };
  GParamFlags flags = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;

  object_class->get_property = revolution_sample_message_get_property;

  props[MSG_PROP_SENDER] = g_param_spec_string ("sender", NULL, NULL, NULL, flags);
  props[MSG_PROP_SUBJECT] = g_param_spec_string ("subject", NULL, NULL, NULL, flags);
  props[MSG_PROP_PREVIEW] = g_param_spec_string ("preview", NULL, NULL, NULL, flags);
  props[MSG_PROP_DATE] = g_param_spec_string ("date", NULL, NULL, NULL, flags);
  props[MSG_PROP_UNREAD] = g_param_spec_boolean ("unread", NULL, NULL, FALSE, flags);
  props[MSG_PROP_STARRED] = g_param_spec_boolean ("starred", NULL, NULL, FALSE, flags);
  props[MSG_PROP_ATTACHMENT] = g_param_spec_boolean ("attachment", NULL, NULL, FALSE, flags);
  g_object_class_install_properties (object_class, MSG_N_PROPS, props);
}

static void
revolution_sample_message_init (G_GNUC_UNUSED RevolutionSampleMessage *self)
{
}

/* create_func des GtkTreeListModel: Unterordner oder NULL (kein Aufklapper). */
static GListModel *
folder_children (gpointer item,
                 G_GNUC_UNUSED gpointer user_data)
{
  RevolutionSampleFolder *folder = item;

  return folder->children ? g_object_ref (G_LIST_MODEL (folder->children)) : NULL;
}

static void
append_group (GListStore *groups,
              GListStore *group)
{
  g_autoptr (GtkTreeListModel) tree = NULL;

  if (group == NULL)
    return;

  tree = gtk_tree_list_model_new (G_LIST_MODEL (group), FALSE, TRUE, folder_children, NULL, NULL);
  g_list_store_append (groups, tree);
}

/* Je Gruppe ein GtkTreeListModel; das GtkFlattenListModel macht daraus
 * Abschnitte (GtkSectionModel) für die Abschnittsköpfe der GtkListView. */
GListModel *
revolution_mail_sample_folders (void)
{
  GListStore *groups = g_list_store_new (GTK_TYPE_TREE_LIST_MODEL);
  GListStore *group = NULL;
  RevolutionSampleFolder *parent = NULL;
  const char *group_name = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (folders); i++)
    {
      RevolutionSampleFolder *folder;

      if (folders[i].icon_name == NULL)
        {
          append_group (groups, group);
          group = g_list_store_new (REVOLUTION_TYPE_SAMPLE_FOLDER);
          group_name = _(folders[i].title);
          continue;
        }

      folder = g_object_new (REVOLUTION_TYPE_SAMPLE_FOLDER, NULL);
      folder->title = _(folders[i].title);
      folder->icon_name = folders[i].icon_name;
      folder->unread = folders[i].unread;
      folder->group = group_name;

      if (folders[i].depth == 0)
        {
          g_list_store_append (group, folder);
          parent = folder;
        }
      else
        {
          if (parent->children == NULL)
            parent->children = g_list_store_new (REVOLUTION_TYPE_SAMPLE_FOLDER);
          g_list_store_append (parent->children, folder);
        }

      g_object_unref (folder);
    }
  append_group (groups, group);

  return G_LIST_MODEL (gtk_flatten_list_model_new (G_LIST_MODEL (groups)));
}

GListModel *
revolution_mail_sample_messages (void)
{
  GListStore *store = g_list_store_new (REVOLUTION_TYPE_SAMPLE_MESSAGE);

  for (guint i = 0; i < G_N_ELEMENTS (messages); i++)
    {
      RevolutionSampleMessage *message = g_object_new (REVOLUTION_TYPE_SAMPLE_MESSAGE, NULL);

      message->sender = _(messages[i].sender);
      message->subject = _(messages[i].subject);
      message->preview = _(messages[i].preview);
      message->date = _(messages[i].date);
      message->unread = messages[i].unread;
      message->starred = messages[i].starred;
      message->attachment = messages[i].attachment;
      g_list_store_append (store, message);
      g_object_unref (message);
    }

  return G_LIST_MODEL (store);
}
