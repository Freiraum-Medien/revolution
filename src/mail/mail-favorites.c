/* mail-favorites.c – Favoriten über alle Konten (#351)
 *
 * Abschnitt „Favoriten“ oben in der Seitenleiste wie der abgenommene Prototyp (Dokument #5,
 * Design/Prototyp/mail-hell.png): Alle Eingänge, Markiert, Entwürfe, Gesendet. Je Eintrag ein
 * CamelVeeFolder (Suchordner) in einem eigenen CamelVeeStore wie Evolutions Suchordner
 * (libemail-engine/e-mail-session.c:897-915 Store, mail-vfolder.c:923-956 je Regel ein Ordner per
 * camel_store_get_folder_sync, gehalten in vfolder_hash; mail-vfolder.c:154-197 Ausdruck und
 * Quellordner). Anders als Evolution ohne Regeldatei (vfolders.xml) und ohne Bearbeitung: Ausdrücke
 * und Quellen stehen fest, nichts wird gespeichert außer dem, was Camel selbst unter
 * ~/.local/share/revolution/mail/revolution-favoriten anlegt (folders.db, *.cmeta).
 * Vorlage für die Gruppe: Stamp stamp-collection-item.{c,h} (Quellen je Konto, Zähler als Summe);
 * Stamp führt die Ordner selbst zusammen, hier macht es Camel.
 *
 * Quellen je Konto (revolution_mail_favorites_add_store), Ordnertyp aus der Ordnerliste
 * (Posteingang vom Server, Entwürfe/Gesendet aus ESource bzw. Ersteinrichtung, #350):
 *   Alle Eingänge  Posteingänge
 *   Markiert       Posteingänge, nur CAMEL_MESSAGE_FLAGGED. Nicht alle Ordner: Evolution würde
 *                  dafür jeden Ordner jedes Kontos öffnen (Regel „alle aktiven entfernten Ordner“,
 *                  mail-vfolder.c:492-499, vfolder_adduri), und deren Summary wäre ohne Abgleich je
 *                  Ordner nur der Stand des Caches.
 *   Entwürfe       Entwürfe-Ordner; Gesendet: Gesendet-Ordner. Ohne erkannten Spezialordner (Gmail
 *                  „Gesendet“, camel-imapx-store.c:2874-2881) bleibt das Konto draußen.
 * Gelöschte und Spam blendet der Ausdruck aus, wie die Liste in normalen Ordnern (mail-view.c
 * hidden_flags) bzw. Evolution in Suchordnern (message-list.c:4595-4596, 4630-4633).
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>

#include "mail-favorites.h"
#include "mail-folder.h"

#define VISIBLE "(not (or (system-flag \"Deleted\") (system-flag \"Junk\")))"

static const struct
{
  const char *name;
  const char *label;
  const char *icon_name;
  guint32     source_type;
  const char *expression;
} favorites[] = {
  { "inbox",   N_("Alle Eingänge"), "inbox-symbolic",   CAMEL_FOLDER_TYPE_INBOX,
    "(match-all " VISIBLE ")" },
  { "flagged", N_("Markiert"),      "starred-symbolic", CAMEL_FOLDER_TYPE_INBOX,
    "(match-all (and (system-flag \"Flagged\") " VISIBLE "))" },
  { "drafts",  N_("Entwürfe"),      "drafts-symbolic",  CAMEL_FOLDER_TYPE_DRAFTS,
    "(match-all " VISIBLE ")" },
  { "sent",    N_("Gesendet"),      "sent-symbolic",    CAMEL_FOLDER_TYPE_SENT,
    "(match-all " VISIBLE ")" },
};

/* Am CamelVeeStore gehalten wie Evolution vfolder_hash: der Ordner-Cache des Stores hält nur
 * schwach (EDS camel-object-bag.c:446-452). */
#define FOLDERS_KEY "revolution-favorites"

static void schedule_counts (CamelFolder *folder, CamelFolderChangeInfo *changes, gpointer user_data);

/* Im Worker (main.c, load_session_thread), bevor die Konten gemeldet werden: Store in der Session
 * wie Evolution (camel_session_add_service mit dem Protokoll „vfolder“, EDS camel-provider.c:86-95
 * und 177-180; die Session konfiguriert ihn ohne ESource, mail-session.c), verbunden wie dort
 * (e-mail-session.c:909), ohne Ordner „Unmatched“ (camel-vee-store.c:569). */
