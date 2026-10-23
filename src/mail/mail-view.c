/* mail-view.c – Mail-Ansicht (Prototyp #336)
 *
 * Vorlage: quelltext/stamp/src/views/mail/stamp-mail-view.c (Template-Klasse
 * mit Eigenschaft „stack“ für den Ansichtsumschalter). Oberfläche in
 * mail-view.blp; hier Template-Anbindung, seit #345 die Konten der Seitenleiste,
 * seit #348 ihre Ordner und seit #352 die Nachrichten des gewählten Ordners (Vorlage
 * stamp-conversation-list.c:610-955, Evolution message-list.c:6200-6380 und
 * e-mail-paned-view.c:690-693). Aktionen ohne Funktion sind nicht
 * registriert, ihre Knöpfe und Menüeinträge sind daher ausgegraut (#342).
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>

#include "composer/composer.h"
#include "mail-account.h"
#include "mail-folder.h"
#include "mail-message.h"
#include "mail-view.h"

struct _RevolutionMailView
{
  AdwBreakpointBin     parent_instance;
  AdwViewStack        *stack;
  ESourceRegistry     *registry;
  GListStore          *accounts;
  GtkStack            *sidebar_stack;
  AdwBanner           *banner;
  GtkSingleSelection  *folder_selection;
  GtkSingleSelection  *message_selection;
  GListStore          *messages;
  RevolutionMailFolder *folder;
  CamelSession        *session;
  GCancellable        *load_cancellable;
  guint                load_generation;
  gint64               load_started;
  char                *loading;
  GtkStack            *list_stack;
  GtkLabel            *list_title;
  GtkLabel            *list_subtitle;
  GtkListView         *message_list;
  gint64               frame_start;
  GtkStack            *header_stack;
  GtkSearchEntry      *search_entry;
  GtkToggleButton     *search_button;
  AdwOverlaySplitView *split;
};

G_DEFINE_FINAL_TYPE (RevolutionMailView, revolution_mail_view, ADW_TYPE_BREAKPOINT_BIN)

/* list.compose öffnet den Composer (#337) als eigenes Fenster wie Stamp. */
static void
on_compose (GSimpleAction *action G_GNUC_UNUSED,
            GVariant      *parameter G_GNUC_UNUSED,
            gpointer       user_data)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (user_data));

  gtk_window_present (revolution_composer_new (gtk_window_get_application (GTK_WINDOW (root))));
}

static const GActionEntry list_actions[] = {
  { "compose", on_compose, NULL, NULL, NULL, { 0 } },
};

/* Fokus ins Suchfeld ohne alles zu markieren: sonst ersetzt beim Tippen in der
 * Liste (key-capture-widget) der zweite Buchstabe den ersten. */
static void
focus_search_entry (RevolutionMailView *self)
{
  GtkEditable *text = gtk_editable_get_delegate (GTK_EDITABLE (self->search_entry));

  gtk_text_grab_focus_without_selecting (GTK_TEXT (text));
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
static RevolutionMailAccount *
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

  return NULL;
}

/* #345: Konten aus der Registry in den Store, nur im UI-Thread (Registry-Signale kommen
 * dort an, README „Worker“). Reihenfolge alphabetisch nach Anzeigename wie Evolutions
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
static GtkTreeListModel *
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

/* #352: Nachrichten des gewählten Ordners. Ablauf wie Evolution beim Wählen eines Ordners:
 * Liste aus der Summary (message_list_regen_thread, message-list.c:6200-6380:
 * camel_folder_get_uids, camel_folder_summary_prepare_fetch_all, camel_folder_get_message_info)
 * und Abgleich mit dem Server (e_mail_reader_refresh_folder, e-mail-paned-view.c:690-693).
 * Hier beides in einem Worker-GTask (Architektur 1): erst der Stand im Cache, nach
 * camel_folder_refresh_info_sync der Stand des Servers, dieser nur, wenn er sich vom Cache
 * unterscheidet. Die Einträge entstehen im Worker und kommen in Blöcken in den UI-Thread
 * (g_main_context_invoke_full), dort nur g_list_store_splice (README „Datenmodell“). Ein
 * anderer Ordner bricht das Laden über den GCancellable ab; verspätete Blöcke erkennt die
 * Generation. Sortierung nach Datum absteigend wie der Prototyp (Dokument #5); Evolution
 * sortiert ohne gespeicherten Zustand nicht (message-list.etspec:46-50). */

