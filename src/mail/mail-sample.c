/* mail-sample.c – Listeneinträge des Prototyps (#336): Ordner und Nachricht
 *
 * Die Blueprint-Zeilenvorlagen (mail-view.blp) binden an diese Eigenschaften.
 * Die statischen Beispieldaten sind entfernt (#342, Stand: Tag m2-prototyp);
 * echte Einträge folgen mit #343, #348 und #352.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "mail-sample.h"

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
