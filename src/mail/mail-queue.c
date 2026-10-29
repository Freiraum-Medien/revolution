/* mail-queue.c – Warteschlange der Mail-Ansicht: Flags setzen und Verschieben im Worker (#353, #409, #363,
 * #351/#357 echter Ordner).
 * Aus mail-view.c (#444); gemeinsamer Zustand in mail-view-private.h.
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
#include "mail-view-private.h"

/* #353: Flags setzen wie Evolution (e_mail_reader_mark_selected, e-mail-reader-utils.c:1029-1065:
 * camel_folder_set_message_flags), hier im Worker (Architektur 1), danach sofort
 * camel_folder_synchronize_sync (FALSE), damit der Server die Änderung hat. Evolution überlässt
 * das Camel: „changed“ plant das Speichern nach store-changes-interval Sekunden (Standard 3,
 * EDS camel-folder.c:214-253, camel-store-settings.c:110-115), und beim Ordnerwechsel
 * mail_sync_folder (e-mail-reader.c:4398-4404). Die Liste folgt über „changed“.
 * Ein Auftrag nach dem anderen (Warteschlange wie start_folder_changes), damit der Server die
 * Änderungen in der Reihenfolge der Bedienung bekommt. Ohne Verbindung bleibt die Änderung wie
 * bei Evolution in der Summary (imapx überspringt das Synchronisieren offline,
 * camel-imapx-folder.c:877-880) und geht beim nächsten Abgleich mit (refresh_info schickt
 * zuerst die Änderungen, camel-imapx-conn-manager.c:1548). Die Zeile zeigt die Bedienung sofort
 * (revolution_mail_message_set_flags); „changed“ wird erst danach wieder angewendet
 * (start_folder_changes). */

static void
flags_location_free (FlagsLocation *location)
{
  g_object_unref (location->folder);
  g_free (location->uid);
  g_free (location);
}

void
set_flags_free (SetFlags *data)
{
  g_object_unref (data->folder);
  g_free (data->uid);
  g_clear_pointer (&data->location, flags_location_free);
  g_clear_pointer (&data->transfer, transfer_release);
  g_free (data->loading);
  g_free (data);
}

/* #351: Im Suchordner (Favoriten; #357 auch Konversationen, mail-thread.c) am echten Ordner setzen und
 * speichern wie Evolution (mail_reader_replace_vee_folder_with_real, e-mail-reader.c:425-450,
 * camel_vee_folder_get_location);
 * camel_folder_synchronize_sync bzw. das Speichern der Summary des Suchordners speichert die
 * Quellordner nicht (camel-vee-folder.c:1465-1477). Die Liste folgt über „changed“ der Quelle und des
 * Suchordners. Gibt den echten Ordner (mit Referenz) und in real_uid dessen UID zurück, NULL wenn die
 * Nachricht nicht mehr im Suchordner ist (z. B. „Markiert“ nach Entfernen der Markierung). Im Worker. */
CamelFolder *
real_folder (CamelFolder  *folder,
             const char   *uid,
             char        **real_uid)
{
  g_autoptr (CamelMessageInfo) info = NULL;

  if (!CAMEL_IS_VEE_FOLDER (folder))
    {
      *real_uid = g_strdup (uid);
      return g_object_ref (folder);
    }
  info = camel_folder_get_message_info (folder, uid);
  folder = info != NULL ? camel_vee_folder_get_location (CAMEL_VEE_FOLDER (folder), CAMEL_VEE_MESSAGE_INFO (info), real_uid)
                        : NULL;

  return folder != NULL ? g_object_ref (folder) : NULL;
}

