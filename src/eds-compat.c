/* eds-compat.c
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Die einzige Stelle für Aufrufe, die von der EDS-/Camel-Version abhängen
 * (CLAUDE.md Architektur-Festlegung 6, README „Architektur“).
 * EDS_CHECK_VERSION: /usr/include/evolution-data-server/libedataserver/eds-version.h:63. */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <libedataserver/libedataserver.h>

#include "eds-compat.h"

#if !EDS_CHECK_VERSION (3, 56, 0)
#error "Revolution braucht Evolution-Data-Server 3.56 oder neuer"
#endif

/* folder_flagged der Nachricht selbst: camel_folder_summary_get_info_flags führt es bei Änderungen
 * der Kategorien (camel_message_info_set_user_flag) nicht nach (gemessen, #415). */
static CamelMessageInfo *
ref_if_folder_flagged (CamelFolderSummary *summary,
                       const char         *uid)
{
  CamelMessageInfo *info = camel_folder_summary_get (summary, uid);

  if (info != NULL && !camel_message_info_get_folder_flagged (info))
    g_clear_object (&info);

  return info;
}

/* #409: Sperre je Ordner für Flag-Aufträge und Abgleich. camel_folder_lock wirkt bei imapx und
 * microsoft365 nicht: beide rufen camel_folder_set_lock_async (folder, TRUE) (EDS
 * camel-imapx-folder.c:1237, evolution-ews camel-m365-folder.c:1773), danach kehren
 * camel_folder_lock/-unlock sofort zurück (camel-folder.c:1613-1617, 3117-3140). Am Ordner gespeichert
 * (g_object_set_data_full), freigegeben mit ihm; wer sperrt, hält eine Referenz auf den Ordner.
 * Nur Revolution nimmt sie (set_flags_thread, revolution_eds_folder_refresh_info_sync), nie im UI-Thread,
 * und wer sie hält, wartet auf keinen anderen Halter: kein Deadlock. */
static GMutex folder_locks_mutex;

static void
free_folder_lock (gpointer data)
{
  g_rec_mutex_clear (data);
  g_free (data);
}

static GRecMutex *
folder_lock (CamelFolder *folder)
{
  GRecMutex *lock;

  g_mutex_lock (&folder_locks_mutex);
  lock = g_object_get_data (G_OBJECT (folder), "revolution-folder-lock");
  if (lock == NULL)
    {
      lock = g_new0 (GRecMutex, 1);
      g_rec_mutex_init (lock);
      g_object_set_data_full (G_OBJECT (folder), "revolution-folder-lock", lock, free_folder_lock);
    }
  g_mutex_unlock (&folder_locks_mutex);

  return lock;
}

void
revolution_eds_folder_lock (CamelFolder *folder)
{
  g_rec_mutex_lock (folder_lock (folder));
}

void
revolution_eds_folder_unlock (CamelFolder *folder)
{
  g_rec_mutex_unlock (folder_lock (folder));
}

/* UIDs der lokal geänderten, noch nicht an den Server geschickten Nachrichten. */
static GHashTable *
folder_flagged_uids (CamelFolderSummary *summary)
{
  GHashTable *uids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GPtrArray *changed = camel_folder_summary_get_changed (summary);

  for (guint i = 0; i < changed->len; i++)
    {
      g_autoptr (CamelMessageInfo) info = ref_if_folder_flagged (summary, changed->pdata[i]);

      if (info != NULL)
        g_hash_table_add (uids, g_strdup (changed->pdata[i]));
    }
  camel_folder_summary_free_array (changed);

  return uids;
}

