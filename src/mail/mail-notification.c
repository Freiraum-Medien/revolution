/* mail-notification.c – Benachrichtigung bei neuer Mail im Posteingang (#366)
 *
 * Eine GNotification für alle Posteingänge wie Evolutions Plugin mail-notification: eine
 * Benachrichtigung, deren Zähler (status_count) weiterläuft, bis sie geöffnet wird
 * (plugins/mail-notification/mail-notification.c:329-357, 509-630), „(und %d weitere)“ (dort 551-561).
 * Inhalt wie Stamp (stamp-folder-item.c:206-233): Absender als Titel, Betreff als Text, Klick öffnet
 * die Nachricht (Aktion app.show-message mit Ordner-URI und UID wie Evolutions NotifyDefaultActionData,
 * dort 383-407, 409-476).
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>
#include <gio/gio.h>

#include "mail-notification.h"

/* Neue Nachrichten seit dem letzten Öffnen der Benachrichtigung (Evolution status_count). */
static guint pending;

typedef struct
{
  CamelFolder *folder;
  GPtrArray *uids;
  time_t     latest;
  guint      count;
  char      *uid;
  char      *sender;
  char      *subject;
} Batch;

static void
batch_free (Batch *batch)
{
  g_object_unref (batch->folder);
  g_ptr_array_unref (batch->uids);
  g_free (batch->uid);
  g_free (batch->sender);
  g_free (batch->subject);
  g_free (batch);
}

/* Im Worker: neue Nachrichten zählen wie Evolution folder_cache_process_folder_changes_thread
 * (libemail-engine/mail-folder-cache.c:1124-1160): nicht gelesen, kein Spam, nicht gelöscht und nicht
 * früher empfangen als die zuletzt gemeldete dieses Ordners (Evolution: später; das Empfangsdatum hat
 * Sekunden, eine zweite Mail derselben Sekunde in einem eigenen „changed“ fiele sonst weg). Angezeigt
 * wird die neueste. */
static void
check_thread (GTask        *task,
              gpointer      source_object,
              gpointer      task_data,
              GCancellable *cancellable G_GNUC_UNUSED)
{
  Batch *batch = task_data;
  time_t newest = batch->latest;

  for (guint i = 0; i < batch->uids->len; i++)
    {
      g_autoptr (CamelMessageInfo) info = camel_folder_get_message_info (source_object, batch->uids->pdata[i]);
      g_autoptr (CamelInternetAddress) from = camel_internet_address_new ();
      const char *name = NULL, *address = NULL;
      time_t received;

      if (info == NULL ||
          (camel_message_info_get_flags (info) & (CAMEL_MESSAGE_SEEN | CAMEL_MESSAGE_JUNK | CAMEL_MESSAGE_DELETED)) != 0 ||
          (received = camel_message_info_get_date_received (info)) < batch->latest)
        continue;

      batch->count++;
      if (received < newest)
        continue;
      newest = received;
      /* Absender wie Stamp: Name, sonst Adresse (stamp-folder-item.c:296-302). */
      camel_address_unformat (CAMEL_ADDRESS (from), camel_message_info_get_from (info));
      camel_internet_address_get (from, 0, &name, &address);
      g_set_str (&batch->sender, name != NULL && *name != '\0' ? name : address);
      g_set_str (&batch->subject, camel_message_info_get_subject (info));
      g_set_str (&batch->uid, camel_message_info_get_uid (info));
    }
  batch->latest = newest;

  g_task_return_boolean (task, TRUE);
}

