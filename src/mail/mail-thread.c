/* mail-thread.c – Konversationen: Threads der Nachrichtenliste (#357)
 *
 * Konversationsansicht wie Apple Mail (Dokument #10 „#357“): je Thread eine Zeile, Eingang und
 * Gesendet gemischt. Vorlage Stamp stamp-conversation-list.c:221-267 (get_combined_thread: Ordner und
 * Gesendet des Kontos zusammen threaden) und 621-641 (build_items: je Wurzel ein Eintrag, Threads ohne
 * Nachricht aus dem Ordner ausgeblendet). Stamp nutzt dafür camel_folder_thread_new_items (EDS 3.62,
 * Nachrichten aus mehreren Ordnern); EDS 3.56 threadet nur die Nachrichten eines Ordners
 * (camel_folder_thread_messages_new, camel-folder-thread.c:689-720: camel_folder_get_message_info je
 * UID). Daher hier wie bei den Favoriten (#351, mail-favorites.c) ein CamelVeeFolder aus Ordner und
 * Gesendet im Suchordner-Store; dessen Einträge liefern Message-ID und References der Quelle
 * (camel-vee-message-info.c:394-416). Alles hier läuft im Worker (Architektur 1).
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>

#include "eds-compat.h"
#include "mail-favorites.h"
#include "mail-message.h"
#include "mail-thread.h"

/* Am Suchordner-Store: Name → Suchordner der Konversation, für die Sitzung gehalten wie die Favoriten
 * (mail-favorites.c FOLDERS_KEY; der Ordner-Cache des Stores hält nur schwach). Am Suchordner: der
 * Ordner, zu dem er gehört (build: Threads ohne Nachricht aus diesem Ordner fallen weg). */
#define CONVERSATIONS_KEY "revolution-conversations"
#define SOURCE_KEY        "revolution-conversation-source"

static GMutex conversations_lock;

/* Name des Suchordners zur Konversation eines Ordners, fest je Konto und Ordner (mail-view.c lädt
 * Nachrichten der Konversation über diesen Namen im Store revolution-favoriten). */
char *
revolution_mail_thread_folder_name (const char *uid,
                                    const char *full_name)
{
  g_autofree char *key = g_strdup_printf ("%s/%s", uid, full_name);
  g_autofree char *sum = g_compute_checksum_for_string (G_CHECKSUM_SHA1, key, -1);

  return g_strdup_printf ("konversation-%.16s", sum);
}

/* Im Worker: Suchordner aus folder und dem Gesendet-Ordner seines Kontos (sent_full_name, Spezialordner
 * aus #350), Gelöschte und Spam ausgeblendet wie die Favoriten. Danach folgt der Suchordner seinen
 * Quellen über „changed“ (camel-vee-folder.c:575-610). Einmal angelegt bleibt er für die Sitzung; gehört
 * er noch zu einer früheren Instanz des Ordners (Store neu angelegt, #351), bekommt er die neuen Quellen.
 * Neu angelegt: *sent_out ist der Gesendet-Ordner, den der Aufrufer einmal abgleicht wie die Favoriten
 * (add_sources_thread), nach der ersten Anzeige aus dem Cache (mail-view.c load_messages_thread; vorher
 * kostete der Abgleich hier bis 1,2 s bis zur ersten Zeile, build/m4-357/last-konv), sonst NULL. */
