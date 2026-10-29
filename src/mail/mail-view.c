/* mail-view.c – Mail-Ansicht (Prototyp #336)
 *
 * Vorlage: quelltext/stamp/src/views/mail/stamp-mail-view.c (Template-Klasse
 * mit Eigenschaft „stack“ für den Ansichtsumschalter). Oberfläche in
 * mail-view.blp; hier Template-Anbindung, seit #345 die Konten der Seitenleiste,
 * seit #348 ihre Ordner und seit #352 die Nachrichten des gewählten Ordners (Vorlage
 * stamp-conversation-list.c:610-955, Evolution message-list.c:6200-6380 und
 * e-mail-paned-view.c:690-693), seit #353 Gelesen/Markiert, Live-Änderungen und Aktualisieren
 * (Evolution e-mail-reader.c, message-list.c:5099-5250). Aktionen ohne Funktion sind nicht
 * registriert, ihre Knöpfe und Menüeinträge sind daher ausgegraut (#342).
 * #444 aufgeteilt: hier Suche, Konten, Ordnerbaum, Aktualisieren, Theme, Lebenszyklus, Einstiege aus main.c;
 * mail-list.c Nachrichtenliste, mail-queue.c Flags/Verschieben im Worker, mail-actions.c Nachrichtenaktionen,
 * mail-folder-actions.c Ordner-Menü, mail-view-private.h Struct und gemeinsame Funktionen.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <string.h>

#include <glib/gi18n.h>

#include "composer/composer.h"
#include "mail-favorites.h"
#include "mail-session.h"
#include "mail-search.h"
#include "mail-thread.h"
#include "mail-view-private.h"

G_DEFINE_FINAL_TYPE (RevolutionMailView, revolution_mail_view, ADW_TYPE_BREAKPOINT_BIN)

/* #421: Signal „retry-stores“ (UID des Kontos der offenen Liste oder NULL), main.c: gescheiterte Stores
 * erneut verbinden; TRUE, wenn das Konto der Liste dazugehört. */
static guint retry_signal;

/* list.compose öffnet den Composer (#337) als eigenes Fenster wie Stamp. #359: Absender = Konto des
 * gewählten Ordners (sonst die Standard-Identität, composer.c). */
static void
on_compose (GSimpleAction *action G_GNUC_UNUSED,
            GVariant      *parameter G_GNUC_UNUSED,
            gpointer       user_data)
{
  RevolutionMailView *self = user_data;
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  g_autoptr (RevolutionMailAccount) account = NULL;

  if (self->folder != NULL)
    g_object_get (self->folder, "account", &account, NULL);
  gtk_window_present (revolution_composer_new (gtk_window_get_application (GTK_WINDOW (root)), self->registry,
                                               self->session, REVOLUTION_COMPOSER_NEW,
                                               account != NULL ? revolution_mail_account_get_uid (account) : NULL,
                                               NULL, NULL, NULL));
}

static void on_refresh (GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_filter_change_state (GSimpleAction *action, GVariant *value, gpointer user_data);

static const GActionEntry list_actions[] = {
  { "compose", on_compose, NULL, NULL, NULL, { 0 } },
  { "refresh", on_refresh, NULL, NULL, NULL, { 0 } },
  { "context-menu", on_context_menu, NULL, NULL, NULL, { 0 } },
  { "filter", NULL, "s", "'all'", on_filter_change_state, { 0 } },
};

/* Fokus ins Suchfeld ohne alles zu markieren: sonst ersetzt beim Tippen in der
 * Liste (key-capture-widget) der zweite Buchstabe den ersten. */
static void
focus_search_entry (RevolutionMailView *self)
{
  GtkEditable *text = gtk_editable_get_delegate (GTK_EDITABLE (self->search_entry));

  gtk_text_grab_focus_without_selecting (GTK_TEXT (text));
}

/* #365: Leerzustand der Liste wie Evolution (message-list.c:6524-6531): während „Alle Ordner“ aufgebaut wird
 * „Suche läuft …“ (dort „Generating message list…“, Suchordner im Aufbau, e-mail-shell-view.c:117-121, 188),
 * mit Suche ohne Treffer „Keine Nachricht entspricht den Suchkriterien“, sonst „Keine Nachrichten“. */
void
set_empty_page (RevolutionMailView *self,
                gboolean            searching)
{
  adw_status_page_set_icon_name (self->empty_page, self->search != NULL ? "edit-find-symbolic" : "inbox-symbolic");
  adw_status_page_set_title (self->empty_page, searching          ? _("Searching…")
                                               : self->search != NULL ? _("No Messages Match the Search Criteria")
                                                                      : _("No Messages"));
}

/* #365: Suche aus Suchfeld, Suchbereich und Filter. Die Liste bleibt dieselbe: der gewählte Ordner wird neu
 * geladen (on_folder_selected), mit dem Ausdruck im aktuellen Ordner bzw. aus dem Suchordner „Alle Ordner“
 * (load_messages_thread). „Alle Ordner“ nur mit Suchtext, sonst der Ordner wie Evolution
 * (e-mail-shell-view.c:1404-1410). Endet die Suche (Escape, Filter „Alle“), wählt die Liste die vorher
 * gewählte Nachricht wieder. */
static void
update_search (RevolutionMailView *self)
{
  gboolean open = gtk_toggle_button_get_active (self->search_button);
  g_autofree char *text = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->search_entry))));
  g_autofree char *search = revolution_mail_search_expression (open ? text : NULL, self->filter);
  gboolean all = open && *text != '\0' && g_strcmp0 (adw_toggle_group_get_active_name (self->search_scope), "all") == 0;
  g_autofree char *keep = NULL;

  if (g_strcmp0 (search, self->search) == 0 && all == self->search_all)
    return;

  g_debug ("Suche: %s in %s", search != NULL ? search : "aus", all ? "allen Ordnern" : "Ordner");
  if (self->search == NULL)
    {
      RevolutionMailMessage *selected = gtk_single_selection_get_selected_item (self->message_selection);

      g_free (self->return_uid);
      self->return_uid = selected != NULL ? g_strdup (revolution_mail_message_get_uid (selected)) : NULL;
    }
  g_free (self->search);
  self->search = g_steal_pointer (&search);
  self->search_all = all;
  /* Während „Alle Ordner“ bleibt der Ordnerbaum gesperrt wie Evolution (e-mail-shell-view.c:1457): ein anderer
   * Ordner hätte die ganze Suche neu gestartet. */
  gtk_widget_set_sensitive (self->folder_list, !all);
  set_empty_page (self, FALSE);
  if (self->folder == NULL)
    return;

  /* on_folder_selected vergisst die Nachricht (Wechsel des Ordners), hier bleibt sie. */
  keep = g_steal_pointer (&self->return_uid);
  g_clear_object (&self->folder);
  on_folder_selected (self);
  self->return_uid = g_steal_pointer (&keep);
  self->returning = self->search == NULL && self->return_uid != NULL;
}

static void
on_filter_change_state (GSimpleAction *action,
                        GVariant      *value,
                        gpointer       user_data)
{
  RevolutionMailView *self = user_data;

  g_simple_action_set_state (action, value);
  g_free (self->filter);
  self->filter = g_variant_dup_string (value, NULL);
  update_search (self);
}

/* #399 Punkt 5: Suchknopf schaltet die Kopfleiste der Nachricht zwischen den
 * Aktionen und dem Suchfeld um („Lights off“), Fokus wandert mit. */