/* Block klein halten: GtkListView legt beim ersten Füllen für rund 200 Einträge Zeilen an,
 * gemessen etwa 0,4 ms je Zeile (Xvfb, Cairo); 25 Einträge sind damit unter einem Frame
 * (16,7 ms). Der Worker schickt den nächsten Block erst, wenn der UI-Thread den vorigen
 * übernommen hat: GLib arbeitet alle gleichzeitig bereiten Quellen gleicher Priorität in
 * einem Durchlauf ab (gemessen: 9 Blöcke am Stück, 85 ms ohne Antwort), so liegt zwischen
 * zwei Blöcken immer ein Durchlauf für Eingaben und Zeichnen. Priorität
 * G_PRIORITY_DEFAULT_IDLE, also nach Eingaben und Zeichnen (GDK_PRIORITY_REDRAW). */
#define MESSAGES_BLOCK 25

typedef struct
{
  CamelSession *session;
  char         *uid;
  char         *full_name;
  guint         generation;
  /* Übergabe der Blöcke (send_messages, messages_block_free). */
  GMutex        lock;
  GCond         cond;
  guint         consumed;
} LoadMessages;

static void
load_messages_free (LoadMessages *load)
{
  g_object_unref (load->session);
  g_free (load->uid);
  g_free (load->full_name);
  g_mutex_clear (&load->lock);
  g_cond_clear (&load->cond);
  g_free (load);
}

typedef struct
{
  RevolutionMailView *self;
  LoadMessages       *load;
  guint               generation;
  GPtrArray          *items;
  const char         *pass;
  gboolean            reset;
  gboolean            last;
  guint               total;
  guint               unread;
  guint               previews;
} MessagesBlock;

/* Im UI-Thread nach add_messages_block (bzw. beim Verwerfen der Quelle): meldet dem
 * wartenden Worker, dass der Block übernommen ist. */
static void
messages_block_free (MessagesBlock *block)
{
  g_mutex_lock (&block->load->lock);
  block->load->consumed++;
  g_cond_signal (&block->load->cond);
  g_mutex_unlock (&block->load->lock);

  g_object_unref (block->self);
  g_ptr_array_unref (block->items);
  g_free (block);
}

static void
set_list_subtitle (RevolutionMailView *self,
                   guint               total,
                   guint               unread)
{
  g_autofree char *count = NULL;
  g_autofree char *subtitle = NULL;

  /* Untertitel wie der Prototyp (Design/Prototyp/mail-hell.png: „10 E-Mails, 4 ungelesen“). */
  count = g_strdup_printf (ngettext ("%u E-Mail", "%u E-Mails", total), total);
  if (unread > 0)
    /* Translators: „10 E-Mails, 4 ungelesen“; %s ist die Anzahl der E-Mails. */
    subtitle = g_strdup_printf (_("%s, %u ungelesen"), count, unread);

  gtk_label_set_label (self->list_subtitle, subtitle != NULL ? subtitle : count);
  gtk_widget_set_visible (GTK_WIDGET (self->list_subtitle), TRUE);
}

/* Im UI-Thread: ein Block aus send_messages. Der erste Block eines Durchgangs ersetzt die
 * Liste, die übrigen hängen an. */
static gboolean
add_messages_block (gpointer data)
{
  MessagesBlock *block = data;
  RevolutionMailView *self = block->self;
  guint n_items;

  if (block->generation != self->load_generation)
    return G_SOURCE_REMOVE;

  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages));

  if (block->reset)
    {
      g_list_store_splice (self->messages, 0, n_items, block->items->pdata, block->items->len);
      set_list_subtitle (self, block->total, block->unread);
      if (n_items == 0 && block->items->len > 0)
        g_debug ("Nachrichten: %s erste Zeile nach %.0f ms (%s)", self->loading,
                 (g_get_monotonic_time () - self->load_started) / 1000.0, block->pass);
    }
  else
    g_list_store_splice (self->messages, n_items, 0, block->items->pdata, block->items->len);

  gtk_stack_set_visible_child_name (self->list_stack,
                                    g_list_model_get_n_items (G_LIST_MODEL (self->messages)) > 0 ? "list" : "empty");

  if (block->last)
    g_debug ("Nachrichten: %s %s %u (ungelesen %u, mit Vorschau %u) nach %.0f ms", self->loading,
             block->pass, block->total, block->unread, block->previews,
             (g_get_monotonic_time () - self->load_started) / 1000.0);

  return G_SOURCE_REMOVE;
}