CamelFolder *
revolution_mail_thread_folder_sync (CamelFolder   *folder,
                                    const char    *sent_full_name,
                                    CamelFolder  **sent_out,
                                    GCancellable  *cancellable,
                                    GError       **error)
{
  CamelStore *store = camel_folder_get_parent_store (folder);
  g_autoptr (CamelSession) session = store != NULL ? camel_service_ref_session (CAMEL_SERVICE (store)) : NULL;
  g_autoptr (CamelService) vee_store = session != NULL ? camel_session_ref_service (session, REVOLUTION_MAIL_FAVORITES_UID)
                                                       : NULL;
  const char *uid = store != NULL ? camel_service_get_uid (CAMEL_SERVICE (store)) : NULL;
  g_autofree char *name = NULL;
  g_autoptr (CamelFolder) sent = NULL;
  g_autoptr (CamelFolder) vee = NULL;
  gint64 started = g_get_monotonic_time ();
  GHashTable *cache;
  GList *sources;

  if (!CAMEL_IS_VEE_STORE (vee_store))
    {
      g_set_error (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE, _("Konversationen nicht verfügbar"));
      return NULL;
    }

  name = revolution_mail_thread_folder_name (uid, camel_folder_get_full_name (folder));

  g_mutex_lock (&conversations_lock);
  cache = g_object_get_data (G_OBJECT (vee_store), CONVERSATIONS_KEY);
  if (cache == NULL)
    {
      cache = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
      g_object_set_data_full (G_OBJECT (vee_store), CONVERSATIONS_KEY, cache, (GDestroyNotify) g_hash_table_unref);
    }

  vee = g_hash_table_lookup (cache, name);
  if (vee != NULL && g_object_get_data (G_OBJECT (vee), SOURCE_KEY) == folder)
    {
      g_object_ref (vee);
      g_mutex_unlock (&conversations_lock);
      return g_steal_pointer (&vee);
    }
  if (vee != NULL)
    g_object_ref (vee);

  sent = camel_store_get_folder_sync (store, sent_full_name, 0, cancellable, error);
  if (sent == NULL)
    {
      g_mutex_unlock (&conversations_lock);
      return NULL;
    }
  if (vee == NULL)
    {
      vee = camel_store_get_folder_sync (CAMEL_STORE (vee_store), name, 0, cancellable, error);
      if (vee == NULL)
        {
          g_mutex_unlock (&conversations_lock);
          return NULL;
        }
      camel_vee_folder_set_expression (CAMEL_VEE_FOLDER (vee), "(match-all " REVOLUTION_MAIL_FAVORITES_VISIBLE ")");
    }

  sources = g_list_append (g_list_append (NULL, folder), sent);
  camel_vee_folder_set_folders (CAMEL_VEE_FOLDER (vee), sources, cancellable);
  g_list_free (sources);
  g_object_set_data_full (G_OBJECT (vee), SOURCE_KEY, g_object_ref (folder), g_object_unref);
  g_hash_table_replace (cache, g_strdup (name), g_object_ref (vee));
  g_mutex_unlock (&conversations_lock);

  g_debug ("Konversationen: %s %s mit %s als %s nach %.0f ms", uid, camel_folder_get_full_name (folder),
           sent_full_name, name, (g_get_monotonic_time () - started) / 1000.0);
  *sent_out = g_steal_pointer (&sent);

  return g_steal_pointer (&vee);
}

/* Im Worker (mail-view.c, list.refresh): Abgleich mit dem Server. Der Abgleich eines Suchordners baut
 * ihn nur aus seinen Quellen neu auf (camel-vee-folder.c:1439-1449), daher beim Suchordner einer
 * Konversation seine Quellen (Ordner und Gesendet) abgleichen, sonst den Ordner selbst; immer über
 * revolution_eds_folder_refresh_info_sync (#415, keine Echo-PATCH an microsoft365). */
gboolean
revolution_mail_thread_refresh_sync (CamelFolder   *folder,
                                     GCancellable  *cancellable,
                                     GError       **error)
{
  GList *sources;
  gboolean ok = TRUE;

  if (!CAMEL_IS_VEE_FOLDER (folder) || g_object_get_data (G_OBJECT (folder), SOURCE_KEY) == NULL)
    return revolution_eds_folder_refresh_info_sync (folder, cancellable, error);

  sources = camel_vee_folder_ref_folders (CAMEL_VEE_FOLDER (folder));
  for (GList *l = sources; l != NULL && ok; l = l->next)
    ok = revolution_eds_folder_refresh_info_sync (l->data, cancellable, error);
  g_list_free_full (sources, g_object_unref);

  return ok;
}

static gint64
info_date (CamelMessageInfo *info)
{
  gint64 date = camel_message_info_get_date_sent (info);

  return date > 0 ? date : camel_message_info_get_date_received (info);
}

/* Alt → neu, bei gleichem Datum nach UID. */
static int
compare_infos (gconstpointer a,
               gconstpointer b)
{
  CamelMessageInfo *ia = *(CamelMessageInfo **) a, *ib = *(CamelMessageInfo **) b;
  gint64 da = info_date (ia), db = info_date (ib);

  if (da != db)
    return da < db ? -1 : 1;

  return g_strcmp0 (camel_message_info_get_uid (ia), camel_message_info_get_uid (ib));
}