static void
on_search_toggled (RevolutionMailView *self)
{
  if (gtk_toggle_button_get_active (self->search_button))
    {
      gtk_stack_set_visible_child_name (self->header_stack, "search");
      focus_search_entry (self);
    }
  else
    {
      gtk_stack_set_visible_child_name (self->header_stack, "actions");
      gtk_editable_set_text (GTK_EDITABLE (self->search_entry), "");
      gtk_widget_grab_focus (GTK_WIDGET (self->search_button));
    }
  update_search (self);
}

/* #365: GtkSearchEntry::search-changed (nach search-delay) und Wechsel des Suchbereichs. */
static void
on_search_changed (RevolutionMailView *self)
{
  update_search (self);
}

/* Tippen in der Listenspalte (GtkSearchEntry::search-started) öffnet die Suche. */
static void
on_search_started (RevolutionMailView *self)
{
  gtk_toggle_button_set_active (self->search_button, TRUE);
}

/* Escape im Suchfeld (GtkSearchEntry::stop-search). */
static void
on_search_stop (RevolutionMailView *self)
{
  gtk_toggle_button_set_active (self->search_button, FALSE);
}

/* mail.search für Strg+F: öffnet nur, schließt nicht (Schließen: Escape, Suchknopf).
 * Ist die Suche schon offen, holt es den Fokus zurück ins Suchfeld. */
static void
on_search (GSimpleAction *action G_GNUC_UNUSED,
           GVariant      *parameter G_GNUC_UNUSED,
           gpointer       user_data)
{
  RevolutionMailView *self = user_data;

  if (gtk_toggle_button_get_active (self->search_button))
    focus_search_entry (self);
  else
    gtk_toggle_button_set_active (self->search_button, TRUE);
}

/* mail.toggle-sidebar fürs Hauptmenü (#401 Punkt 2): wie mail.sidebar (F9), aber
 * zustandslos, damit GtkPopoverMenu keine Haken-Spalte einrückt. */
static void
on_toggle_sidebar (GSimpleAction *action G_GNUC_UNUSED,
                   GVariant      *parameter G_GNUC_UNUSED,
                   gpointer       user_data)
{
  RevolutionMailView *self = user_data;

  adw_overlay_split_view_set_show_sidebar (self->split, !adw_overlay_split_view_get_show_sidebar (self->split));
}

static const GActionEntry mail_actions[] = {
  { "search", on_search, NULL, NULL, NULL, { 0 } },
  { "toggle-sidebar", on_toggle_sidebar, NULL, NULL, NULL, { 0 } },
};

/* Eintrag zum Konto mit dieser UID (neue Referenz) oder NULL. */
RevolutionMailAccount *
find_account (RevolutionMailView *self,
              const char         *uid)
{
  GListModel *model = G_LIST_MODEL (self->accounts);

  for (guint i = 0; i < g_list_model_get_n_items (model); i++)
    {
      g_autoptr (RevolutionMailAccount) account = g_list_model_get_item (model, i);

      if (g_strcmp0 (revolution_mail_account_get_uid (account), uid) == 0)
        return g_steal_pointer (&account);
    }

  /* #351: Favoriten für Erstauswahl und Zähler (select_initial_folder, revolution_mail_view_watch_folder). */
  if (self->favorites != NULL && g_strcmp0 (revolution_mail_account_get_uid (self->favorites), uid) == 0)
    return g_object_ref (self->favorites);
  /* #362: Postausgang (Erstauswahl, Zähler). */
  if (self->local != NULL && g_strcmp0 (revolution_mail_account_get_uid (self->local), uid) == 0)
    return g_object_ref (self->local);

  return NULL;
}

/* #345: Konten aus der Registry in den Store, nur im UI-Thread (Registry-Signale kommen
 * dort an, docs/ENTWICKLUNG.md „Worker“). Reihenfolge alphabetisch nach Anzeigename wie Evolutions
 * Standard (e-mail-account-store.c:119-153, g_utf8_collate); e_source_registry_list_sources
 * sortiert schon so (EDS e-source-registry.c:2474-2476, e-source.c:3368-3375). Evolutions
 * eigene Reihenfolge (sortorder.ini, e-mail-account-store.c:565) liest Revolution nicht
 * (Architektur-Festlegung 4). Bleiben die Konten gleich, ändert sich nur der Name: die
 * Einträge und damit Auswahl und Zeilen bleiben erhalten. */
static void
sync_accounts (RevolutionMailView *self)
{
  g_autolist (ESource) sources = e_source_registry_list_sources (self->registry, E_SOURCE_EXTENSION_MAIL_ACCOUNT);
  g_autoptr (GPtrArray) shown = g_ptr_array_new ();
  GListModel *model = G_LIST_MODEL (self->accounts);
  guint n_items = g_list_model_get_n_items (model);
  gboolean same;

  for (GList *l = sources; l != NULL; l = l->next)
    {
      const char *reason = revolution_mail_account_hidden_reason (self->registry, l->data);
      g_autofree char *name = e_source_dup_display_name (l->data);

      g_debug ("Mail-Konto %s „%s“: %s", e_source_get_uid (l->data), name, reason ? reason : "angezeigt");
      if (reason == NULL)
        g_ptr_array_add (shown, l->data);
    }

  same = shown->len == n_items;
  for (guint i = 0; same && i < n_items; i++)
    {
      g_autoptr (RevolutionMailAccount) account = g_list_model_get_item (model, i);

      same = g_strcmp0 (revolution_mail_account_get_uid (account), e_source_get_uid (shown->pdata[i])) == 0;
    }

  if (same)
    {
      for (guint i = 0; i < n_items; i++)
        {
          g_autoptr (RevolutionMailAccount) account = g_list_model_get_item (model, i);
          g_autofree char *name = e_source_dup_display_name (shown->pdata[i]);

          revolution_mail_account_set_display_name (account, name);
          revolution_mail_account_update_note (account, shown->pdata[i]);
        }
    }
  else
    {
      g_autoptr (GPtrArray) accounts = g_ptr_array_new_with_free_func (g_object_unref);

      for (guint i = 0; i < shown->len; i++)
        {
          g_autofree char *name = e_source_dup_display_name (shown->pdata[i]);
          RevolutionMailAccount *account = find_account (self, e_source_get_uid (shown->pdata[i]));

          /* #348: vorhandene Einträge behalten, sonst gingen ihre geladenen Ordner verloren. */
          if (account != NULL)
            revolution_mail_account_set_display_name (account, name);
          else
            account = revolution_mail_account_new (e_source_get_uid (shown->pdata[i]), name);
          /* #366: Hinweis microsoft365 ohne Push (Tooltip des Abschnittskopfs). */
          revolution_mail_account_update_note (account, shown->pdata[i]);
          g_ptr_array_add (accounts, account);
        }
      g_list_store_splice (self->accounts, 0, n_items, accounts->pdata, accounts->len);
    }

  g_debug ("Seitenleiste: %u von %u Mail-Konten angezeigt", shown->len, g_list_length (sources));
  gtk_stack_set_visible_child_name (self->sidebar_stack, shown->len > 0 ? "list" : "empty");
}

/* create_func des GtkTreeListModel (Vorlage Tag m2-prototyp mail-sample.c:217-225):
 * Unterordner oder NULL (kein Ausklapp-Pfeil). */
static GListModel *
folder_children (gpointer item,
                 gpointer user_data G_GNUC_UNUSED)
{
  GListModel *children = revolution_mail_folder_get_children (item);

  return children != NULL ? g_object_ref (children) : NULL;
}

/* #348: Je Konto ein GtkTreeListModel über seinen Ordnern, am Konto-Eintrag gehalten
 * (sync_accounts behält die Einträge). autoexpand wie der Prototyp (mail-sample.c:236). */
