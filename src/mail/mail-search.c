/* mail-search.c – Suche in der Nachrichtenliste (#365)
 *
 * Verhalten wie Evolutions Schnellsuche (modules/mail/e-mail-shell-view.c, mail_shell_view_execute_search):
 * Suchtext nach der Regel „Nachricht enthält“ (mail/searchtypes.xml.in:772-793: Betreff, Absender,
 * Empfänger, Text), Filter Ungelesen/Markiert/Mit Anhang als Systemflag (e-mail-shell-view.c:1136-1140,
 * 1224-1228, 1206-1210), alles in „(match-all …)“ (dort 1117-1123). Gesucht wird mit
 * camel_folder_search_by_expression wie die Nachrichtenliste (message-list.c:6296-6300), im Worker
 * (mail-view.c). Den Text sucht der Server, wo das Backend es kann: imapx per UID SEARCH BODY
 * (camel-imapx-search.c:344-383), microsoft365 per Graph $filter contains(body/content, …)
 * (camel-m365-folder-search.c:224-248); offline der Cache (dort 371-376 bzw. 380-387).
 *
 * „Alle Ordner“ wie Evolutions Bereich „Alle Konten“ (e-mail-shell-view.c:1408-1473): ein privater
 * CamelVeeFolder im Suchordner-Store der Favoriten (#351, mail-favorites.c) mit der Suche als Ausdruck
 * und allen Ordnern aller Stores als Quellen (add_folders_from_store, dort 50-104; search_results_exec,
 * dort 120-145). Ordner wie die Seitenleiste (main.c, note_store_thread: RECURSIVE | SUBSCRIBED), mit
 * FAST nur aus dem Cache des Stores, ohne LIST am Server je Suche (camel-imapx-store.c:2165-2170).
 * Endet die Suche, verliert der Suchordner seine Quellen wie dort mail_shell_view_stop_and_clear_search_vfolders
 * (775-785), sonst suchte er bei jeder Änderung irgendeines Ordners weiter.
 *
 * Die Suche schreibt nichts: keine Flags, keine Nachrichten.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>

#include "mail-favorites.h"
#include "mail-search.h"

/* Am Suchordner-Store gehalten wie die Konversationen (mail-thread.c CONVERSATIONS_KEY): der Ordner-Cache des
 * Stores hält nur schwach; der Lesebereich holt den Suchordner über seinen Namen (camel_store_get_folder_sync). */
#define SEARCH_KEY "revolution-search"

/* Nacheinander: Tippen startet eine Suche nach der anderen, abgebrochene Worker laufen noch zu Ende. */
static GMutex search_lock;

/* Im UI-Thread: Ausdruck aus Suchtext und Filter („all“, „unread“, „starred“, „attachment“ wie list.filter
 * in mail-view.blp), NULL ohne beides. Nur Zeichenketten, kein Camel-Aufruf mit Ein-/Ausgabe. */
char *
revolution_mail_search_expression (const char *text,
                                   const char *filter)
{
  g_autoptr (GString) parts = g_string_new (NULL);
  g_autofree char *stripped = g_strstrip (g_strdup (text != NULL ? text : ""));

  if (g_strcmp0 (filter, "unread") == 0)
    g_string_append (parts, " (match-all (not (system-flag \"Seen\")))");
  else if (g_strcmp0 (filter, "starred") == 0)
    g_string_append (parts, " (match-all (system-flag \"Flagged\"))");
  else if (g_strcmp0 (filter, "attachment") == 0)
    g_string_append (parts, " (match-all (system-flag \"Attachments\"))");

  if (*stripped != '\0')
    {
      g_autoptr (GString) word = g_string_new (NULL);

      /* Wert wie Evolutions Filterregel (e-filter-input.c:331-342, camel_sexp_encode_string, mit führendem Leerzeichen). */
      camel_sexp_encode_string (word, stripped);
      g_string_append_printf (parts,
                              " (match-all (or (header-contains \"Subject\"%s) (header-contains \"From\"%s)"
                              " (or (header-contains \"To\"%s) (header-contains \"Cc\"%s)) (body-contains%s)))",
                              word->str, word->str, word->str, word->str, word->str);
    }

  return parts->len > 0 ? g_strdup_printf ("(and%s)", parts->str) : NULL;
}

/* Alle Ordner eines Stores außer NOSELECT und Suchordnern (add_folders_from_store, e-mail-shell-view.c:50-104). */
static void
add_folders (GList        **folders,
             CamelStore    *store,
             GCancellable  *cancellable)
{
  g_autoptr (GError) error = NULL;
  CamelFolderInfo *root, *fi;

  root = camel_store_get_folder_info_sync (store, NULL,
                                           CAMEL_STORE_FOLDER_INFO_RECURSIVE | CAMEL_STORE_FOLDER_INFO_SUBSCRIBED |
                                           CAMEL_STORE_FOLDER_INFO_FAST,
                                           cancellable, &error);
  if (root == NULL && error != NULL)
    g_debug ("Suche: %s ohne Ordner: %s", camel_service_get_uid (CAMEL_SERVICE (store)), error->message);

  for (fi = root; fi != NULL && !g_cancellable_is_cancelled (cancellable);)
    {
      if ((fi->flags & CAMEL_FOLDER_NOSELECT) == 0)
        {
          g_autoptr (GError) folder_error = NULL;
          CamelFolder *folder = camel_store_get_folder_sync (store, fi->full_name, 0, cancellable, &folder_error);

          if (folder != NULL && !CAMEL_IS_VEE_FOLDER (folder))
            *folders = g_list_prepend (*folders, folder);
          else if (folder != NULL)
            g_object_unref (folder);
          else
            g_debug ("Suche: %s %s nicht geöffnet: %s", camel_service_get_uid (CAMEL_SERVICE (store)), fi->full_name,
                     folder_error != NULL ? folder_error->message : "");
        }

      /* Tiefe zuerst wie dort (e-mail-shell-view.c:84-98). */
      if (fi->child != NULL)
        fi = fi->child;
      else
        {
          while (fi != NULL && fi->next == NULL)
            fi = fi->parent;
          if (fi != NULL)
            fi = fi->next;
        }
    }

  camel_folder_info_free (root);
}