/* Neueste Konversation zuerst wie die flache Liste (mail-view.c compare_messages). */
static int
compare_conversations (gconstpointer a,
                       gconstpointer b)
{
  RevolutionMailMessage *ca = *(RevolutionMailMessage **) a, *cb = *(RevolutionMailMessage **) b;
  gint64 da = revolution_mail_message_get_timestamp (ca), db = revolution_mail_message_get_timestamp (cb);

  if (da != db)
    return da < db ? 1 : -1;

  return g_strcmp0 (revolution_mail_message_get_uid (cb), revolution_mail_message_get_uid (ca));
}

/* Alle Nachrichten unter node und seinen Geschwistern (Platzhalter ohne Nachricht übersprungen). */
static void
collect (CamelFolderThreadNode *node,
         GPtrArray             *infos)
{
  for (; node != NULL; node = revolution_eds_thread_node_get_next (node))
    {
      CamelMessageInfo *info = revolution_eds_thread_node_get_item (node);

      if (info != NULL)
        g_ptr_array_add (infos, info);
      collect (revolution_eds_thread_node_get_child (node), infos);
    }
}

static gboolean
has_source (GPtrArray   *infos,
            CamelFolder *source)
{
  for (guint i = 0; i < infos->len; i++)
    if (CAMEL_IS_VEE_MESSAGE_INFO (infos->pdata[i]) &&
        camel_vee_message_info_get_original_folder (infos->pdata[i]) == source)
      return TRUE;

  return FALSE;
}

/* Im Worker (mail-view.c, send_messages und Live-Änderungen): Konversationen aus den sichtbaren
 * Nachrichten infos des Ordners (Liste oder Suchordner der Konversation). Je Wurzel des Threads ein
 * Eintrag (revolution_mail_message_new_conversation), neueste Konversation zuerst. Im Suchordner der
 * Konversation nur Threads mit mindestens einer Nachricht aus dem Ordner selbst, wie Stamp
 * (stamp-conversation-list.c:270-300, 633-634 node_has_message_from_folder); reine Gesendet-Threads
 * gehören nicht in den Posteingang. now: Stichtag fürs relative Datum. */
GPtrArray *
revolution_mail_thread_build (CamelFolder *folder,
                              GPtrArray   *infos,
                              GDateTime   *now)
{
  CamelFolder *source = g_object_get_data (G_OBJECT (folder), SOURCE_KEY);
  gboolean keep_account = source == NULL && CAMEL_IS_VEE_FOLDER (folder);
  g_autoptr (GPtrArray) uids = g_ptr_array_sized_new (infos->len);
  GPtrArray *conversations = g_ptr_array_new_with_free_func (g_object_unref);
  CamelFolderThread *thread;
  guint messages = 0;

  for (guint i = 0; i < infos->len; i++)
    g_ptr_array_add (uids, (gpointer) camel_message_info_get_uid (infos->pdata[i]));

  thread = revolution_eds_thread_new (folder, uids);
  for (CamelFolderThreadNode *root = revolution_eds_thread_get_tree (thread); root != NULL;
       root = revolution_eds_thread_node_get_next (root))
    {
      g_autoptr (GPtrArray) members = g_ptr_array_new ();
      g_autoptr (GPtrArray) items = NULL;
      CamelMessageInfo *first = revolution_eds_thread_node_get_item (root);

      if (first != NULL)
        g_ptr_array_add (members, first);
      collect (revolution_eds_thread_node_get_child (root), members);
      if (members->len == 0 || (source != NULL && !has_source (members, source)))
        continue;

      g_ptr_array_sort (members, compare_infos);
      items = g_ptr_array_new_full (members->len, g_object_unref);
      for (guint i = 0; i < members->len; i++)
        g_ptr_array_add (items, revolution_mail_message_new (members->pdata[i], now));
      messages += members->len;

      g_ptr_array_add (conversations,
                       revolution_mail_message_new_conversation (camel_message_info_get_uid (first != NULL ? first : members->pdata[0]),
                                                                 items, keep_account));
    }
  revolution_eds_thread_free (thread);

  g_ptr_array_sort (conversations, compare_conversations);
  g_debug ("Konversationen: %s %u aus %u Nachrichten (%u in Konversationen)", camel_folder_get_full_name (folder),
           conversations->len, infos->len, messages);

  return conversations;
}