GtkTreeListModel *
account_tree (RevolutionMailAccount *account)
{
  GtkTreeListModel *tree = g_object_get_data (G_OBJECT (account), "folder-tree");

  if (tree == NULL)
    {
      tree = gtk_tree_list_model_new (G_LIST_MODEL (g_list_store_new (REVOLUTION_TYPE_MAIL_FOLDER)),
                                      FALSE, TRUE, folder_children, NULL, NULL);
      g_object_set_data_full (G_OBJECT (account), "folder-tree", tree, g_object_unref);
    }

  return tree;
}

/* map_func des GtkMapListModel: Konto → sein Ordnerbaum. Das GtkFlattenListModel darüber
 * macht daraus je Konto einen Abschnitt (GtkSectionModel) mit dem Kontonamen als Kopf
 * (mail-view.blp), wie der Prototyp (mail-sample.c:241-245). Konten ohne Ordner (noch nicht
 * geladen, POP, Nur-Versand) haben keinen Abschnitt. */
static gpointer
account_folders (gpointer item,
                 gpointer user_data G_GNUC_UNUSED)
{
  GtkTreeListModel *tree = account_tree (item);

  g_object_ref (tree);
  g_object_unref (item);

  return tree;
}

/* #408: Ordner eines Kontos nach „Netz zurück“ (main.c, on_store_noted), im UI-Thread: Baum
 * abgleichen statt ersetzen (revolution_mail_folder_merge_tree), Auswahl und Aufklappzustand
 * bleiben. Ein inzwischen ausgeblendetes Konto wird übergangen. */
void
revolution_mail_view_update_folders (RevolutionMailView *self,
                                     const char         *uid,
                                     CamelFolderInfo    *info)
{
  g_autoptr (RevolutionMailAccount) account = NULL;
  guint added = 0, removed = 0;

  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (self));

  account = find_account (self, uid);
  if (account == NULL)
    return;

  revolution_mail_folder_merge_tree (G_LIST_STORE (gtk_tree_list_model_get_model (account_tree (account))),
                                     info, account, &added, &removed);
  g_debug ("Ordner: %s abgeglichen, %u neu, %u entfernt", uid, added, removed);
}

/* #353: Ordner neu vom Server (list.refresh, F5 wie Evolution mail-folder-refresh,
 * e-mail-shell-view-actions.c:1721-1726; Strg+R ist dort Antworten, e-mail-reader.c:3382-3387).
 * camel_folder_refresh_info_sync im Worker, die Liste folgt über „changed“. Ohne geladenen
 * Ordner (Fehler beim Laden) lädt es den Ordner neu. */
/* #421: Store des Ordners offline (main.c schaltet ihn nach gescheiterter Verbindung offline), bei
 * Suchordnern (Konversation, Favoriten) der Store eines Quellordners; wie load_messages_thread. */
static gboolean
folder_store_offline (CamelFolder *folder)
{
  CamelStore *store = camel_folder_get_parent_store (folder);
  gboolean offline = FALSE;

  if (CAMEL_IS_VEE_FOLDER (folder))
    {
      GList *sources = camel_vee_folder_ref_folders (CAMEL_VEE_FOLDER (folder));

      for (GList *l = sources; l != NULL && !offline; l = l->next)
        offline = folder_store_offline (l->data);
      g_list_free_full (sources, g_object_unref);
    }
  else if (CAMEL_IS_OFFLINE_STORE (store))
    offline = !camel_offline_store_get_online (CAMEL_OFFLINE_STORE (store));

  return offline;
}

static void
refresh_thread (GTask        *task,
                gpointer      source_object G_GNUC_UNUSED,
                gpointer      task_data,
                GCancellable *cancellable)
{
  GError *error = NULL;
  CamelStore *store = camel_folder_get_parent_store (task_data);
  g_autoptr (CamelSession) session = store != NULL ? camel_service_ref_session (CAMEL_SERVICE (store)) : NULL;
  /* #408: eigenes Cancellable je Vorgang wie load_messages_thread. */
  g_autoptr (GCancellable) op = camel_operation_new_proxy (cancellable);

  /* #357: in Konversationen den Ordner und Gesendet am Server abgleichen (mail-thread.c). */
  if (revolution_mail_thread_refresh_sync (task_data, op, &error))
    g_task_return_boolean (task, TRUE);
  else if ((session != NULL && !camel_session_get_online (session)) || folder_store_offline (task_data))
    {
      /* #408: offline bleibt der Cache (wie load_messages_thread). */
      g_debug ("Aktualisieren: offline, Stand des Caches (%s)", error->message);
      g_error_free (error);
      g_task_return_boolean (task, TRUE);
    }
  else
    g_task_return_error (task, error);
}

static void
on_refreshed (GObject      *source_object,
              GAsyncResult *result,
              gpointer      user_data)
{
  RevolutionMailView *self = REVOLUTION_MAIL_VIEW (source_object);
  g_autofree char *loading = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *detail = NULL;

  if (g_task_propagate_boolean (G_TASK (result), &error))
    {
      g_debug ("Aktualisieren: %s fertig", loading);
      return;
    }
  /* #408: still nur, wenn wir abgebrochen haben (wie on_messages_loaded). */
  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
      g_cancellable_is_cancelled (g_task_get_cancellable (G_TASK (result))))
    {
      g_debug ("Aktualisieren: %s abgebrochen", loading);
      return;
    }

  g_warning ("Aktualisieren %s: %s", loading, error->message);
  detail = g_strdup_printf ("%s: %s", gtk_label_get_label (self->list_title), error->message);
  revolution_mail_view_show_error (self, _("Refresh Failed"), detail, FALSE);
}

static void
refresh_list (RevolutionMailView *self)
{
  g_autoptr (GTask) task = NULL;

  if (self->camel_folder == NULL)
    {
      g_clear_object (&self->folder);
      on_folder_selected (self);
      return;
    }

  g_debug ("Aktualisieren: %s", self->loading);
  task = g_task_new (self, self->load_cancellable, on_refreshed, g_strdup (self->loading));
  g_task_set_source_tag (task, on_refresh);
  g_task_set_task_data (task, g_object_ref (self->camel_folder), g_object_unref);
  g_task_run_in_thread (task, refresh_thread);
}

/* #421: F5 verbindet zuerst die Stores, deren Verbindung bei Netzmonitor „online“ scheiterte (main.c,
 * retry_stores), wie Evolution einen getrennten Store beim nächsten Zugriff neu verbindet
 * (camel-m365-store.c:1754-1762, camel-store.c:262-313). Gehört das Konto der Liste dazu, folgt die
 * Liste nach dem Verbinden (revolution_mail_view_refresh_account); sonst gleich. */
static void
on_refresh (GSimpleAction *action G_GNUC_UNUSED,
            GVariant      *parameter G_GNUC_UNUSED,
            gpointer       user_data)
{
  RevolutionMailView *self = user_data;
  g_autoptr (RevolutionMailAccount) account = NULL;
  gboolean waits = FALSE;

  if (self->folder != NULL)
    g_object_get (self->folder, "account", &account, NULL);
  g_signal_emit (self, retry_signal, 0, account != NULL ? revolution_mail_account_get_uid (account) : NULL, &waits);
  if (waits)
    g_debug ("Aktualisieren: %s nach erneutem Verbinden", self->loading);
  else
    refresh_list (self);
}

/* #352: Nur für Testläufe (tests/nachrichtenliste.sh) wie REVOLUTION_THEME (main.c):
 * REVOLUTION_FRAME_LOG=1 schreibt je Frame des Fensters die Dauer von flush-events bzw.
 * before-paint bis after-paint (Ereignisse, Layout, Zeichnen; GdkFrameClock-Phasen) ins
 * Debug-Log, als Beleg für flüssiges Scrollen. */