static gint64
message_date (CamelMessageInfo *info)
{
  gint64 date = camel_message_info_get_date_sent (info);

  return date > 0 ? date : camel_message_info_get_date_received (info);
}

/* Neueste zuerst, bei gleichem Datum die höhere UID zuerst. */
static int
compare_messages (gconstpointer a,
                  gconstpointer b,
                  gpointer      user_data)
{
  CamelMessageInfo *ia = (CamelMessageInfo *) a, *ib = (CamelMessageInfo *) b;
  gint64 da = message_date (ia), db = message_date (ib);

  if (da != db)
    return da < db ? 1 : -1;

  return camel_folder_cmp_uids (user_data, camel_message_info_get_uid (ib), camel_message_info_get_uid (ia));
}

/* Im Worker: alle Nachrichten des Ordners aus der Summary, sortiert, in Blöcken in den
 * UI-Thread. *signature (UIDs und Flags) erkennt einen unveränderten Stand, der dann nicht
 * erneut geschickt wird. Gelöschte und Spam ausgeblendet wie Evolution
 * (message_list_get_hide_deleted/_junk, message-list.c:4585-4666), ohne die Ausnahmen für
 * Suchordner (gibt es noch nicht). FALSE nach Abbruch. */
static gboolean
send_messages (GTask       *task,
               CamelFolder *folder,
               const char  *pass,
               guint       *signature)
{
  RevolutionMailView *self = g_task_get_source_object (task);
  LoadMessages *load = g_task_get_task_data (task);
  GCancellable *cancellable = g_task_get_cancellable (task);
  guint32 store_flags = camel_store_get_flags (camel_folder_get_parent_store (folder));
  guint32 folder_flags = camel_folder_get_flags (folder);
  gboolean hide_deleted = (store_flags & CAMEL_STORE_VTRASH) == 0 || (folder_flags & CAMEL_FOLDER_IS_TRASH) == 0;
  gboolean hide_junk = (store_flags & (CAMEL_STORE_VJUNK | CAMEL_STORE_REAL_JUNK_FOLDER)) != 0 &&
                       (folder_flags & (CAMEL_FOLDER_IS_JUNK | CAMEL_FOLDER_IS_TRASH)) == 0;
  g_autoptr (GPtrArray) infos = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  guint hash = 0, unread = 0, previews = 0, i = 0, sent = load->consumed;
  GPtrArray *uids;

  camel_folder_summary_prepare_fetch_all (camel_folder_get_folder_summary (folder), NULL);
  uids = camel_folder_get_uids (folder);
  for (guint j = 0; j < uids->len; j++)
    {
      CamelMessageInfo *info = camel_folder_get_message_info (folder, uids->pdata[j]);
      guint32 flags;

      if (info == NULL)
        continue;

      flags = camel_message_info_get_flags (info);
      if ((hide_deleted && (flags & CAMEL_MESSAGE_DELETED) != 0) ||
          (hide_junk && (flags & CAMEL_MESSAGE_JUNK) != 0))
        {
          g_object_unref (info);
          continue;
        }

      hash = hash * 31 + g_str_hash (uids->pdata[j]) + flags;
      if ((flags & CAMEL_MESSAGE_SEEN) == 0)
        unread++;
      g_ptr_array_add (infos, info);
    }
  camel_folder_free_uids (folder, uids);
  hash = hash * 31 + infos->len;

  /* Leerer Cache: nichts schicken, der Server-Durchgang folgt. */
  if (hash == *signature || (infos->len == 0 && g_str_equal (pass, "Cache")))
    {
      *signature = hash;
      return !g_cancellable_is_cancelled (cancellable);
    }
  *signature = hash;

  g_ptr_array_sort_values_with_data (infos, compare_messages, folder);

  do
    {
      MessagesBlock *block;
      guint end = MIN (i + MESSAGES_BLOCK, infos->len);

      if (g_cancellable_is_cancelled (cancellable))
        return FALSE;

      block = g_new0 (MessagesBlock, 1);
      block->self = g_object_ref (self);
      block->load = load;
      block->generation = load->generation;
      block->items = g_ptr_array_new_full (end - i, g_object_unref);
      block->pass = pass;
      block->reset = i == 0;
      block->total = infos->len;
      block->unread = unread;

      for (; i < end; i++)
        {
          RevolutionMailMessage *item = revolution_mail_message_new (infos->pdata[i], now);

          previews += revolution_mail_message_has_preview (item);
          g_ptr_array_add (block->items, item);
        }

      block->last = i >= infos->len;
      block->previews = previews;
      g_main_context_invoke_full (g_task_get_context (task), G_PRIORITY_DEFAULT_IDLE, add_messages_block, block,
                                  (GDestroyNotify) messages_block_free);

      /* Warten, bis der UI-Thread den Block übernommen hat (siehe MESSAGES_BLOCK). */
      sent++;
      g_mutex_lock (&load->lock);
      while (load->consumed < sent)
        g_cond_wait (&load->cond, &load->lock);
      g_mutex_unlock (&load->lock);
    }
  while (i < infos->len);

  return TRUE;
}