static void
set_flags_thread (GTask        *task,
                  gpointer      source_object G_GNUC_UNUSED,
                  gpointer      task_data,
                  GCancellable *cancellable G_GNUC_UNUSED)
{
  SetFlags *data = task_data;
  FlagsLocation *location = g_new0 (FlagsLocation, 1);
  CamelFolder *folder;
  const char *uid;
  CamelStore *store;
  g_autoptr (CamelSession) session = NULL;
  GError *error = NULL;

  if (data->transfer != NULL)
    {
      g_free (location);
      transfer_thread (task, data);
      return;
    }
  location->folder = real_folder (data->folder, data->uid, &location->uid);
  if (location->folder == NULL)
    {
      g_free (location);
      g_task_return_new_error (task, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID_UID,
                               _("Message %s not found"), data->uid);
      return;
    }
  /* #409: für den Notpfad (flags_deadline) merken: im Suchordner „Markiert“ fällt die Nachricht nach
   * dem Entfernen der Markierung heraus, danach fände real_folder sie nicht mehr. */
  g_atomic_pointer_set (&data->location, location);
  folder = location->folder;
  uid = location->uid;

  store = camel_folder_get_parent_store (folder);
  session = camel_service_ref_session (CAMEL_SERVICE (store));
  /* #409: Setzen und Speichern unter Revolutions Sperre je Ordner (revolution_eds_folder_lock,
   * eds-compat.c; camel_folder_lock wirkt bei imapx und microsoft365 nicht), die auch der Abgleich bei
   * microsoft365 hält (revolution_eds_folder_refresh_info_sync); sonst setzt dieser eine während des
   * Abgleichs gesetzte Änderung mit zurück. Der Notpfad beim Beenden (flags_save_local_thread) nimmt
   * sie nicht. */
  revolution_eds_folder_lock (folder);
  camel_folder_set_message_flags (folder, uid, data->mask, data->set);
  if ((CAMEL_IS_OFFLINE_STORE (store) && !camel_offline_store_get_online (CAMEL_OFFLINE_STORE (store))) ||
      session == NULL || !camel_session_get_online (session))
    {
      /* Summary sofort auf die Platte: sie speichert sonst erst beim Freigeben des Ordners
       * (camel-folder.c:810-811), das Revolution beim Beenden nicht abwartet. Evolution speichert
       * beim Beenden über camel_store_synchronize (e-mail-backend.c:491, camel-store.c:477-478). */
      if (!camel_folder_summary_save (camel_folder_get_folder_summary (folder), &error))
        g_task_return_error (task, error);
      else
        g_task_return_int (task, FLAGS_LOCAL);
    }
  /* Ohne Cancellable: die Anwendung bleibt gehalten, bis der letzte Auftrag durch ist
   * (start_flags), auch nach dem Schließen des Fensters (Obergrenze flags_deadline). Bei einem
   * Fehler die Summary lokal speichern, damit der nächste Abgleich die Änderung nachholt. */
  else if (!camel_folder_synchronize_sync (folder, FALSE, NULL, &error))
    {
      camel_folder_summary_save (camel_folder_get_folder_summary (folder), NULL);
      g_task_return_error (task, error);
    }
  else
    g_task_return_int (task, FLAGS_SAVED);
  revolution_eds_folder_unlock (folder);
}

/* Anwendung halten bzw. freigeben (g_application_hold), damit das Beenden die Aufträge abwartet
 * wie Evolution, das beim Beenden synchronisiert (mail_backend_prepare_for_quit_cb,
 * e-mail-backend.c:407-491). */
static void
hold_for_flags (RevolutionMailView *self,
                gboolean            hold)
{
  if (hold && self->flags_hold == NULL)
    {
      self->flags_hold = g_application_get_default ();
      if (self->flags_hold != NULL)
        g_application_hold (g_object_ref (self->flags_hold));
    }
  else if (!hold && self->flags_hold != NULL)
    {
      g_clear_handle_id (&self->flags_deadline_id, g_source_remove);
      g_application_release (self->flags_hold);
      g_clear_object (&self->flags_hold);
    }
}

/* Obergrenze nach dem Schließen: hängt der laufende Auftrag (z. B. Server antwortet nicht), ihn und
 * die wartenden nur lokal setzen und die Summaries der echten Ordner speichern; der nächste Start holt
 * sie nach (refresh_info schickt zuerst die Änderungen, camel-imapx-conn-manager.c:1548; microsoft365
 * über revolution_eds_folder_refresh_info_sync). #409: ohne revolution_eds_folder_lock, die hält der
 * hängende Auftrag (set_flags_thread). camel_folder_summary_save sperrt nur die Summary
 * (camel-folder-summary.c:2133); die Provider halten sie nicht über Netzzugriffe (imapx
 * camel-imapx-server.c:6090-6235 und 6525-6582 ohne Netz; microsoft365 camel-m365-folder.c:1187-1258). */

static void
flags_save_local_thread (GTask        *task,
                         gpointer      source_object G_GNUC_UNUSED,
                         gpointer      task_data,
                         GCancellable *cancellable G_GNUC_UNUSED)
{
  GPtrArray *jobs = task_data;

  for (guint i = 0; i < jobs->len; i++)
    {
      SetFlags *data = jobs->pdata[i];
      g_autofree char *uid = NULL;
      g_autoptr (CamelFolder) folder = real_folder (data->folder, data->uid, &uid);

      if (folder == NULL)
        continue;
      camel_folder_set_message_flags (folder, uid, data->mask, data->set);
      camel_folder_summary_save (camel_folder_get_folder_summary (folder), NULL);
    }
  g_task_return_boolean (task, TRUE);
}