static void
on_frame_start (RevolutionMailView *self)
{
  if (self->frame_start == 0)
    self->frame_start = g_get_monotonic_time ();
}

static void
on_frame_end (RevolutionMailView *self)
{
  if (self->frame_start != 0)
    g_debug ("Frame: %.2f ms", (g_get_monotonic_time () - self->frame_start) / 1000.0);
  self->frame_start = 0;
}

static void
on_message_list_realize (RevolutionMailView *self)
{
  GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (self->message_list));

  g_signal_connect_object (clock, "flush-events", G_CALLBACK (on_frame_start), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (clock, "before-paint", G_CALLBACK (on_frame_start), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (clock, "after-paint", G_CALLBACK (on_frame_end), self, G_CONNECT_SWAPPED);
}

/* #401 Punkt 1: Klasse .dark für Regeln nur im dunklen Design (style.css). */
static void
on_dark_changed (AdwStyleManager    *manager,
                 GParamSpec         *pspec G_GNUC_UNUSED,
                 RevolutionMailView *self)
{
  if (adw_style_manager_get_dark (manager))
    gtk_widget_add_css_class (GTK_WIDGET (self), "dark");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "dark");
}

/* #357: Umschalter Konversation/flach (list.conversations, Strg+T wie Evolution „Nach Threads gruppieren“,
 * e-mail-shell-view-actions.c:1922-1927): gewählten Ordner neu laden. */
static void
on_conversations_changed (RevolutionMailView *self)
{
  g_debug ("Konversationen: %s", g_settings_get_boolean (self->settings, "conversations") ? "an" : "aus");
  if (self->folder == NULL)
    return;

  g_clear_object (&self->folder);
  on_folder_selected (self);
  /* Der Fokus lag evtl. im Lesebereich, der jetzt leer ist: zurück in die Liste, damit Kürzel (Strg+T)
   * weiter wirken (gemessen: zweites Strg+T ohne Wirkung, build/m4-357/lauf1). */
  gtk_widget_grab_focus (GTK_WIDGET (self->message_list));
}

enum { PROP_0, PROP_STACK, N_PROPS };

static void
revolution_mail_view_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  RevolutionMailView *self = REVOLUTION_MAIL_VIEW (object);

  if (prop_id == PROP_STACK)
    g_value_set_object (value, self->stack);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
revolution_mail_view_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  RevolutionMailView *self = REVOLUTION_MAIL_VIEW (object);

  if (prop_id == PROP_STACK)
    g_set_object (&self->stack, g_value_get_object (value));
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static const GActionEntry folder_actions[] = {
  { "new", on_folder_edit, NULL, NULL, NULL, { 0 } },
  { "rename", on_folder_edit, NULL, NULL, NULL, { 0 } },
  { "delete", on_folder_edit, NULL, NULL, NULL, { 0 } },
};

static void
revolution_mail_view_dispose (GObject *object)
{
  RevolutionMailView *self = REVOLUTION_MAIL_VIEW (object);

  /* Blöcke, die noch kommen, verwerfen (add_messages_block). */
  self->load_generation++;
  g_cancellable_cancel (self->load_cancellable);
  g_clear_object (&self->load_cancellable);
  /* #353: laufende Flag-Aufträge dürfen zu Ende laufen (Anwendung gehalten), höchstens
   * FLAGS_DEADLINE_SECONDS lang ab dem Schließen (#409: revolution_mail_view_unroot). */
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_signal_handlers_disconnect_by_data (self->message_selection, self);
  g_clear_handle_id (&self->seen_id, g_source_remove);
  if (self->member_seen != NULL)
    clear_member_seen (self);
  g_clear_pointer (&self->member_seen, g_ptr_array_unref);
  g_clear_signal_handler (&self->selected_handler, self->selected);
  g_clear_object (&self->selected);
  g_clear_object (&self->camel_folder);
  g_clear_object (&self->thread_folder);
  g_clear_pointer (&self->pending_changed, g_hash_table_unref);
  g_clear_pointer (&self->pending_removed, g_hash_table_unref);
  g_clear_object (&self->settings);
  g_clear_pointer (&self->row_popover, gtk_widget_unparent);
  g_clear_pointer (&self->folder_popover, gtk_widget_unparent);
  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_VIEW);
  g_clear_object (&self->stack);
  g_clear_object (&self->registry);
  g_clear_object (&self->accounts);
  g_clear_object (&self->favorites);
  g_clear_object (&self->local);
  g_clear_object (&self->local_section);
  g_clear_object (&self->messages);
  g_clear_object (&self->folder);
  g_clear_object (&self->session);
  g_clear_pointer (&self->loading, g_free);
  g_clear_pointer (&self->search, g_free);
  g_clear_pointer (&self->filter, g_free);
  g_clear_pointer (&self->return_uid, g_free);
  g_clear_pointer (&self->show_folder, g_free);
  g_clear_pointer (&self->show_uid, g_free);
  g_clear_pointer (&self->move_account, g_free);
  g_clear_pointer (&self->undo, g_ptr_array_unref);
  g_clear_object (&self->toast);

  G_OBJECT_CLASS (revolution_mail_view_parent_class)->dispose (object);
}

/* #409: Obergrenze schon beim Schließen des Fensters (GtkWindow nimmt beim Zerstören sein Kind aus dem
 * Baum, unroot kommt vor dispose): dispose läuft erst, wenn kein Worker-GTask (Flag-Auftrag, Laden)
 * die Ansicht mehr hält. Den Gelesen-Timer wie in dispose beenden, sonst käme nach dem Schließen ein
 * Auftrag ohne Obergrenze dazu. g_timeout_add statt _seconds (gemessen damit 10,5 bzw. 10,8 s bis zum
 * Prozessende, #409). */
static void
revolution_mail_view_unroot (GtkWidget *widget)
{
  RevolutionMailView *self = REVOLUTION_MAIL_VIEW (widget);

  g_clear_handle_id (&self->seen_id, g_source_remove);
  if (self->flags_running && self->flags_deadline_id == 0)
    self->flags_deadline_id = g_timeout_add (FLAGS_DEADLINE_SECONDS * 1000, flags_deadline, self);

  GTK_WIDGET_CLASS (revolution_mail_view_parent_class)->unroot (widget);
}

static void
revolution_mail_view_finalize (GObject *object)
{
  RevolutionMailView *self = REVOLUTION_MAIL_VIEW (object);

  g_queue_clear_full (&self->flag_jobs, (GDestroyNotify) set_flags_free);

  G_OBJECT_CLASS (revolution_mail_view_parent_class)->finalize (object);
}