static void
load_messages_thread (GTask        *task,
                      gpointer      source_object G_GNUC_UNUSED,
                      gpointer      task_data,
                      GCancellable *cancellable)
{
  LoadMessages *load = task_data;
  g_autoptr (CamelService) service = camel_session_ref_service (load->session, load->uid);
  g_autoptr (CamelFolder) folder = NULL;
  GError *error = NULL;
  guint signature = 0;

  if (!CAMEL_IS_STORE (service))
    {
      g_task_return_new_error (task, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE,
                               _("Konto %s nicht verfügbar"), load->uid);
      return;
    }

  /* Ordner immer über camel_store_get_folder_sync (README „Camel-Ordner“). */
  folder = camel_store_get_folder_sync (CAMEL_STORE (service), load->full_name, 0, cancellable, &error);
  if (folder == NULL)
    {
      g_task_return_error (task, error);
      return;
    }

  if (!send_messages (task, folder, "Cache", &signature) ||
      !camel_folder_refresh_info_sync (folder, cancellable, &error) ||
      !send_messages (task, folder, "Server", &signature))
    {
      if (error == NULL)
        g_task_return_error_if_cancelled (task);
      else
        g_task_return_error (task, error);
      return;
    }

  g_task_return_boolean (task, TRUE);
}

static void
on_messages_loaded (GObject      *source_object,
                    GAsyncResult *result,
                    gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionMailView *self = REVOLUTION_MAIL_VIEW (source_object);
  LoadMessages *load = g_task_get_task_data (G_TASK (result));
  g_autoptr (GError) error = NULL;
  g_autofree char *detail = NULL;
  guint n_items;

  if (!g_task_propagate_boolean (G_TASK (result), &error) &&
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      g_debug ("Nachrichten: %s %s abgebrochen", load->uid, load->full_name);
      return;
    }

  if (load->generation != self->load_generation)
    return;

  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages));
  if (n_items == 0)
    {
      gtk_stack_set_visible_child_name (self->list_stack, "empty");
      set_list_subtitle (self, 0, 0);
    }

  if (error != NULL)
    {
      g_warning ("Nachrichten %s %s: %s", load->uid, load->full_name, error->message);
      detail = g_strdup_printf ("%s: %s", gtk_label_get_label (self->list_title), error->message);
      revolution_mail_view_show_error (self, _("Nachrichten nicht geladen"), detail, FALSE);
    }

  g_debug ("Nachrichten: %s vollständig %u nach %.0f ms", self->loading, n_items,
           (g_get_monotonic_time () - self->load_started) / 1000.0);
}

