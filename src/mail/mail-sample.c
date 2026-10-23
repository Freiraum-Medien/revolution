/* mail-sample.c – Listeneintrag des Prototyps (#336): Nachricht
 *
 * Die Blueprint-Zeilenvorlage (mail-view.blp) bindet an diese Eigenschaften.
 * Die statischen Beispieldaten sind entfernt (#342, Stand: Tag m2-prototyp);
 * echte Einträge folgen mit #352. Den Ordner-Eintrag ersetzt seit #345 das
 * Konto (mail-account.c); Ordner folgen mit #348.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "mail-sample.h"

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
