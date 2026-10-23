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

#include <string.h>

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
  gint64    timestamp;
  char     *accessible_label;
  /* #351: Konto der Nachricht im Suchordner (Favoriten), sonst NULL: voller Anzeigename, Kurzform
   * für die Zeile, UID und voller Name des Quellordners (Ort im Nachrichtenkopf, mail-view.c). */
  char     *account;
  char     *account_label;
  char     *source_uid;
  char     *source_folder;
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
  PROP_ACCOUNT,
  PROP_ACCOUNT_LABEL,
  PROP_SHOW_ACCOUNT,
  N_PROPS
};

static GParamSpec *props[N_PROPS];

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
    case PROP_ACCOUNT:          g_value_set_string (value, self->account); break;
    case PROP_ACCOUNT_LABEL:    g_value_set_string (value, self->account_label); break;
    case PROP_SHOW_ACCOUNT:     g_value_set_boolean (value, self->account != NULL); break;
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
  g_free (self->account);
  g_free (self->account_label);
  g_free (self->source_uid);
  g_free (self->source_folder);

  G_OBJECT_CLASS (revolution_mail_message_parent_class)->finalize (object);
}

static void
revolution_mail_message_class_init (RevolutionMailMessageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
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
  props[PROP_ACCOUNT] = g_param_spec_string ("account", NULL, NULL, NULL, flags);
  props[PROP_ACCOUNT_LABEL] = g_param_spec_string ("account-label", NULL, NULL, NULL, flags);
  props[PROP_SHOW_ACCOUNT] = g_param_spec_boolean ("show-account", NULL, NULL, FALSE, flags);
  g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
revolution_mail_message_init (RevolutionMailMessage *self G_GNUC_UNUSED)
{
}

/* Anzeigename des Absenders, sonst die Adresse (Prototyp: nur der Name). Name in spitzen
 * Klammern oder gleich der Adresse gilt als kein Name: microsoft365 setzt den Namen aus
 * Graph, der dort oft die Adresse ist, und baut daraus „adresse <adresse>“
 * (camel-m365-folder.c:787-800); camel_address_decode liefert dann den Namen „<adresse>“. */
static char *
sender_for (const char *from)
{
  g_autoptr (CamelInternetAddress) address = camel_internet_address_new ();
  const char *name = NULL, *email = NULL;
  g_autofree char *bare = NULL;

  if (from == NULL || camel_address_decode (CAMEL_ADDRESS (address), from) <= 0 ||
      !camel_internet_address_get (address, 0, &name, &email))
    return g_strdup (from != NULL && *from != '\0' ? from : _("Unbekannter Absender"));

  bare = g_strstrip (g_strdup (name != NULL ? name : ""));
  if (bare[0] == '<' && g_str_has_suffix (bare, ">"))
    {
      char *inner = g_strndup (bare + 1, strlen (bare) - 2);

      g_free (bare);
      bare = g_strstrip (inner);
    }

  if (email != NULL && *email != '\0' && (*bare == '\0' || g_ascii_strcasecmp (bare, email) == 0))
    return g_strdup (email);
  if (*bare != '\0')
    return g_steal_pointer (&bare);

  return g_strdup (email != NULL && *email != '\0' ? email : _("Unbekannter Absender"));
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

/* #351: Kurzform des Kontonamens für die Zeile (PM 24.10.2026: kurz, Absender nicht unnötig kürzen):
 * bei einer Adresse der Teil vor dem @, sofern kein anderes Konto der Session denselben hat, sonst
 * der volle Anzeigename. Im Worker; die Session liest nur ihre Dienste (camel_session_list_services). */
static char *
account_label_for (CamelStore *store,
                   const char *name)
{
  g_autoptr (CamelSession) session = camel_service_ref_session (CAMEL_SERVICE (store));
  g_autolist (CamelService) services = session != NULL ? camel_session_list_services (session) : NULL;
  const char *at = name != NULL ? strchr (name, '@') : NULL;
  g_autofree char *local = NULL;

  if (at == NULL || at == name)
    return g_strdup (name);

  local = g_strndup (name, at - name);
  for (GList *l = services; l != NULL; l = l->next)
    {
      g_autofree char *other = NULL;

      if (l->data == (gpointer) store || !CAMEL_IS_STORE (l->data) || CAMEL_IS_VEE_STORE (l->data))
        continue;

      other = camel_service_dup_display_name (l->data);
      if (other != NULL && g_ascii_strncasecmp (other, local, at - name) == 0 && other[at - name] == '@')
        return g_strdup (name);
    }

  return g_steal_pointer (&local);
}

/* Zugänglicher Name der Zeile (Architektur 8): Absender, Betreff, Datum, Zustände, #351 Konto. */
static char *
accessible_label_for (RevolutionMailMessage *self)
{
  g_autofree char *account = self->account != NULL ? g_strdup_printf (_(", Konto %s"), self->account) : NULL;

  return g_strdup_printf ("%s, %s, %s%s%s%s%s", self->sender, self->subject, self->date,
                          self->unread ? _(", ungelesen") : "",
                          self->starred ? _(", markiert") : "",
                          self->attachment ? _(", mit Anhang") : "",
                          account != NULL ? account : "");
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

  self->timestamp = date;
  self->date = date_for (date, now);
  self->unread = (flags & CAMEL_MESSAGE_SEEN) == 0;
  self->starred = (flags & CAMEL_MESSAGE_FLAGGED) != 0;
  self->attachment = (flags & CAMEL_MESSAGE_ATTACHMENTS) != 0;

  /* #351: Im Suchordner das Konto des Quellordners wie Evolution die Spalte „Ort“ (message-list.c:
   * 2014-2020, camel_vee_folder_get_location); Name wie der Abschnittskopf der Seitenleiste
   * (Anzeigename der ESource, an den Store gebunden, mail-session.c add_service). */
  if (CAMEL_IS_VEE_MESSAGE_INFO (info))
    {
      CamelFolder *original = camel_vee_message_info_get_original_folder (CAMEL_VEE_MESSAGE_INFO (info));
      CamelStore *store = original != NULL ? camel_folder_get_parent_store (original) : NULL;

      if (store != NULL)
        {
          self->account = camel_service_dup_display_name (CAMEL_SERVICE (store));
          self->account_label = account_label_for (store, self->account);
          self->source_uid = g_strdup (camel_service_get_uid (CAMEL_SERVICE (store)));
          self->source_folder = g_strdup (camel_folder_get_full_name (original));
        }
    }

  self->accessible_label = accessible_label_for (self);

  return self;
}

const char *
revolution_mail_message_get_uid (RevolutionMailMessage *self)
{
  g_return_val_if_fail (REVOLUTION_IS_MAIL_MESSAGE (self), NULL);

  return self->uid;
}

static gboolean
update_string (RevolutionMailMessage  *self,
               char                  **field,
               const char             *value,
               guint                   prop)
{
  if (g_strcmp0 (*field, value) == 0)
    return FALSE;

  g_free (*field);
  *field = g_strdup (value);
  g_object_notify_by_pspec (G_OBJECT (self), props[prop]);

  return TRUE;
}

static gboolean
update_boolean (RevolutionMailMessage *self,
                gboolean              *field,
                gboolean               value,
                guint                  prop)
{
  if (*field == value)
    return FALSE;

  *field = value;
  g_object_notify_by_pspec (G_OBJECT (self), props[prop]);

  return TRUE;
}

/* Im UI-Thread (Abgleich in mail-view.c, merge_messages): Felder von from (gleiche UID)
 * übernehmen, am bestehenden Eintrag, damit Auswahl und Zeile erhalten bleiben; die
 * Bindungen der Zeile (mail-view.blp) folgen über notify. Verglichen wird der Zeitstempel,
 * nicht das relative Datum (das nach Mitternacht anders lautet). TRUE bei Änderung. */
gboolean
revolution_mail_message_update (RevolutionMailMessage *self,
                                RevolutionMailMessage *from)
{
  gboolean changed = FALSE;

  g_return_val_if_fail (REVOLUTION_IS_MAIL_MESSAGE (self), FALSE);
  g_return_val_if_fail (REVOLUTION_IS_MAIL_MESSAGE (from), FALSE);

  if (self->timestamp != from->timestamp)
    {
      self->timestamp = from->timestamp;
      update_string (self, &self->date, from->date, PROP_DATE);
      changed = TRUE;
    }
  changed |= update_string (self, &self->sender, from->sender, PROP_SENDER);
  changed |= update_string (self, &self->subject, from->subject, PROP_SUBJECT);
  changed |= update_string (self, &self->preview, from->preview, PROP_PREVIEW);
  changed |= update_boolean (self, &self->unread, from->unread, PROP_UNREAD);
  changed |= update_boolean (self, &self->starred, from->starred, PROP_STARRED);
  changed |= update_boolean (self, &self->attachment, from->attachment, PROP_ATTACHMENT);
  if (changed)
    update_string (self, &self->accessible_label, from->accessible_label, PROP_ACCESSIBLE_LABEL);

  return changed;
}

/* #353: Im UI-Thread sofort nach einer Bedienung (mail-view.c, mark_message), bevor Camel die
 * Änderung über „changed“ bestätigt; so folgen Zeile, Aktionen und Zähler ohne Verzögerung, und
 * schnell nacheinander gedrückte Kürzel sehen schon den neuen Zustand. */
void
revolution_mail_message_set_flags (RevolutionMailMessage *self,
                                   gboolean               unread,
                                   gboolean               starred)
{
  gboolean changed;
  g_autofree char *label = NULL;

  g_return_if_fail (REVOLUTION_IS_MAIL_MESSAGE (self));

  changed = update_boolean (self, &self->unread, unread, PROP_UNREAD);
  changed |= update_boolean (self, &self->starred, starred, PROP_STARRED);
  if (!changed)
    return;

  label = accessible_label_for (self);
  update_string (self, &self->accessible_label, label, PROP_ACCESSIBLE_LABEL);
}

gboolean
revolution_mail_message_get_unread (RevolutionMailMessage *self)
{
  g_return_val_if_fail (REVOLUTION_IS_MAIL_MESSAGE (self), FALSE);

  return self->unread;
}

/* #353: Zustand für die Aktionen (mail-view.c, update_message_actions). */
gboolean
revolution_mail_message_get_starred (RevolutionMailMessage *self)
{
  g_return_val_if_fail (REVOLUTION_IS_MAIL_MESSAGE (self), FALSE);

  return self->starred;
}

/* #353: Sortierschlüssel (Datum) beim Einfügen neuer Nachrichten (mail-view.c, apply_folder_changes). */
gint64
revolution_mail_message_get_timestamp (RevolutionMailMessage *self)
{
  g_return_val_if_fail (REVOLUTION_IS_MAIL_MESSAGE (self), 0);

  return self->timestamp;
}

/* #351: Quellordner einer Nachricht im Suchordner (UID des Kontos, voller Name), sonst FALSE. */
gboolean
revolution_mail_message_get_source (RevolutionMailMessage  *self,
                                    const char            **uid,
                                    const char            **full_name)
{
  g_return_val_if_fail (REVOLUTION_IS_MAIL_MESSAGE (self), FALSE);

  *uid = self->source_uid;
  *full_name = self->source_folder;

  return self->source_uid != NULL && self->source_folder != NULL;
}

gboolean
revolution_mail_message_has_preview (RevolutionMailMessage *self)
{
  g_return_val_if_fail (REVOLUTION_IS_MAIL_MESSAGE (self), FALSE);

  return self->preview != NULL;
}