/* #415: camel_folder_refresh_info_sync, bei microsoft365 ohne Rückschreiben dessen, was der Server
 * gerade geliefert hat. evolution-ews 3.56.2 übernimmt Server-Änderungen an vorhandenen Nachrichten
 * mit camel_message_info_set_flags (camel-m365-folder.c:902-904, 666-674), das folder_flagged setzt
 * (camel-message-info.c:1856-1862); der nächste Sync (Camel-Zeitgeber store-changes-interval,
 * camel-folder.c:309-313 und 236-249, oder eine eigene Bedienung im selben Ordner, mail-view.c
 * set_flags_thread) schickt sie dann als PATCH zurück (camel-m365-folder.c:1341, 1385-1388), aus
 * Teilobjekten des Deltas mit „categories“:[] und „importance“ normal. EWS und imapx setzen
 * folder_flagged nach dem Server-Abgleich zurück (evolution-ews camel-ews-utils.c:1004-1019, EDS
 * camel-imapx-utils.c:394); dasselbe hier: was vorher nicht lokal geändert war, gilt danach als
 * gespeichert. Vorher-Stand, Abgleich, Zurücksetzen und Speichern unter revolution_eds_folder_lock
 * (#409; camel_folder_lock wirkt bei microsoft365 nicht, siehe oben). Die
 * Summary danach speichern: microsoft365 speichert sie am Ende des Abgleichs noch mit folder_flagged
 * (camel-m365-folder.c:1000); ohne erneutes Speichern ginge das Echo nach Beenden oder Absturz
 * innerhalb von store-changes-interval beim nächsten Start doch noch raus.
 * Nur über öffentliche Camel-API (die Server-Flags des Providers sind nicht öffentlich). Im Worker.
 * #409: Eine eigene Flag-Änderung während des Abgleichs wird nicht mit zurückgesetzt: set_flags_thread
 * (mail-view.c) setzt sie unter derselben Sperre, also erst nach dem Abgleich. Lokal geänderte
 * Nachrichten (folder_flagged, z. B. offline gesetzt oder nach abgebrochenem Speichern) zuerst
 * schicken, wie imapx vor jedem Abgleich (camel_imapx_conn_manager_refresh_info_sync: erst
 * sync_changes, bei Fehler kein Abgleich,
 * camel-imapx-conn-manager.c:1548-1549). microsoft365 tut das nicht (camel-m365-folder.c:932-1022),
 * und camel_offline_store_set_online_sync synchronisiert nur geöffnete Ordner; sonst ginge eine solche
 * Änderung beim nächsten Start nicht an den Server.
 * Grenze: Camels Zeitgeber store-changes (camel-folder.c:214-253, Job 140-150) synchronisiert ohne
 * diese Sperre. Er startet nach jedem „changed“ neu (camel-folder.c:222-224) und schickt nur
 * Nachrichten mit folder_flagged; ein Echo bliebe nur möglich, wenn zwischen zwei Seiten des Deltas
 * mehr als store-changes-interval (Standard 3 s, camel-store-settings.c:110-116) vergehen.
 * Abschalten ginge nur über CamelStoreSettings, die an Evolutions ESource gebunden sind
 * (e-source-camel.c:539-545, bidirektional); daher nicht geändert. */