gboolean
revolution_mail_favorites_setup (CamelSession  *session,
                                 GError       **error)
{
  g_autoptr (CamelService) service = NULL;
  g_autoptr (GPtrArray) folders = g_ptr_array_new_with_free_func (g_object_unref);

  service = camel_session_add_service (session, REVOLUTION_MAIL_FAVORITES_UID, "vfolder", CAMEL_PROVIDER_STORE,
                                       error);
  if (service == NULL)
    return FALSE;

  camel_vee_store_set_unmatched_enabled (CAMEL_VEE_STORE (service), FALSE);
  if (!camel_service_connect_sync (service, NULL, error))
    return FALSE;

  for (gsize i = 0; i < G_N_ELEMENTS (favorites); i++)
    {
      CamelFolder *folder = camel_store_get_folder_sync (CAMEL_STORE (service), favorites[i].name, 0, NULL, error);

      if (folder == NULL)
        return FALSE;

      camel_vee_folder_set_expression (CAMEL_VEE_FOLDER (folder), favorites[i].expression);
      g_signal_connect (folder, "changed", G_CALLBACK (schedule_counts), NULL);
      g_ptr_array_add (folders, folder);
    }

  g_object_set_data_full (G_OBJECT (service), FOLDERS_KEY, g_steal_pointer (&folders),
                          (GDestroyNotify) g_ptr_array_unref);
  g_debug ("Favoriten: Suchordner-Store %s bereit", REVOLUTION_MAIL_FAVORITES_UID);

  return TRUE;
}

/* Im UI-Thread (mail-view.c): Einträge des Abschnitts in Reihenfolge des Prototyps. */
void
revolution_mail_favorites_fill (GListStore            *store,
                                RevolutionMailAccount *account)
{
  for (gsize i = 0; i < G_N_ELEMENTS (favorites); i++)
    {
      g_autoptr (RevolutionMailFolder) folder = revolution_mail_folder_new (favorites[i].name, _(favorites[i].label),
                                                                            favorites[i].icon_name, account);

      g_list_store_append (store, folder);
    }
}

/* Zahl der Nachrichten je Quellordner fürs Log (Beleg „Zählung je Konto = Server“,
 * tests/kombiniert.sh), höchstens einmal je Sekunde und nur mit G_MESSAGES_DEBUG. */
static gint counts_pending;

static void
log_counts_thread (GTask        *task,
                   gpointer      source_object,
                   gpointer      task_data G_GNUC_UNUSED,
                   GCancellable *cancellable G_GNUC_UNUSED)
{
  GPtrArray *folders = g_object_get_data (source_object, FOLDERS_KEY);

  for (guint i = 0; folders != NULL && i < folders->len; i++)
    {
      CamelVeeFolder *vfolder = folders->pdata[i];
      CamelVeeSummary *summary = CAMEL_VEE_SUMMARY (camel_folder_get_folder_summary (folders->pdata[i]));
      GList *sources = camel_vee_folder_ref_folders (vfolder);

      for (GList *l = sources; l != NULL; l = l->next)
        {
          g_autoptr (GHashTable) uids = camel_vee_summary_get_uids_for_subfolder (summary, l->data);

          g_debug ("Favoriten: Anzahl %s %s %s %u", favorites[i].name,
                   camel_service_get_uid (CAMEL_SERVICE (camel_folder_get_parent_store (l->data))),
                   camel_folder_get_full_name (l->data), g_hash_table_size (uids));
        }
      g_list_free_full (sources, g_object_unref);
    }

  g_task_return_boolean (task, TRUE);
}

static gboolean
log_counts (gpointer data)
{
  g_autoptr (GTask) task = g_task_new (data, NULL, NULL, NULL);

  g_atomic_int_set (&counts_pending, 0);
  g_task_set_source_tag (task, log_counts);
  g_task_run_in_thread (task, log_counts_thread);

  return G_SOURCE_REMOVE;
}

/* CamelFolder::changed des Suchordners, im Hauptkontext oder (eingefroren) im Worker. */
static void
schedule_counts (CamelFolder           *folder,
                 CamelFolderChangeInfo *changes G_GNUC_UNUSED,
                 gpointer               user_data G_GNUC_UNUSED)
{
  if (g_log_writer_default_would_drop (G_LOG_LEVEL_DEBUG, G_LOG_DOMAIN) ||
      !g_atomic_int_compare_and_exchange (&counts_pending, 0, 1))
    return;

  g_timeout_add_seconds_full (G_PRIORITY_DEFAULT_IDLE, 1, log_counts,
                              g_object_ref (camel_folder_get_parent_store (folder)), g_object_unref);
}

/* Quellordner eines Kontos (volle Namen, NULL ohne Ordner dieses Typs), im Worker. */
typedef struct
{
  char *names[G_N_ELEMENTS (favorites)];
} AddSources;

static void
add_sources_free (AddSources *sources)
{
  for (gsize i = 0; i < G_N_ELEMENTS (favorites); i++)
    g_free (sources->names[i]);
  g_free (sources);
}

