/* mail-message.c – Nachricht als Eintrag der Nachrichtenliste (#352)
 *
 * Ersetzt den Listeneintrag des Prototyps (mail-sample.c, #336). Kopie der Felder aus der
 * CamelMessageInfo, angelegt im Worker (mail-view.c, load_messages_thread) und danach
 * unverändert; die Blueprint-Zeilenvorlage (mail-view.blp) bindet an die Eigenschaften.
 * Felder wie Evolution (message-list.c:1833-1866, COL_ATTACHMENT, COL_FROM, COL_SUBJECT, COL_SENT)
 * und Stamp (stamp-conversation-item.c:579-598, Vorschau aus camel_message_info_get_preview).
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>

#include "mail-message.h"

struct _RevolutionMailMessage
{
  GObject   parent_instance;
  char     *uid;
  char     *sender;
  char     *subject;
  char     *preview;
  char     *date;
  char     *accessible_label;
  gboolean  unread;
  gboolean  starred;
  gboolean  attachment;
};

G_DEFINE_FINAL_TYPE (RevolutionMailMessage, revolution_mail_message, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_SENDER,
  PROP_SUBJECT,
  PROP_PREVIEW,
  PROP_DATE,
  PROP_ACCESSIBLE_LABEL,
  PROP_UNREAD,
  PROP_STARRED,
  PROP_ATTACHMENT,
  N_PROPS
};

static void
revolution_mail_message_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  RevolutionMailMessage *self = REVOLUTION_MAIL_MESSAGE (object);

  switch (prop_id)
    {
    case PROP_SENDER:           g_value_set_string (value, self->sender); break;
    case PROP_SUBJECT:          g_value_set_string (value, self->subject); break;
    case PROP_PREVIEW:          g_value_set_string (value, self->preview); break;
    case PROP_DATE:             g_value_set_string (value, self->date); break;
    case PROP_ACCESSIBLE_LABEL: g_value_set_string (value, self->accessible_label); break;
    case PROP_UNREAD:           g_value_set_boolean (value, self->unread); break;
    case PROP_STARRED:          g_value_set_boolean (value, self->starred); break;
    case PROP_ATTACHMENT:       g_value_set_boolean (value, self->attachment); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
revolution_mail_message_finalize (GObject *object)
{
  RevolutionMailMessage *self = REVOLUTION_MAIL_MESSAGE (object);

  g_free (self->uid);
  g_free (self->sender);
  g_free (self->subject);
  g_free (self->preview);
  g_free (self->date);
  g_free (self->accessible_label);

  G_OBJECT_CLASS (revolution_mail_message_parent_class)->finalize (object);
}

static void
revolution_mail_message_class_init (RevolutionMailMessageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamSpec *props[N_PROPS] = { NULL, };
  GParamFlags flags = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;

  object_class->get_property = revolution_mail_message_get_property;
  object_class->finalize = revolution_mail_message_finalize;

  props[PROP_SENDER] = g_param_spec_string ("sender", NULL, NULL, NULL, flags);
  props[PROP_SUBJECT] = g_param_spec_string ("subject", NULL, NULL, NULL, flags);
  props[PROP_PREVIEW] = g_param_spec_string ("preview", NULL, NULL, NULL, flags);
  props[PROP_DATE] = g_param_spec_string ("date", NULL, NULL, NULL, flags);
  props[PROP_ACCESSIBLE_LABEL] = g_param_spec_string ("accessible-label", NULL, NULL, NULL, flags);
  props[PROP_UNREAD] = g_param_spec_boolean ("unread", NULL, NULL, FALSE, flags);
  props[PROP_STARRED] = g_param_spec_boolean ("starred", NULL, NULL, FALSE, flags);
  props[PROP_ATTACHMENT] = g_param_spec_boolean ("attachment", NULL, NULL, FALSE, flags);
  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
revolution_mail_message_init (RevolutionMailMessage *self G_GNUC_UNUSED)
{
}

/* Anzeigename des Absenders, sonst die Adresse (Prototyp: nur der Name). */
static char *
sender_for (const char *from)
{
  g_autoptr (CamelInternetAddress) address = camel_internet_address_new ();
  const char *name = NULL, *email = NULL;

  if (from != NULL && camel_address_decode (CAMEL_ADDRESS (address), from) > 0 &&
      camel_internet_address_get (address, 0, &name, &email))
    return g_strdup (name != NULL && *name != '\0' ? name : email);

  return g_strdup (from != NULL && *from != '\0' ? from : _("Unbekannter Absender"));
}