/* Auswahl im Ordnerbaum: Nachrichten dieses Ordners laden, laufendes Laden abbrechen. */
static void
on_folder_selected (RevolutionMailView *self)
{
  GtkTreeListRow *row = gtk_single_selection_get_selected_item (self->folder_selection);
  g_autoptr (RevolutionMailFolder) folder = row != NULL ? gtk_tree_list_row_get_item (row) : NULL;
  g_autoptr (RevolutionMailAccount) account = NULL;
  g_autofree char *display_name = NULL;
  g_autoptr (GTask) task = NULL;
  LoadMessages *load;

  /* GtkSingleSelection meldet dieselbe Auswahl erneut, wenn der Ordnerbaum wächst. */
  if (folder == self->folder)
    return;
  g_set_object (&self->folder, folder);

  g_cancellable_cancel (self->load_cancellable);
  g_clear_object (&self->load_cancellable);
  self->load_generation++;
  g_list_store_remove_all (self->messages);
  gtk_widget_set_visible (GTK_WIDGET (self->list_subtitle), FALSE);

  if (folder != NULL)
    g_object_get (folder, "account", &account, "display-name", &display_name, NULL);

  gtk_label_set_label (self->list_title, display_name != NULL ? display_name : _("Mail"));

  /* Ohne Konto bzw. Ordner ohne Inhalt (NOSELECT, z. B. Gmail „[Gmail]“): leere Liste wie
   * Evolution (e-mail-shell-view-private.c:116-121). */
  if (account == NULL || self->session == NULL ||
      (revolution_mail_folder_get_flags (folder) & CAMEL_FOLDER_NOSELECT) != 0)
    {
      gtk_stack_set_visible_child_name (self->list_stack, "empty");
      return;
    }

  gtk_stack_set_visible_child_name (self->list_stack, "list");

  load = g_new0 (LoadMessages, 1);
  g_mutex_init (&load->lock);
  g_cond_init (&load->cond);
  load->session = g_object_ref (self->session);
  load->uid = g_strdup (revolution_mail_account_get_uid (account));
  load->full_name = g_strdup (revolution_mail_folder_get_full_name (folder));
  load->generation = self->load_generation;

  g_free (self->loading);
  self->loading = g_strdup_printf ("%s %s", load->uid, load->full_name);
  self->load_started = g_get_monotonic_time ();
  g_debug ("Nachrichten: %s lädt", self->loading);

  self->load_cancellable = g_cancellable_new ();
  task = g_task_new (self, self->load_cancellable, on_messages_loaded, NULL);
  g_task_set_source_tag (task, on_folder_selected);
  g_task_set_priority (task, G_PRIORITY_DEFAULT_IDLE);
  g_task_set_task_data (task, load, (GDestroyNotify) load_messages_free);
  g_task_run_in_thread (task, load_messages_thread);
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

static void
revolution_mail_view_dispose (GObject *object)
{
  RevolutionMailView *self = REVOLUTION_MAIL_VIEW (object);

  /* Blöcke, die noch kommen, verwerfen (add_messages_block). */
  self->load_generation++;
  g_cancellable_cancel (self->load_cancellable);
  g_clear_object (&self->load_cancellable);
  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_VIEW);
  g_clear_object (&self->stack);
  g_clear_object (&self->registry);
  g_clear_object (&self->accounts);
  g_clear_object (&self->messages);
  g_clear_object (&self->folder);
  g_clear_object (&self->session);
  g_clear_pointer (&self->loading, g_free);

  G_OBJECT_CLASS (revolution_mail_view_parent_class)->dispose (object);
}

static void
revolution_mail_view_class_init (RevolutionMailViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = revolution_mail_view_get_property;
  object_class->set_property = revolution_mail_view_set_property;
  object_class->dispose = revolution_mail_view_dispose;

  g_object_class_install_property (object_class, PROP_STACK,
                                   g_param_spec_object ("stack", NULL, NULL, ADW_TYPE_VIEW_STACK,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_type_ensure (REVOLUTION_TYPE_MAIL_ACCOUNT);
  g_type_ensure (REVOLUTION_TYPE_MAIL_FOLDER);
  g_type_ensure (REVOLUTION_TYPE_MAIL_MESSAGE);

  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/mail-view.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, folder_selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, sidebar_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, banner);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, message_selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, list_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, list_title);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, list_subtitle);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, message_list);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, header_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, search_entry);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, search_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, split);
  gtk_widget_class_bind_template_callback (widget_class, on_search_toggled);
  gtk_widget_class_bind_template_callback (widget_class, on_search_started);
  gtk_widget_class_bind_template_callback (widget_class, on_search_stop);
}