static void
on_flags_saved_local (GObject      *source_object,
                      GAsyncResult *result G_GNUC_UNUSED,
                      gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionMailView *self = REVOLUTION_MAIL_VIEW (source_object);

  self->flags_saving = FALSE;
  g_debug ("Flags: lokal gespeichert, Beenden");
  hold_for_flags (self, FALSE);
}

gboolean
flags_deadline (gpointer user_data)
{
  RevolutionMailView *self = user_data;
  g_autoptr (GTask) task = NULL;
  GPtrArray *jobs = g_ptr_array_new_with_free_func ((GDestroyNotify) set_flags_free);
  SetFlags *data;

  self->flags_deadline_id = 0;
  self->flags_saving = TRUE;
  g_debug ("Flags: nach %d s noch nicht gespeichert, %u Aufträge nur lokal", FLAGS_DEADLINE_SECONDS,
           g_queue_get_length (&self->flag_jobs) + (self->running != NULL ? 1 : 0));
  while ((data = g_queue_pop_head (&self->flag_jobs)) != NULL)
    g_ptr_array_add (jobs, data);
  /* Den laufenden Auftrag ebenfalls: er kann noch vor dem Setzen auf die Sperre warten
   * (set_flags_thread); erneutes Setzen derselben Flags ändert nichts. Am vom Worker aufgelösten
   * echten Ordner, sonst (noch nicht aufgelöst) am Ordner des Auftrags. */
  if (self->running != NULL)
    {
      FlagsLocation *location = g_atomic_pointer_get (&((SetFlags *) self->running)->location);

      data = g_new0 (SetFlags, 1);
      data->folder = g_object_ref (location != NULL ? location->folder : self->running->folder);
      data->uid = g_strdup (location != NULL ? location->uid : self->running->uid);
      data->mask = self->running->mask;
      data->set = self->running->set;
      g_ptr_array_insert (jobs, 0, data);
    }

  task = g_task_new (self, NULL, on_flags_saved_local, NULL);
  g_task_set_source_tag (task, flags_deadline);
  g_task_set_task_data (task, jobs, (GDestroyNotify) g_ptr_array_unref);
  g_task_run_in_thread (task, flags_save_local_thread);

  return G_SOURCE_REMOVE;
}

static void
on_flags_set (GObject      *source_object,
              GAsyncResult *result,
              gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionMailView *self = REVOLUTION_MAIL_VIEW (source_object);
  SetFlags *data = g_task_get_task_data (G_TASK (result));
  g_autoptr (GError) error = NULL;
  g_autofree char *detail = NULL;
  gssize done = g_task_propagate_int (G_TASK (result), &error);

  self->flags_running = FALSE;
  /* Ansicht schon weg (Beenden): nichts mehr anzeigen, wartende Aufträge trotzdem abarbeiten.
   * #408: Synchronisieren ohne Cancellable (set_flags_thread); ein Abbruch von innen (microsoft365
   * beim Trennen) ist ein Fehler wie jeder andere. Die Änderung steht dann in der Summary und geht
   * beim nächsten Abgleich mit; offline genügt das Log. */
  if (data->transfer != NULL)
    on_transferred (self, data, error);
  else if (self->cancellable == NULL || g_cancellable_is_cancelled (self->cancellable))
    ;
  else if (error != NULL && self->session != NULL && !camel_session_get_online (self->session))
    g_debug ("Flags: %s %s %s lokal gespeichert (offline, Abgleich abgebrochen: %s)", data->loading, data->uid,
             data->what, error->message);
  else if (done == FLAGS_SAVED)
    g_debug ("Flags: %s %s %s gespeichert nach %.0f ms", data->loading, data->uid, data->what,
             (g_get_monotonic_time () - data->started) / 1000.0);
  else if (done == FLAGS_LOCAL)
    g_debug ("Flags: %s %s %s lokal gespeichert (offline)", data->loading, data->uid, data->what);
  else
    {
      g_warning ("Flags %s %s %s: %s", data->loading, data->uid, data->what, error->message);
      detail = g_strdup_printf ("%s: %s", gtk_label_get_label (self->list_title), error->message);
      revolution_mail_view_show_error (self, _("Flags Not Saved"), detail, FALSE);
    }
  self->running = NULL;
  start_flags (self);
  if (!self->flags_running && !self->flags_saving)
    hold_for_flags (self, FALSE);
  if (!self->flags_running && self->cancellable != NULL)
    start_folder_changes (self);
}