static void
on_checked (GObject      *source_object,
            GAsyncResult *result,
            gpointer      user_data G_GNUC_UNUSED)
{
  CamelFolder *folder = CAMEL_FOLDER (source_object);
  Batch *batch = g_task_get_task_data (G_TASK (result));
  CamelStore *store = camel_folder_get_parent_store (folder);
  g_autoptr (GNotification) notification = NULL;
  g_autofree char *encoded_uid = NULL, *encoded_name = NULL, *uri = NULL, *body = NULL;
  GApplication *app = g_application_get_default ();

  if (!g_task_propagate_boolean (G_TASK (result), NULL) || store == NULL || app == NULL)
    return;
  if (batch->count == 0)
    {
      g_debug ("Benachrichtigung: %u neu in %s, keine ungelesen", batch->uids->len, camel_folder_get_full_name (folder));
      return;
    }

  g_object_set_data (G_OBJECT (folder), "revolution-notified", GINT_TO_POINTER (batch->latest));
  pending += batch->count;

  /* Ordner-URI wie Evolution e_mail_folder_uri_build (libemail-engine/e-mail-folder-utils.c:1808-1835). */
  encoded_uid = camel_url_encode (camel_service_get_uid (CAMEL_SERVICE (store)), ":;@/");
  encoded_name = camel_url_encode (camel_folder_get_full_name (folder), ":;@?#");
  uri = g_strdup_printf ("folder://%s/%s", encoded_uid, encoded_name);

  notification = g_notification_new (batch->sender != NULL ? batch->sender : _("Neue E-Mail"));
  if (pending > 1)
    /* Translators: Betreff, darunter die Zahl der übrigen neuen Nachrichten (Evolution „(and %d more)“). */
    body = g_strdup_printf (ngettext ("%s\n(und %u weitere)", "%s\n(und %u weitere)", pending - 1),
                            batch->subject != NULL ? batch->subject : "", pending - 1);
  else
    body = g_strdup (batch->subject);
  g_notification_set_body (notification, body);
  g_notification_set_category (notification, "email.arrived");
  g_notification_set_default_action_and_target (notification, "app.show-message", "(ss)", uri, batch->uid);
  g_application_send_notification (app, "new-mail", notification);
  g_debug ("Benachrichtigung: %u neu in %s, zusammen %u, Ziel %s %s", batch->count, camel_folder_get_full_name (folder),
           pending, uri, batch->uid);
}

static gboolean
start_check (gpointer data)
{
  Batch *batch = data;
  g_autoptr (GTask) task = g_task_new (batch->folder, NULL, on_checked, NULL);

  g_task_set_source_tag (task, start_check);
  g_task_set_task_data (task, batch, (GDestroyNotify) batch_free);
  g_task_run_in_thread (task, check_thread);

  return G_SOURCE_REMOVE;
}

/* „changed“ kommt im Hauptkontext, bei eingefrorenem Ordner im aufrufenden Thread (mail-folder.c,
 * on_camel_folder_changed); der Auftrag beginnt daher im UI-Thread wie mail-view.c on_folder_changed,
 * sonst käme das Ergebnis im Kontext dieses Threads an. */
static void
on_changed (CamelFolder           *folder,
            CamelFolderChangeInfo *changes,
            gpointer               user_data G_GNUC_UNUSED)
{
  Batch *batch;

  if (changes == NULL || changes->uid_added == NULL || changes->uid_added->len == 0)
    return;

  batch = g_new0 (Batch, 1);
  batch->folder = g_object_ref (folder);
  batch->uids = g_ptr_array_new_full (changes->uid_added->len, g_free);
  for (guint i = 0; i < changes->uid_added->len; i++)
    g_ptr_array_add (batch->uids, g_strdup (changes->uid_added->pdata[i]));
  batch->latest = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (folder), "revolution-notified"));

  g_main_context_invoke (NULL, start_check, batch);
}

/* Aufruf im UI-Thread (main.c, nach dem Abgleich des Posteingangs): neue Nachrichten melden. Nur
 * Posteingänge wie Evolutions Standard notify-only-inbox (org.gnome.evolution.plugin.mail-notification
 * .gschema.xml.in:3-7); Gesendet, Spam und Papierkorb also nie. Einmal je Ordner. */
void
revolution_mail_notification_watch (CamelFolder *inbox)
{
  g_return_if_fail (CAMEL_IS_FOLDER (inbox));

  if (g_object_get_data (G_OBJECT (inbox), "revolution-notify") != NULL)
    return;

  g_object_set_data (G_OBJECT (inbox), "revolution-notify", GINT_TO_POINTER (TRUE));
  g_signal_connect (inbox, "changed", G_CALLBACK (on_changed), NULL);
}

/* Geöffnet: Zähler zurück und Benachrichtigung weg wie Evolution remove_notification (dort 349-358). */
void
revolution_mail_notification_reset (void)
{
  pending = 0;
  if (g_application_get_default () != NULL)
    g_application_withdraw_notification (g_application_get_default (), "new-mail");
}