static void
revolution_mail_view_class_init (RevolutionMailViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = revolution_mail_view_get_property;
  object_class->set_property = revolution_mail_view_set_property;
  object_class->dispose = revolution_mail_view_dispose;
  object_class->finalize = revolution_mail_view_finalize;
  widget_class->unroot = revolution_mail_view_unroot;

  retry_signal = g_signal_new ("retry-stores", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0,
                               g_signal_accumulator_first_wins, NULL, NULL, G_TYPE_BOOLEAN, 1, G_TYPE_STRING);

  g_object_class_install_property (object_class, PROP_STACK,
                                   g_param_spec_object ("stack", NULL, NULL, ADW_TYPE_VIEW_STACK,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_type_ensure (REVOLUTION_TYPE_MAIL_ACCOUNT);
  g_type_ensure (REVOLUTION_TYPE_MAIL_FOLDER);
  g_type_ensure (REVOLUTION_TYPE_MAIL_MESSAGE);
  g_type_ensure (REVOLUTION_TYPE_MAIL_READER);
  g_type_ensure (REVOLUTION_TYPE_MAIL_CONVERSATION);

  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/mail-view.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, folder_selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, sidebar_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, banner);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, offline_notice);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, message_selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, list_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, list_title);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, list_subtitle);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, message_list);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, header_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, search_entry);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, search_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, search_scope);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, empty_page);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, folder_list);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, split);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, row_menu);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, folder_menu);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, message_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, reader);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, conversation_view);
  gtk_widget_class_bind_template_callback (widget_class, on_search_toggled);
  gtk_widget_class_bind_template_callback (widget_class, on_row_secondary_click);
  gtk_widget_class_bind_template_callback (widget_class, on_folder_secondary_click);
  gtk_widget_class_bind_template_callback (widget_class, on_row_swipe);
  gtk_widget_class_bind_template_callback (widget_class, on_search_started);
  gtk_widget_class_bind_template_callback (widget_class, on_search_stop);
  gtk_widget_class_bind_template_callback (widget_class, on_message_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_search_changed);
  /* #363 */
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, move_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, move_menu);
  gtk_widget_class_bind_template_callback (widget_class, on_row_drag_prepare);
  gtk_widget_class_bind_template_callback (widget_class, on_folder_drop);
}

/* #353: Aktionen der gewählten Nachricht (update_message_actions); Kürzel in mail-view.blp. */
static const GActionEntry message_actions[] = {
  { "mark-read", on_mark_read, NULL, NULL, NULL, { 0 } },
  { "mark-unread", on_mark_unread, NULL, NULL, NULL, { 0 } },
  { "star-on", on_star_on, NULL, NULL, NULL, { 0 } },
  { "star-off", on_star_off, NULL, NULL, NULL, { 0 } },
  { "star", NULL, NULL, "false", on_star_change_state, { 0 } },
  { "reply", on_reply, NULL, NULL, NULL, { 0 } },
  { "reply-all", on_reply_all, NULL, NULL, NULL, { 0 } },
  { "forward", on_forward, NULL, NULL, NULL, { 0 } },
  { "open", on_open, NULL, NULL, NULL, { 0 } },
  /* #363 */
  { "delete", on_transfer_action, NULL, NULL, NULL, { 0 } },
  { "archive", on_transfer_action, NULL, NULL, NULL, { 0 } },
  { "junk", on_transfer_action, NULL, NULL, NULL, { 0 } },
  { "move", on_transfer_action, NULL, NULL, NULL, { 0 } },
  { "move-to", on_transfer_action, "s", NULL, NULL, { 0 } },
  { "undo", on_transfer_action, NULL, NULL, NULL, { 0 } },
  /* #370 */
  { "add-sender", on_add_sender, NULL, NULL, NULL, { 0 } },
};

static void
revolution_mail_view_init (RevolutionMailView *self)
{
  g_autoptr (GtkFlattenListModel) folders = NULL;
  g_autoptr (GListStore) sections = g_list_store_new (G_TYPE_LIST_MODEL);
  g_autoptr (GListStore) favorites = g_list_store_new (REVOLUTION_TYPE_MAIL_ACCOUNT);
  g_autoptr (GSimpleActionGroup) message_group = g_simple_action_group_new ();
  g_autoptr (GSimpleActionGroup) list_group = g_simple_action_group_new ();
  g_autoptr (GSimpleActionGroup) mail_group = g_simple_action_group_new ();
  g_autoptr (GSimpleActionGroup) folder_group = g_simple_action_group_new ();
  g_autoptr (GPropertyAction) sidebar_action = NULL;
  AdwStyleManager *style_manager = adw_style_manager_get_default ();

  gtk_widget_init_template (GTK_WIDGET (self));
  /* #365: Suchbereich anfangs der gewählte Ordner: sucht nur dort, ohne alle Ordner aller Konten zu öffnen. */
  adw_toggle_group_set_active_name (self->search_scope, "folder");

  /* CR #397: Ordnerbaum als GtkTreeListModel je Konto (#348), Konten als Abschnitte (#345). */
  self->accounts = g_list_store_new (REVOLUTION_TYPE_MAIL_ACCOUNT);
  /* #351: Favoriten als erster Abschnitt vor den Konten (Prototyp), außerhalb von self->accounts
   * (sync_accounts gleicht nur die Konten der Registry ab); Ordner sind gleich da. */
  self->favorites = revolution_mail_account_new (REVOLUTION_MAIL_FAVORITES_UID, _("Favorites"));
  revolution_mail_favorites_fill (G_LIST_STORE (gtk_tree_list_model_get_model (account_tree (self->favorites))),
                                  self->favorites);
  g_object_set_data (G_OBJECT (self->favorites), "folders-noted", GINT_TO_POINTER (TRUE));
  g_list_store_append (favorites, self->favorites);
  g_list_store_append (sections, favorites);
  /* #362: Postausgang wie Evolution im lokalen Store „Auf diesem Rechner“ (e-mail-session.c:824-894,
   * em-folder-tree-model.c:1534-1538), Name wie dort „Outbox“ mit eigenem Typ; erst sichtbar, wenn er
   * Nachrichten hat (revolution_mail_view_set_outbox). */
  self->local = revolution_mail_account_new (REVOLUTION_MAIL_LOCAL_UID, _("On This Computer"));
  {
    CamelFolderInfo *fi = camel_folder_info_new ();

    fi->full_name = g_strdup ("Outbox");
    fi->display_name = g_strdup (_("Outbox"));
    fi->flags = CAMEL_FOLDER_TYPE_OUTBOX | CAMEL_FOLDER_NOCHILDREN;
    revolution_mail_folder_append_tree (G_LIST_STORE (gtk_tree_list_model_get_model (account_tree (self->local))),
                                        fi, self->local);
    camel_folder_info_free (fi);
  }
  g_object_set_data (G_OBJECT (self->local), "folders-noted", GINT_TO_POINTER (TRUE));
  self->local_section = g_list_store_new (REVOLUTION_TYPE_MAIL_ACCOUNT);
  g_list_store_append (sections, self->local_section);
  g_list_store_append (sections, self->accounts);
  folders = gtk_flatten_list_model_new (G_LIST_MODEL (gtk_map_list_model_new (
    G_LIST_MODEL (gtk_flatten_list_model_new (G_LIST_MODEL (g_steal_pointer (&sections)))), account_folders, NULL, NULL)));
  gtk_single_selection_set_model (self->folder_selection, G_LIST_MODEL (folders));
  g_signal_connect_object (self->folder_selection, "notify::selected-item", G_CALLBACK (on_folder_selected),
                           self, G_CONNECT_SWAPPED);
  /* #352: Nachrichten des gewählten Ordners (on_folder_selected). */
  self->messages = g_list_store_new (REVOLUTION_TYPE_MAIL_MESSAGE);
  gtk_single_selection_set_model (self->message_selection, G_LIST_MODEL (self->messages));
  g_signal_connect_object (self->message_selection, "notify::selected-item", G_CALLBACK (on_message_selected),
                           self, G_CONNECT_SWAPPED);
  if (g_getenv ("REVOLUTION_FRAME_LOG") != NULL)
    g_signal_connect_object (self->message_list, "realize", G_CALLBACK (on_message_list_realize), self,
                             G_CONNECT_SWAPPED);

  g_action_map_add_action_entries (G_ACTION_MAP (list_group), list_actions, G_N_ELEMENTS (list_actions), self);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "list", G_ACTION_GROUP (list_group));
  g_action_map_add_action_entries (G_ACTION_MAP (mail_group), mail_actions, G_N_ELEMENTS (mail_actions), self);
  /* mail.sidebar für F9 wie calendar.sidebar (calendar-view.c:426-431). */
  sidebar_action = g_property_action_new ("sidebar", self->split, "show-sidebar");
  g_action_map_add_action (G_ACTION_MAP (mail_group), G_ACTION (sidebar_action));
  gtk_widget_insert_action_group (GTK_WIDGET (self), "mail", G_ACTION_GROUP (mail_group));

  /* #353: Gelesen, Markieren, Live-Änderungen, Kontextmenü der Zeile. */
  self->settings = g_settings_new ("de.freiraum_medien.Revolution");
  self->cancellable = g_cancellable_new ();
  self->pending_changed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->pending_removed = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_action_map_add_action_entries (G_ACTION_MAP (message_group), message_actions, G_N_ELEMENTS (message_actions), self);
  self->message_actions = G_ACTION_MAP (message_group);
  gtk_menu_button_set_create_popup_func (self->move_button, on_move_popup, self, NULL);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "message", G_ACTION_GROUP (message_group));
  update_message_actions (self);
  g_signal_connect_object (self->reader, "shown", G_CALLBACK (on_reader_shown), self, G_CONNECT_SWAPPED);
  /* #357: Konversationen: Umschalter (GSettings-Aktion, Haken im Filter-Menü), Lesebereich. */
  {
    g_autoptr (GAction) conversations = g_settings_create_action (self->settings, "conversations");

    g_action_map_add_action (G_ACTION_MAP (list_group), conversations);
  }
  g_signal_connect_object (self->settings, "changed::conversations", G_CALLBACK (on_conversations_changed), self,
                           G_CONNECT_SWAPPED);
  self->member_seen = g_ptr_array_new_with_free_func ((GDestroyNotify) g_source_unref);
  g_signal_connect_object (self->conversation_view, "shown", G_CALLBACK (on_conversation_shown), self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->conversation_view, "mark", G_CALLBACK (on_conversation_mark), self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->message_selection, "notify::selected-item", G_CALLBACK (watch_selected_message),
                           self, G_CONNECT_SWAPPED);
  self->row_popover = gtk_popover_menu_new_from_model (self->row_menu);
  gtk_popover_set_has_arrow (GTK_POPOVER (self->row_popover), FALSE);
  gtk_widget_set_halign (self->row_popover, GTK_ALIGN_START);
  gtk_widget_set_parent (self->row_popover, GTK_WIDGET (self->message_list));
  /* #364 */
  g_action_map_add_action_entries (G_ACTION_MAP (folder_group), folder_actions, G_N_ELEMENTS (folder_actions), self);
  self->folder_actions = G_ACTION_MAP (folder_group);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "folder", G_ACTION_GROUP (folder_group));
  update_folder_actions (self);
  self->folder_popover = gtk_popover_menu_new_from_model (self->folder_menu);
  gtk_popover_set_has_arrow (GTK_POPOVER (self->folder_popover), FALSE);
  gtk_widget_set_halign (self->folder_popover, GTK_ALIGN_START);
  gtk_widget_set_parent (self->folder_popover, self->folder_list);
  {
    GtkEventController *keys = gtk_shortcut_controller_new ();

    gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (keys),
                                          gtk_shortcut_new (gtk_shortcut_trigger_parse_string ("Delete|KP_Delete|BackSpace"),
                                                            gtk_callback_action_new (on_folder_delete_key, self, NULL)));
    gtk_widget_add_controller (self->folder_list, keys);
  }

  g_signal_connect_object (style_manager, "notify::dark", G_CALLBACK (on_dark_changed), self, G_CONNECT_DEFAULT);
  on_dark_changed (style_manager, NULL, self);
}