/* Datum relativ wie der abgenommene Prototyp (Dokument #5, Design/Prototyp/mail-hell.png):
 * heute die Uhrzeit, gestern „Gestern“, bis sechs Tage zurück der Wochentag, sonst das Datum.
 * Stichtag ist der Zeitpunkt des Ladens. */
static char *
date_for (gint64     date,
          GDateTime *now)
{
  g_autoptr (GDateTime) local = NULL;
  g_autoptr (GDate) day = NULL;
  g_autoptr (GDate) today = NULL;
  int days;

  if (date <= 0)
    return g_strdup ("");

  local = g_date_time_new_from_unix_local (date);
  if (local == NULL)
    return g_strdup ("");

  day = g_date_new_dmy (g_date_time_get_day_of_month (local), g_date_time_get_month (local),
                        g_date_time_get_year (local));
  today = g_date_new_dmy (g_date_time_get_day_of_month (now), g_date_time_get_month (now),
                          g_date_time_get_year (now));
  days = g_date_days_between (day, today);

  if (days == 0)
    /* Translators: Uhrzeit heutiger Nachrichten in der Liste (g_date_time_format). */
    return g_date_time_format (local, _("%H:%M"));
  if (days == 1)
    return g_strdup (_("Gestern"));
  if (days > 1 && days < 7)
    /* Translators: Wochentag (abgekürzt) für Nachrichten der letzten Woche, z. B. „Mo.“. */
    return g_date_time_format (local, _("%a."));

  /* Translators: Datum älterer Nachrichten in der Liste, z. B. „17.10.26“. */
  return g_date_time_format (local, _("%d.%m.%y"));
}

/* Vorschau einzeilig für die zwei Zeilen der Liste: Zeilenumbrüche und Tabulatoren als
 * Leerzeichen. NULL ohne Vorschau. */
static char *
preview_for (const char *preview)
{
  char *text;

  if (preview == NULL)
    return NULL;

  text = g_strstrip (g_strdelimit (g_strdup (preview), "\r\n\t", ' '));
  if (*text == '\0')
    g_clear_pointer (&text, g_free);

  return text;
}

/* Im Worker: Kopie der Felder aus info. now ist der Stichtag fürs relative Datum. */
RevolutionMailMessage *
revolution_mail_message_new (CamelMessageInfo *info,
                             GDateTime        *now)
{
  RevolutionMailMessage *self = g_object_new (REVOLUTION_TYPE_MAIL_MESSAGE, NULL);
  const char *subject;
  gint64 date;
  guint32 flags;

  camel_message_info_property_lock (info);
  self->uid = g_strdup (camel_message_info_get_uid (info));
  self->sender = sender_for (camel_message_info_get_from (info));
  subject = camel_message_info_get_subject (info);
  self->subject = g_strdup (subject != NULL && *subject != '\0' ? subject : _("(Kein Betreff)"));
  self->preview = preview_for (camel_message_info_get_preview (info));
  /* Datum wie Evolution COL_SENT (message-list.c:1854-1866), ohne Absendedatum der Empfang. */
  date = camel_message_info_get_date_sent (info);
  if (date <= 0)
    date = camel_message_info_get_date_received (info);
  flags = camel_message_info_get_flags (info);
  camel_message_info_property_unlock (info);

  self->date = date_for (date, now);
  self->unread = (flags & CAMEL_MESSAGE_SEEN) == 0;
  self->starred = (flags & CAMEL_MESSAGE_FLAGGED) != 0;
  self->attachment = (flags & CAMEL_MESSAGE_ATTACHMENTS) != 0;

  /* Zugänglicher Name der Zeile (Architektur 8): Absender, Betreff, Datum, ungelesen. */
  self->accessible_label = g_strdup_printf ("%s, %s, %s%s", self->sender, self->subject, self->date,
                                            self->unread ? _(", ungelesen") : "");

  return self;
}

gboolean
revolution_mail_message_get_unread (RevolutionMailMessage *self)
{
  g_return_val_if_fail (REVOLUTION_IS_MAIL_MESSAGE (self), FALSE);

  return self->unread;
}

gboolean
revolution_mail_message_has_preview (RevolutionMailMessage *self)
{
  g_return_val_if_fail (REVOLUTION_IS_MAIL_MESSAGE (self), FALSE);

  return self->preview != NULL;
}