void
start_flags (RevolutionMailView *self)
{
  g_autoptr (GTask) task = NULL;

  if (self->flags_running || g_queue_is_empty (&self->flag_jobs))
    return;

  self->flags_running = TRUE;
  hold_for_flags (self, TRUE);
  task = g_task_new (self, self->cancellable, on_flags_set, NULL);
  g_task_set_source_tag (task, start_flags);
  g_task_set_task_data (task, g_queue_pop_head (&self->flag_jobs), (GDestroyNotify) set_flags_free);
  self->running = g_task_get_task_data (task);
  g_task_run_in_thread (task, set_flags_thread);
}

void
mark_message (RevolutionMailView    *self,
              RevolutionMailMessage *message,
              guint32                mask,
              guint32                set,
              const char            *what)
{
  SetFlags *data;
  GListModel *members = message != NULL ? revolution_mail_message_get_members (message) : NULL;

  if (message == NULL || self->camel_folder == NULL)
    return;

  /* #357: Bedienung an einer Konversation gilt wie in Evolution für alle Nachrichten eines
   * zugeklappten Threads (e_mail_reader_mark_selected mit e_mail_reader_get_selected_uids_with_collapsed_threads,
   * e-mail-reader-utils.c:1028-1065, e-mail-reader.c:4357-4363), hier für die, deren Zustand sich ändert. */
  if (members != NULL)
    {
      for (guint i = 0; i < g_list_model_get_n_items (members); i++)
        {
          g_autoptr (RevolutionMailMessage) member = g_list_model_get_item (members, i);
          gboolean change = ((mask & CAMEL_MESSAGE_SEEN) != 0 &&
                             revolution_mail_message_get_unread (member) == ((set & CAMEL_MESSAGE_SEEN) != 0)) ||
                            ((mask & CAMEL_MESSAGE_FLAGGED) != 0 &&
                             revolution_mail_message_get_starred (member) != ((set & CAMEL_MESSAGE_FLAGGED) != 0));

          if (change)
            mark_message (self, member, mask, set, what);
        }
      revolution_mail_message_sync_conversation (message);
      set_list_counts (self);
      return;
    }

  /* Ungelesen beendet den Gelesen-Timer wie Evolution (e-mail-reader.c:1187-1190). */
  if ((mask & CAMEL_MESSAGE_SEEN) != 0 && (set & CAMEL_MESSAGE_SEEN) == 0)
    g_clear_handle_id (&self->seen_id, g_source_remove);

  data = g_new0 (SetFlags, 1);
  data->folder = g_object_ref (self->camel_folder);
  data->uid = g_strdup (revolution_mail_message_get_uid (message));
  data->mask = mask;
  data->set = set;
  data->what = what;
  data->loading = g_strdup (self->loading);
  data->started = g_get_monotonic_time ();
  g_debug ("Flags: %s %s %s", data->loading, data->uid, what);

  /* Zeile, Aktionen und Zähler sofort; Camel bestätigt über „changed“ (apply_folder_changes). */
  revolution_mail_message_set_flags (message,
                                     (mask & CAMEL_MESSAGE_SEEN) != 0 ? (set & CAMEL_MESSAGE_SEEN) == 0
                                                                      : revolution_mail_message_get_unread (message),
                                     (mask & CAMEL_MESSAGE_FLAGGED) != 0 ? (set & CAMEL_MESSAGE_FLAGGED) != 0
                                                                         : revolution_mail_message_get_starred (message));
  /* #357: einzelne Nachricht der gewählten Konversation: Zeile der Konversation nachziehen. */
  if (self->selected != NULL && self->selected != message && revolution_mail_message_get_members (self->selected) != NULL)
    revolution_mail_message_sync_conversation (self->selected);
  set_list_counts (self);

  g_queue_push_tail (&self->flag_jobs, data);
  start_flags (self);
}

/* Gelesen und Ungelesen wie Evolution mail-mark-read/-unread (e-mail-reader.c:1171-1190,
 * Ungelesen hebt auch „gelöscht“ auf), Markieren wie mail-mark-important/-unimportant
 * (CAMEL_MESSAGE_FLAGGED). */
void
mark_read (RevolutionMailView    *self,
           RevolutionMailMessage *message,
           gboolean               read,
           const char            *what)
{
  mark_message (self, message, CAMEL_MESSAGE_SEEN | (read ? 0 : CAMEL_MESSAGE_DELETED),
                read ? CAMEL_MESSAGE_SEEN : 0, what);
}

void
mark_starred (RevolutionMailView    *self,
              RevolutionMailMessage *message,
              gboolean               starred,
              const char            *what)
{
  mark_message (self, message, CAMEL_MESSAGE_FLAGGED, starred ? CAMEL_MESSAGE_FLAGGED : 0, what);
}

RevolutionMailMessage *
selected_message (RevolutionMailView *self)
{
  return gtk_single_selection_get_selected_item (self->message_selection);
}