/* #345: Aufruf aus main.c, sobald die Registry im Worker geladen ist (UI-Thread).
 * Live wie Evolution: neue, entfernte, (de)aktivierte Quellen (e-mail-session.c:1268-1288)
 * und Umbenennen (Bindung an display-name, e-mail-session.c:1410-1413) aktualisieren die
 * Liste. Den Namen meldet hier „source-changed“ im UI-Thread (EDS e-source-registry.c:
 * 580-598); ESource:display-name selbst ändert sich im D-Bus-Thread der Registry
 * (e-source-registry.c:1051, e-source.c:792-803) und wird daher nicht gebunden. */
void
revolution_mail_view_set_registry (RevolutionMailView *self,
                                   ESourceRegistry    *registry)
{
  const char *signals[] = { "source-added", "source-removed", "source-enabled", "source-disabled", "source-changed" };

  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (self));
  g_return_if_fail (E_IS_SOURCE_REGISTRY (registry));
  g_return_if_fail (self->registry == NULL);

  self->registry = g_object_ref (registry);
  for (gsize i = 0; i < G_N_ELEMENTS (signals); i++)
    g_signal_connect_object (registry, signals[i], G_CALLBACK (sync_accounts), self, G_CONNECT_SWAPPED);
  sync_accounts (self);
}

/* #352: Aufruf aus main.c, sobald die CamelSession im Worker angelegt ist (UI-Thread). Die
 * Stores der Konten holt das Laden der Nachrichten im Worker (load_messages_thread). */
void
revolution_mail_view_set_session (RevolutionMailView *self,
                                  CamelSession       *session)
{
  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (self));
  g_return_if_fail (CAMEL_IS_SESSION (session));

  g_set_object (&self->session, session);
}

/* #343/#345/#347: Fehler im AdwBanner der Mail-Ansicht (mail-view.blp), keine Dialoge
 * (Architektur-Festlegung 3). #352: aus main.c hierher verlegt, weil auch das Laden der
 * Nachrichten Fehler meldet. Ein Banner für alle Fehler: Titel nach der häufigsten
 * Fehlerart (bei Gleichstand die erste), je Fehler eine Zeile im Tooltip. Frühere Fehler
 * (z. B. „Datenordner fehlt“) bleiben im Tooltip erhalten, statt überschrieben zu werden.
 * #420: Verbindungsfehler gehören einem Konto (uid): höchstens einer je Konto, er verschwindet,
 * sobald der Store des Kontos verbunden ist (revolution_mail_view_clear_account_errors). */
typedef struct
{
  const char *title;
  char *detail;
  char *uid;
  gboolean per_account;
} BannerError;

static void
banner_error_free (BannerError *error)
{
  g_free (error->detail);
  g_free (error->uid);
  g_free (error);
}

/* Banner nach den Fehlern in „banner-errors“ zeigen bzw. ausblenden; removed: Titel des zuletzt
 * entfernten Fehlers (Log). */
static void
update_banner (RevolutionMailView *self,
               const char         *removed)
{
  GPtrArray *errors = g_object_get_data (G_OBJECT (self), "banner-errors");
  g_autoptr (GString) details = g_string_new (NULL);
  g_autofree char *shown = NULL;
  BannerError *best = NULL;
  guint best_count = 0;

  if (errors == NULL || errors->len == 0)
    {
      g_object_set_data (G_OBJECT (self), "banner-errors", NULL);
      adw_banner_set_revealed (self->banner, FALSE);
      g_debug ("Banner: %s weg", removed);
      return;
    }

  for (guint i = 0; i < errors->len; i++)
    {
      BannerError *error = g_ptr_array_index (errors, i);
      guint count = 0;

      if (i > 0)
        g_string_append_c (details, '\n');
      g_string_append (details, error->detail);
      for (guint j = 0; j < errors->len; j++)
        count += g_strcmp0 (((BannerError *) g_ptr_array_index (errors, j))->title, error->title) == 0;
      if (count > best_count)
        {
          best = error;
          best_count = count;
        }
    }

  if (best->per_account && best_count > 1)
    /* Translators: title of the error banner, e.g. “No Connection (7 accounts)”:
     * kind of error and number of accounts with this kind of error. */
    shown = g_strdup_printf (ngettext ("%s (%u account)", "%s (%u accounts)", best_count),
                             best->title, best_count);
  else
    shown = g_strdup (best->title);

  adw_banner_set_title (self->banner, shown);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->banner), details->str);
  adw_banner_set_revealed (self->banner, TRUE);
  g_debug ("Banner: %s, %u Fehler", shown, errors->len);
}