static void
revolution_mail_view_init (RevolutionMailView *self)
{
  g_autoptr (GtkFlattenListModel) folders = NULL;
  g_autoptr (GSimpleActionGroup) list_group = g_simple_action_group_new ();
  g_autoptr (GSimpleActionGroup) mail_group = g_simple_action_group_new ();
  g_autoptr (GPropertyAction) sidebar_action = NULL;
  AdwStyleManager *style_manager = adw_style_manager_get_default ();

  gtk_widget_init_template (GTK_WIDGET (self));

  /* CR #397: Ordnerbaum als GtkTreeListModel je Konto (#348), Konten als Abschnitte (#345). */
  self->accounts = g_list_store_new (REVOLUTION_TYPE_MAIL_ACCOUNT);
  folders = gtk_flatten_list_model_new (G_LIST_MODEL (gtk_map_list_model_new (g_object_ref (G_LIST_MODEL (self->accounts)),
                                                                              account_folders, NULL, NULL)));
  gtk_single_selection_set_model (self->folder_selection, G_LIST_MODEL (folders));
  g_signal_connect_object (self->folder_selection, "notify::selected-item", G_CALLBACK (on_folder_selected),
                           self, G_CONNECT_SWAPPED);
  /* #352: Nachrichten des gewählten Ordners (on_folder_selected). */
  self->messages = g_list_store_new (REVOLUTION_TYPE_MAIL_MESSAGE);
  gtk_single_selection_set_model (self->message_selection, G_LIST_MODEL (self->messages));
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
 * (z. B. „Datenordner fehlt“) bleiben im Tooltip erhalten, statt überschrieben zu werden. */
typedef struct
{
  const char *title;
  guint count;
  gboolean per_account;
} BannerKind;

typedef struct
{
  GString *details;
  GArray *kinds;
} BannerErrors;

static void
banner_errors_free (BannerErrors *errors)
{
  g_string_free (errors->details, TRUE);
  g_array_unref (errors->kinds);
  g_free (errors);
}

/* title: statische Zeichenkette (Ergebnis von _()); per_account: Fehler zählen als Konten. */
void
revolution_mail_view_show_error (RevolutionMailView *self,
                                 const char         *title,
                                 const char         *detail,
                                 gboolean            per_account)
{
  g_autofree char *shown = NULL;
  BannerErrors *errors;
  BannerKind *best = NULL;
  guint i;

  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (self));

  errors = g_object_get_data (G_OBJECT (self), "banner-errors");
  if (errors == NULL)
    {
      errors = g_new0 (BannerErrors, 1);
      errors->details = g_string_new (NULL);
      errors->kinds = g_array_new (FALSE, TRUE, sizeof (BannerKind));
      g_object_set_data_full (G_OBJECT (self), "banner-errors", errors,
                              (GDestroyNotify) banner_errors_free);
    }
  else
    g_string_append_c (errors->details, '\n');

  g_string_append (errors->details, detail);

  for (i = 0; i < errors->kinds->len; i++)
    if (g_strcmp0 (g_array_index (errors->kinds, BannerKind, i).title, title) == 0)
      break;
  if (i == errors->kinds->len)
    {
      BannerKind kind = { title, 0, per_account };

      g_array_append_val (errors->kinds, kind);
    }
  g_array_index (errors->kinds, BannerKind, i).count++;

  for (i = 0; i < errors->kinds->len; i++)
    if (best == NULL || g_array_index (errors->kinds, BannerKind, i).count > best->count)
      best = &g_array_index (errors->kinds, BannerKind, i);

  if (best->per_account && best->count > 1)
    /* Translators: Titel des Fehlerbanners, z. B. „Keine Verbindung (7 Konten)“:
     * Fehlerart und Zahl der Konten mit dieser Fehlerart. */
    shown = g_strdup_printf (ngettext ("%s (%u Konto)", "%s (%u Konten)", best->count),
                             best->title, best->count);
  else
    shown = g_strdup (best->title);

  adw_banner_set_title (self->banner, shown);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->banner), errors->details->str);
  adw_banner_set_revealed (self->banner, TRUE);
}

/* #352: Nur für Testläufe (tests/nachrichtenliste.sh) wie REVOLUTION_THEME (main.c): die
 * Umgebungsvariable REVOLUTION_FOLDER=<UID des Kontos>/<voller Ordnername> wählt diesen Ordner,
 * sobald er in der Seitenleiste steht. Die Erstauswahl beim Start ist #350. */
static void
select_folder_from_env (RevolutionMailView *self)
{
  const char *wanted = g_getenv ("REVOLUTION_FOLDER");
  GListModel *model = G_LIST_MODEL (self->folder_selection);

  if (wanted == NULL)
    return;

  for (guint i = 0; i < g_list_model_get_n_items (model); i++)
    {
      g_autoptr (GtkTreeListRow) row = g_list_model_get_item (model, i);
      g_autoptr (RevolutionMailFolder) folder = gtk_tree_list_row_get_item (row);
      g_autoptr (RevolutionMailAccount) account = NULL;
      g_autofree char *key = NULL;

      g_object_get (folder, "account", &account, NULL);
      if (account == NULL)
        continue;

      key = g_strdup_printf ("%s/%s", revolution_mail_account_get_uid (account),
                             revolution_mail_folder_get_full_name (folder));
      if (g_str_equal (key, wanted))
        {
          if (gtk_single_selection_get_selected (self->folder_selection) != i)
            gtk_single_selection_set_selected (self->folder_selection, i);
          return;
        }
    }
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
  select_folder_from_env (self);
}