gboolean
revolution_eds_folder_refresh_info_sync (CamelFolder   *folder,
                                         GCancellable  *cancellable,
                                         GError       **error)
{
  CamelStore *store = camel_folder_get_parent_store (folder);
  CamelProvider *provider = store != NULL ? camel_service_get_provider (CAMEL_SERVICE (store)) : NULL;
  CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);
  g_autoptr (GHashTable) before = NULL;
  GPtrArray *changed;
  gboolean ok;
  guint reset = 0;

  if (provider == NULL || summary == NULL || g_strcmp0 (provider->protocol, "microsoft365") != 0)
    return camel_folder_refresh_info_sync (folder, cancellable, error);

  revolution_eds_folder_lock (folder);
  before = folder_flagged_uids (summary);
  if (g_hash_table_size (before) > 0)
    {
      guint local = g_hash_table_size (before);

      if (!camel_folder_synchronize_sync (folder, FALSE, cancellable, error))
        {
          revolution_eds_folder_unlock (folder);
          return FALSE;
        }
      g_clear_pointer (&before, g_hash_table_unref);
      before = folder_flagged_uids (summary);
      g_debug ("Abgleich: %s %s: %u lokale Änderungen vorher gespeichert", camel_service_get_uid (CAMEL_SERVICE (store)),
               camel_folder_get_full_name (folder), local);
    }

  ok = camel_folder_refresh_info_sync (folder, cancellable, error);

  changed = camel_folder_summary_get_changed (summary);
  for (guint i = 0; i < changed->len; i++)
    {
      const char *uid = changed->pdata[i];
      g_autoptr (CamelMessageInfo) info = NULL;

      if (g_hash_table_contains (before, uid))
        continue;
      info = ref_if_folder_flagged (summary, uid);
      if (info != NULL && camel_message_info_set_folder_flagged (info, FALSE))
        reset++;
    }
  camel_folder_summary_free_array (changed);

  if (reset > 0)
    {
      g_autoptr (GError) save_error = NULL;

      if (!camel_folder_summary_save (summary, &save_error))
        g_debug ("Abgleich: %s %s: Summary nicht gespeichert: %s", camel_service_get_uid (CAMEL_SERVICE (store)),
                 camel_folder_get_full_name (folder), save_error->message);
      g_debug ("Abgleich: %s %s: %u Server-Änderungen nicht zurückgeschrieben",
               camel_service_get_uid (CAMEL_SERVICE (store)), camel_folder_get_full_name (folder), reset);
    }
  revolution_eds_folder_unlock (folder);

  return ok;
}

/* #357: Threads (Konversationen). EDS 3.56: camel_folder_thread_messages_new mit offener Struktur
 * CamelFolderThreadNode (camel-folder-thread.h:32-38, 51); der Baum ist nach Datum sortiert
 * (camel-folder-thread.c:410-505 sort_thread, 627). Threads nur über Message-ID/References, nicht über
 * den Betreff, wie Evolutions Standard thread-subject false (org.gnome.evolution.mail.gschema.xml.in:528-532).
 * Ab EDS 3.62 heißt die API camel_folder_thread_new mit Zugriffsfunktionen; Namen wie die Vorlage
 * Stamp (stamp-conversation-list.c:242, 628-636, stamp-conversation-item.c:75-77), dort ungeprüft
 * übersetzt: auf diesem System gibt es nur EDS 3.56. */
CamelFolderThread *
revolution_eds_thread_new (CamelFolder *folder,
                           GPtrArray   *uids)
{
#if EDS_CHECK_VERSION (3, 62, 0)
  return camel_folder_thread_new (folder, uids, CAMEL_FOLDER_THREAD_FLAG_SORT);
#else
  return camel_folder_thread_messages_new (folder, uids, FALSE);
#endif
}

void
revolution_eds_thread_free (CamelFolderThread *thread)
{
#if EDS_CHECK_VERSION (3, 62, 0)
  g_object_unref (thread);
#else
  camel_folder_thread_messages_unref (thread);
#endif
}

CamelFolderThreadNode *
revolution_eds_thread_get_tree (CamelFolderThread *thread)
{
#if EDS_CHECK_VERSION (3, 62, 0)
  return camel_folder_thread_get_tree (thread);
#else
  return thread->tree;
#endif
}

CamelFolderThreadNode *
revolution_eds_thread_node_get_next (CamelFolderThreadNode *node)
{
#if EDS_CHECK_VERSION (3, 62, 0)
  return camel_folder_thread_node_get_next (node);
#else
  return node->next;
#endif
}

CamelFolderThreadNode *
revolution_eds_thread_node_get_child (CamelFolderThreadNode *node)
{
#if EDS_CHECK_VERSION (3, 62, 0)
  return camel_folder_thread_node_get_child (node);
#else
  return node->child;
#endif
}

/* NULL bei Platzhaltern (Nachricht fehlt, nur aus References bekannt). */
CamelMessageInfo *
revolution_eds_thread_node_get_item (CamelFolderThreadNode *node)
{
#if EDS_CHECK_VERSION (3, 62, 0)
  return camel_folder_thread_node_get_item (node);
#else
  return (CamelMessageInfo *) node->message;
#endif
}