static CamelFolder *
search_folder (CamelSession  *session,
               gboolean       create,
               GCancellable  *cancellable,
               GError       **error)
{
  g_autoptr (CamelService) vee_store = camel_session_ref_service (session, REVOLUTION_MAIL_FAVORITES_UID);
  CamelFolder *folder;

  if (!CAMEL_IS_VEE_STORE (vee_store))
    {
      g_set_error (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE, _("Search unavailable"));
      return NULL;
    }

  folder = g_object_get_data (G_OBJECT (vee_store), SEARCH_KEY);
  if (folder != NULL || !create)
    return folder != NULL ? g_object_ref (folder) : NULL;

  /* Privat wie Evolutions Suchordner (e-mail-shell-view.c:1444-1447): nicht in der Ordnerliste des Stores. */
  folder = camel_store_get_folder_sync (CAMEL_STORE (vee_store), REVOLUTION_MAIL_SEARCH_NAME, CAMEL_STORE_FOLDER_PRIVATE,
                                        cancellable, error);
  if (folder != NULL)
    g_object_set_data_full (G_OBJECT (vee_store), SEARCH_KEY, g_object_ref (folder), g_object_unref);

  return folder;
}

/* Im Worker: Suchordner über alle Ordner aller Konten mit expression (revolution_mail_search_expression),
 * Gelöschte und Spam ausgeblendet wie die Favoriten. Quellen jedes Mal neu, ohne Quellen den neuen Ausdruck
 * setzen: sonst durchsuchte camel_vee_folder_set_expression erst die alten Quellen (camel-vee-folder.c,
 * Neuaufbau je Quelle). Neue Referenz oder NULL. */
CamelFolder *
revolution_mail_search_folder_sync (CamelSession  *session,
                                    const char    *expression,
                                    GCancellable  *cancellable,
                                    GError       **error)
{
  g_autoptr (GMutexLocker) locker = g_mutex_locker_new (&search_lock);
  g_autofree char *full = g_strdup_printf ("(and (match-all %s) %s)", REVOLUTION_MAIL_FAVORITES_VISIBLE, expression);
  g_autoptr (CamelFolder) folder = search_folder (session, TRUE, cancellable, error);
  GList *services, *folders = NULL;
  gint64 started = g_get_monotonic_time ();

  if (folder == NULL)
    return NULL;

  services = camel_session_list_services (session);
  for (GList *l = services; l != NULL && !g_cancellable_is_cancelled (cancellable); l = l->next)
    if (CAMEL_IS_STORE (l->data) && !CAMEL_IS_VEE_STORE (l->data))
      add_folders (&folders, l->data, cancellable);
  g_list_free_full (services, g_object_unref);
  folders = g_list_reverse (folders);

  camel_vee_folder_set_folders (CAMEL_VEE_FOLDER (folder), NULL, cancellable);
  camel_vee_folder_set_expression (CAMEL_VEE_FOLDER (folder), full);
  if (!g_cancellable_is_cancelled (cancellable))
    camel_vee_folder_set_folders (CAMEL_VEE_FOLDER (folder), folders, cancellable);
  g_debug ("Suche: alle Ordner, %u Ordner, %u Treffer nach %.0f ms", g_list_length (folders),
           camel_folder_get_message_count (folder), (g_get_monotonic_time () - started) / 1000.0);
  g_list_free_full (folders, g_object_unref);

  if (g_cancellable_set_error_if_cancelled (cancellable, error))
    return NULL;

  return g_steal_pointer (&folder);
}

/* Im Worker (mail-view.c, jedes Laden ohne „Alle Ordner“): Quellen des Suchordners freigeben. */
void
revolution_mail_search_clear_sync (CamelSession *session)
{
  g_autoptr (GMutexLocker) locker = g_mutex_locker_new (&search_lock);
  g_autoptr (CamelFolder) folder = search_folder (session, FALSE, NULL, NULL);
  GList *sources = folder != NULL ? camel_vee_folder_ref_folders (CAMEL_VEE_FOLDER (folder)) : NULL;

  if (sources != NULL)
    {
      g_debug ("Suche: alle Ordner beendet, %u Quellen frei", g_list_length (sources));
      camel_vee_folder_set_folders (CAMEL_VEE_FOLDER (folder), NULL, NULL);
    }
  g_list_free_full (sources, g_object_unref);
}