/* Im Worker: Quellordner holen (immer camel_store_get_folder_sync, README „Camel-Ordner“) und den
 * Suchordnern hinzufügen; camel_vee_folder_add_folder durchsucht dabei die Summary des Quellordners
 * (camel-vee-folder.c:302-333), ohne Server. Posteingänge gleicht main.c beim Start ab
 * (refresh_inbox_thread); Entwürfe und Gesendet hier einmal, sonst zeigten sie nur den Stand des
 * Caches (beim ersten Start leer). Danach folgen die Suchordner ihren Quellen über „changed“
 * (camel-vee-folder.c:575-610, auto-update). */
static void
add_sources_thread (GTask        *task,
                    gpointer      source_object,
                    gpointer      task_data,
                    GCancellable *cancellable)
{
  CamelStore *store = source_object;
  AddSources *sources = task_data;
  const char *uid = camel_service_get_uid (CAMEL_SERVICE (store));
  g_autoptr (CamelSession) session = camel_service_ref_session (CAMEL_SERVICE (store));
  g_autoptr (CamelService) vee_store = session != NULL ? camel_session_ref_service (session, REVOLUTION_MAIL_FAVORITES_UID)
                                                       : NULL;
  GPtrArray *folders = vee_store != NULL ? g_object_get_data (G_OBJECT (vee_store), FOLDERS_KEY) : NULL;

  if (folders == NULL)
    {
      g_task_return_new_error (task, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE,
                               _("Favoriten nicht verfügbar"));
      return;
    }

  for (gsize i = 0; i < G_N_ELEMENTS (favorites); i++)
    {
      g_autoptr (CamelFolder) folder = NULL;
      g_autoptr (GError) error = NULL;
      gint64 started = g_get_monotonic_time ();

      if (sources->names[i] == NULL)
        {
          g_debug ("Favoriten: %s ohne %s", uid, favorites[i].name);
          continue;
        }

      folder = camel_store_get_folder_sync (store, sources->names[i], 0, cancellable, &error);
      if (folder == NULL)
        {
          g_debug ("Favoriten: %s %s %s nicht geholt: %s", uid, favorites[i].name, sources->names[i], error->message);
          continue;
        }

      if (favorites[i].source_type != CAMEL_FOLDER_TYPE_INBOX &&
          !camel_folder_refresh_info_sync (folder, cancellable, &error))
        g_debug ("Favoriten: %s %s nicht abgeglichen: %s", uid, sources->names[i], error->message);

      camel_vee_folder_add_folder (folders->pdata[i], folder, cancellable);
      g_debug ("Favoriten: %s aus %s %s nach %.0f ms", favorites[i].name, uid, sources->names[i],
               (g_get_monotonic_time () - started) / 1000.0);
    }

  g_debug ("Favoriten: %s fertig", uid);
  g_task_return_pointer (task, g_ptr_array_ref (folders), (GDestroyNotify) g_ptr_array_unref);
}

static void
on_sources_added (GObject      *source_object,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_autoptr (RevolutionMailView) view = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (GPtrArray) folders = g_task_propagate_pointer (G_TASK (result), &error);

  if (folders == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Favoriten: %s nicht hinzugefügt: %s", camel_service_get_uid (CAMEL_SERVICE (source_object)),
                 error->message);
      return;
    }

  /* Zähler der Einträge live halten und jetzt zählen (revolution_mail_folder_watch). */
  for (guint i = 0; i < folders->len; i++)
    revolution_mail_view_watch_folder (view, folders->pdata[i], TRUE, TRUE);
}

static const char *
find_type (CamelFolderInfo *fi,
           guint32          type)
{
  for (; fi != NULL; fi = fi->next)
    {
      const char *found = (fi->flags & CAMEL_FOLDER_TYPE_MASK) == type && (fi->flags & CAMEL_FOLDER_NOSELECT) == 0
                            ? fi->full_name
                            : find_type (fi->child, type);

      if (found != NULL)
        return found;
    }

  return NULL;
}

/* Im UI-Thread (main.c, on_store_noted): Quellen dieses Kontos aus seiner Ordnerliste. */
void
revolution_mail_favorites_add_store (RevolutionMailView *view,
                                     CamelStore         *store,
                                     CamelFolderInfo    *info,
                                     GCancellable       *cancellable)
{
  AddSources *sources = g_new0 (AddSources, 1);
  g_autoptr (GTask) task = NULL;

  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (view));
  g_return_if_fail (CAMEL_IS_STORE (store));

  for (gsize i = 0; i < G_N_ELEMENTS (favorites); i++)
    sources->names[i] = g_strdup (find_type (info, favorites[i].source_type));

  task = g_task_new (store, cancellable, on_sources_added, g_object_ref (view));
  g_task_set_source_tag (task, revolution_mail_favorites_add_store);
  g_task_set_task_data (task, sources, (GDestroyNotify) add_sources_free);
  g_task_run_in_thread (task, add_sources_thread);
}