static void
add_error (RevolutionMailView *self,
           const char         *uid,
           const char         *title,
           const char         *detail,
           gboolean            per_account)
{
  GPtrArray *errors;
  BannerError *error;

  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (self));

  errors = g_object_get_data (G_OBJECT (self), "banner-errors");
  if (errors == NULL)
    {
      errors = g_ptr_array_new_with_free_func ((GDestroyNotify) banner_error_free);
      g_object_set_data_full (G_OBJECT (self), "banner-errors", errors, (GDestroyNotify) g_ptr_array_unref);
    }
  /* #420: ein neuer Verbindungsfehler eines Kontos ersetzt seinen vorigen. */
  for (guint i = errors->len; uid != NULL && i > 0; i--)
    if (g_strcmp0 (((BannerError *) g_ptr_array_index (errors, i - 1))->uid, uid) == 0)
      g_ptr_array_remove_index (errors, i - 1);

  error = g_new0 (BannerError, 1);
  error->title = title;
  error->detail = g_strdup (detail);
  error->uid = g_strdup (uid);
  error->per_account = per_account;
  g_ptr_array_add (errors, error);
  update_banner (self, NULL);
}

/* title: statische Zeichenkette (Ergebnis von _()); per_account: Fehler zählen als Konten. */
void
revolution_mail_view_show_error (RevolutionMailView *self,
                                 const char         *title,
                                 const char         *detail,
                                 gboolean            per_account)
{
  add_error (self, NULL, title, detail, per_account);
}

/* #420: Verbindungsfehler des Kontos uid (main.c, on_store_noted); zählt als Konto. */
void
revolution_mail_view_show_account_error (RevolutionMailView *self,
                                         const char         *uid,
                                         const char         *title,
                                         const char         *detail)
{
  g_return_if_fail (uid != NULL);

  add_error (self, uid, title, detail, TRUE);
}

/* #420: Aufruf aus main.c, sobald der Store des Kontos uid verbunden ist („notify::connection-status“
 * CAMEL_SERVICE_CONNECTED): seinen Verbindungsfehler aus dem Sammel-Banner nehmen, das Banner ohne
 * übrige Fehler ausblenden. Nur bei CONNECTED, nicht bei anderen Statuswechseln. Muster: Evolutions
 * Status-Symbol des Kontos im Ordnerbaum folgt „notify::connection-status“ des Stores
 * (em-folder-tree-model.c:260-267, 1931-1940): Fehler-Symbol bei DISCONNECTED nach CONNECTING,
 * „network-idle“ bei CONNECTED (dort 1865-1890). */
void
revolution_mail_view_clear_account_errors (RevolutionMailView *self,
                                           const char         *uid)
{
  GPtrArray *errors;
  const char *removed = NULL;

  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (self));

  errors = g_object_get_data (G_OBJECT (self), "banner-errors");
  for (guint i = errors != NULL ? errors->len : 0; i > 0; i--)
    if (g_strcmp0 (((BannerError *) g_ptr_array_index (errors, i - 1))->uid, uid) == 0)
      {
        removed = ((BannerError *) g_ptr_array_index (errors, i - 1))->title;
        g_ptr_array_remove_index (errors, i - 1);
      }
  if (removed == NULL)
    return;

  g_debug ("Banner: %s verbunden, %s weg, %u Fehler übrig", uid, removed, errors->len);
  update_banner (self, removed);
}

/* #408: Aufruf aus main.c im UI-Thread, wenn die Session nach dem Netzmonitor offline bzw.
 * wieder online geht: Pille „Offline“ über der Nachrichtenliste (offline_notice, mail-view.blp).
 * Offline ersetzt die Verbindungsfehler: Besteht das Sammel-Banner nur aus „Keine Verbindung“
 * (Titel aus main.c, on_store_noted), verschwindet es; neue Verbindungsfehler meldet main.c erst
 * wieder online. Gemischte Fehlerarten bleiben unverändert stehen. */
void
revolution_mail_view_set_offline (RevolutionMailView *self,
                                  gboolean            offline)
{
  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (self));

  gtk_widget_set_visible (self->offline_notice, offline);
  g_debug ("Hinweis Offline: %s", offline ? "sichtbar" : "weg");
  if (offline)
    revolution_mail_view_clear_connection_errors (self);
}

/* #421: Sammel-Banner weg, wenn es nur aus „Keine Verbindung“ besteht (set_offline). */
void
revolution_mail_view_clear_connection_errors (RevolutionMailView *self)
{
  GPtrArray *errors;

  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (self));

  errors = g_object_get_data (G_OBJECT (self), "banner-errors");
  if (errors == NULL)
    return;
  for (guint i = 0; i < errors->len; i++)
    if (g_strcmp0 (((BannerError *) g_ptr_array_index (errors, i))->title, _("No Connection")) != 0)
      return;

  g_object_set_data (G_OBJECT (self), "banner-errors", NULL);
  update_banner (self, _("No Connection"));
}

/* #408: Aufruf aus main.c, sobald der Store des Kontos uid nach „Netz zurück“ wieder verbunden
 * ist: die offene Liste, wenn sie zu diesem Konto gehört, vom Server wie F5 (refresh_list). */
void
revolution_mail_view_refresh_account (RevolutionMailView *self,
                                      const char         *uid)
{
  g_autoptr (RevolutionMailAccount) account = NULL;

  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (self));

  if (self->folder == NULL)
    return;

  g_object_get (self->folder, "account", &account, NULL);
  if (account != NULL && g_strcmp0 (revolution_mail_account_get_uid (account), uid) == 0)
    refresh_list (self);
}

/* #350: Ordner der Seitenleiste auswählen (Position im flachen Modell der Auswahl). */
void
select_folder (RevolutionMailView   *self,
               RevolutionMailFolder *target)
{
  GListModel *model = G_LIST_MODEL (self->folder_selection);

  for (guint i = 0; i < g_list_model_get_n_items (model); i++)
    {
      g_autoptr (GtkTreeListRow) row = g_list_model_get_item (model, i);
      g_autoptr (RevolutionMailFolder) folder = gtk_tree_list_row_get_item (row);

      if (folder == target)
        {
          gtk_single_selection_set_selected (self->folder_selection, i);
          return;
        }
    }
}

GListModel *
account_folder_store (RevolutionMailAccount *account)
{
  return gtk_tree_list_model_get_model (account_tree (account));
}

/* #350: Erstauswahl beim Start, sobald die nötigen Ordner geladen sind: der Ordner aus
 * REVOLUTION_FOLDER (#352, nur für Testläufe, Vorrang), sonst der zuletzt gewählte (GSettings
 * „selected-folder“, „<UID des Kontos>/<voller Ordnername>“), sonst der Posteingang des ersten
 * Kontos der Seitenleiste. Fehlt der gewünschte Ordner oder sein Konto, gilt der Posteingang.
 * Ein Konto, dessen Ordner noch nicht da sind, wird abgewartet („folders-noted“,
 * revolution_mail_view_set_folders). Bis dahin ist nichts gewählt (autoselect aus,
 * mail-view.blp), es wird also kein anderer Ordner geladen. */
static void
select_initial_folder (RevolutionMailView *self)
{
  GListModel *accounts = G_LIST_MODEL (self->accounts);
  g_autofree char *saved = NULL;
  /* #366: Start über die Benachrichtigung (revolution_mail_view_show_message) vor allem anderen. */
  const char *wanted = self->show_folder != NULL ? self->show_folder : g_getenv ("REVOLUTION_FOLDER");

  if (self->initial_done)
    return;

  if (wanted == NULL)
    {
      saved = g_settings_get_string (self->settings, "selected-folder");
      wanted = *saved != '\0' ? saved : NULL;
    }

  if (wanted != NULL && strchr (wanted, '/') != NULL)
    {
      g_autofree char *uid = g_strndup (wanted, strchr (wanted, '/') - wanted);
      g_autoptr (RevolutionMailAccount) account = find_account (self, uid);
      g_autoptr (RevolutionMailFolder) folder = NULL;

      if (account != NULL && g_object_get_data (G_OBJECT (account), "folders-noted") == NULL)
        return;

      if (account != NULL)
        folder = revolution_mail_folder_find (account_folder_store (account), strchr (wanted, '/') + 1);
      if (folder != NULL && (revolution_mail_folder_get_flags (folder) & CAMEL_FOLDER_NOSELECT) == 0)
        {
          g_debug ("Erstauswahl: %s", wanted);
          select_folder (self, folder);
          return;
        }
    }

  for (guint i = 0; i < g_list_model_get_n_items (accounts); i++)
    {
      g_autoptr (RevolutionMailAccount) account = g_list_model_get_item (accounts, i);
      GListModel *folders = account_folder_store (account);

      if (g_object_get_data (G_OBJECT (account), "folders-noted") == NULL)
        return;

      for (guint j = 0; j < g_list_model_get_n_items (folders); j++)
        {
          g_autoptr (RevolutionMailFolder) folder = g_list_model_get_item (folders, j);

          if ((revolution_mail_folder_get_flags (folder) & CAMEL_FOLDER_TYPE_MASK) == CAMEL_FOLDER_TYPE_INBOX)
            {
              g_debug ("Erstauswahl: %s/%s (Posteingang des ersten Kontos)",
                       revolution_mail_account_get_uid (account), revolution_mail_folder_get_full_name (folder));
              select_folder (self, folder);
              return;
            }
        }
    }
}

/* #350: Aufruf aus main.c im UI-Thread: Ungelesen-Zähler des Eintrags zu folder live halten
 * (revolution_mail_folder_watch). */
void
revolution_mail_view_watch_folder (RevolutionMailView *self,
                                   CamelFolder        *folder,
                                   gboolean            hold,
                                   gboolean            refreshed)
{
  CamelStore *store;
  g_autoptr (RevolutionMailAccount) account = NULL;
  g_autoptr (RevolutionMailFolder) item = NULL;

  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (self));
  g_return_if_fail (CAMEL_IS_FOLDER (folder));

  /* Nach dispose (Abgleich endet beim Beenden). */
  if (self->accounts == NULL)
    return;

  store = camel_folder_get_parent_store (folder);
  account = store != NULL ? find_account (self, camel_service_get_uid (CAMEL_SERVICE (store))) : NULL;
  if (account != NULL)
    item = revolution_mail_folder_find (account_folder_store (account), camel_folder_get_full_name (folder));
  if (item != NULL)
    revolution_mail_folder_watch (item, folder, hold, refreshed);
}

/* #366: Klick auf die Benachrichtigung (main.c): Mail-Ansicht, Ordner folder_uri und Nachricht uid wählen
 * wie Evolution notify_default_action_cb (em_folder_tree_set_selected, message_list_select_uid;
 * plugins/mail-notification/mail-notification.c:453-473). folder_uri wie e_mail_folder_uri_build
 * (mail-notification.c), gelesen wie e_mail_folder_uri_parse (mail-folder.c, folder_uri_to_name). Sind
 * die Ordner noch nicht geladen, wählt ihn die Erstauswahl (select_initial_folder); die Nachricht folgt
 * mit ihrem Block (select_shown). */
void
revolution_mail_view_show_message (RevolutionMailView *self,
                                   const char         *folder_uri,
                                   const char         *uid)
{
  g_autoptr (CamelURL) url = camel_url_new (folder_uri, NULL);
  g_autoptr (RevolutionMailAccount) account = NULL;
  g_autoptr (RevolutionMailFolder) folder = NULL;
  g_autofree char *store_uid = NULL, *name = NULL;

  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (self));

  if (url == NULL || g_strcmp0 (url->protocol, "folder") != 0 || url->host == NULL || url->path == NULL ||
      *url->path != '/')
    return;

  store_uid = url->user != NULL && *url->user != '\0' ? g_strconcat (url->user, "@", url->host, NULL)
                                                      : g_strdup (url->host);
  name = camel_url_decode_path (url->path + 1);
  g_free (self->show_folder);
  self->show_folder = g_strdup_printf ("%s/%s", store_uid, name);
  g_set_str (&self->show_uid, uid);
  adw_view_stack_set_visible_child_name (self->stack, "mail");

  account = find_account (self, store_uid);
  if (account != NULL)
    folder = revolution_mail_folder_find (account_folder_store (account), name);
  if (folder != NULL && folder == self->folder)
    select_shown (self);
  else if (folder != NULL)
    select_folder (self, folder);
}

/* #348: Ordner eines Kontos aus dem Worker (main.c, camel_store_get_folder_info_sync), im
 * UI-Thread. Ersetzt die bisherigen Ordner; ein inzwischen ausgeblendetes Konto wird
 * übergangen. info NULL: keine Ordner. */
void
revolution_mail_view_set_folders (RevolutionMailView *self,
                                  const char         *uid,
                                  CamelFolderInfo    *info)
{
  g_autoptr (RevolutionMailAccount) account = NULL;
  GListStore *store;
  guint count;

  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (self));

  account = find_account (self, uid);
  if (account == NULL)
    {
      g_debug ("Ordner: %s nicht mehr angezeigt", uid);
      return;
    }

  store = G_LIST_STORE (gtk_tree_list_model_get_model (account_tree (account)));
  g_list_store_remove_all (store);
  count = revolution_mail_folder_append_tree (store, info, account);
  g_debug ("Ordner: %s %u geladen", uid, count);
  g_object_set_data (G_OBJECT (account), "folders-noted", GINT_TO_POINTER (TRUE));
  select_initial_folder (self);
}

/* #362: Aufruf aus main.c nach jedem Ablegen und Sendelauf, im UI-Thread: Postausgang in der
 * Seitenleiste, solange er Nachrichten hat. Evolution zeigt ihn immer (lokaler Store); Revolution
 * nur bei Bedarf (Task #362). */
void
revolution_mail_view_set_outbox (RevolutionMailView *self,
                                 guint               count)
{
  gboolean shown;

  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (self));

  if (self->local_section == NULL)
    return;

  shown = g_list_model_get_n_items (G_LIST_MODEL (self->local_section)) > 0;
  if (count > 0 && !shown)
    g_list_store_append (self->local_section, self->local);
  else if (count == 0 && shown)
    g_list_store_remove_all (self->local_section);
  if ((count > 0) != shown)
    g_debug ("Postausgang: %u Nachrichten, %s", count, count > 0 ? "sichtbar" : "ausgeblendet");
}
