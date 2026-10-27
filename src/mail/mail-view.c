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
#include "eds-compat.h"
#include "mail-account.h"
#include "mail-conversation.h"
#include "mail-favorites.h"
#include "mail-folder.h"
#include "mail-message.h"
#include "mail-reader.h"
#include "mail-thread.h"
#include "mail-view.h"

struct _RevolutionMailView
{
  AdwBreakpointBin     parent_instance;
  AdwViewStack        *stack;
  ESourceRegistry     *registry;
  GListStore          *accounts;
  GtkStack            *sidebar_stack;
  AdwBanner           *banner;
  GtkWidget           *offline_notice;
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
  /* #353: Ordner der Liste nach dem Laden (Signal „changed“), Änderungen in der Warteschlange. */
  CamelFolder         *camel_folder;
  GHashTable          *pending_changed;
  GHashTable          *pending_removed;
  gboolean             changes_running;
  /* #410: ein Durchgang schickt noch Blöcke (add_messages_block), Änderungen warten bis danach. */
  gboolean             blocks_running;
  GActionMap          *message_actions;
  RevolutionMailMessage *selected;
  gulong               selected_handler;
  guint                seen_id;
  GSettings           *settings;
  GMenuModel          *row_menu;
  GtkWidget           *row_popover;
  GQueue               flag_jobs;
  gboolean             flags_running;
  /* Anwendung gehalten, solange Flag-Aufträge laufen; Obergrenze nach dem Schließen. */
  GApplication        *flags_hold;
  guint                flags_deadline_id;
  /* Daten des laufenden Auftrags (gehören der GTask, gültig solange flags_running). */
  const struct _SetFlags *running;
  gboolean             flags_saving;
  GCancellable        *cancellable;
  GtkStack            *message_stack;
  RevolutionMailReader *reader;
  /* #350: Erstauswahl erledigt (select_initial_folder). */
  gboolean             initial_done;
  /* #351: Abschnitt „Favoriten“ über den Konten (mail-favorites.c). */
  RevolutionMailAccount *favorites;
  /* #357: Konversationen (GSettings „conversations“, beim Wählen des Ordners übernommen), Lesebereich
   * der Konversation, Suchordner aus Ordner und Gesendet zum Laden ihrer Nachrichten (mail-thread.c)
   * und Gelesen-Timer je angezeigter Nachricht (GSource). */
  gboolean             conversations;
  RevolutionMailConversation *conversation_view;
  RevolutionMailFolder *thread_folder;
  GPtrArray           *member_seen;
};

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
static void on_context_menu (GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void start_folder_changes (RevolutionMailView *self);
static void clear_member_seen (RevolutionMailView *self);
static void on_message_selected (RevolutionMailView *self);

static const GActionEntry list_actions[] = {
  { "compose", on_compose, NULL, NULL, NULL, { 0 } },
  { "refresh", on_refresh, NULL, NULL, NULL, { 0 } },
  { "context-menu", on_context_menu, NULL, NULL, NULL, { 0 } },
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

  /* #351: Favoriten für Erstauswahl und Zähler (select_initial_folder, revolution_mail_view_watch_folder). */
  if (self->favorites != NULL && g_strcmp0 (revolution_mail_account_get_uid (self->favorites), uid) == 0)
    return g_object_ref (self->favorites);

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
  /* #414: Die Liste zeigte beim Beginn des Durchgangs Einträge (aus dem Cache oder aus
   * Live-Änderungen); gesetzt im UI-Thread (add_messages_block), während der Worker wartet. */
  gboolean      shown;
  /* Übergabe der Blöcke (send_messages, messages_block_free). */
  GMutex        lock;
  GCond         cond;
  guint         posted;
  guint         consumed;
  /* #350: Abgleich mit dem Server, nur beim Beenden abgebrochen (load_messages_thread). */
  GCancellable *refresh_cancellable;
  /* #357: Konversationen statt einzelner Nachrichten; Gesendet des Kontos zum Mischen oder NULL. */
  gboolean      conversations;
  char         *sent_full_name;
} LoadMessages;

/* Mit Referenzzähler (g_atomic_rc_box): Blöcke, die nach einem Abbruch noch in der
 * Hauptschleife liegen, halten LoadMessages über das Ende des Tasks hinaus. */
static void
load_messages_clear (LoadMessages *load)
{
  g_object_unref (load->session);
  g_clear_object (&load->refresh_cancellable);
  g_free (load->uid);
  g_free (load->full_name);
  g_free (load->sent_full_name);
  g_mutex_clear (&load->lock);
  g_cond_clear (&load->cond);
}

static void
load_messages_release (LoadMessages *load)
{
  g_atomic_rc_box_release_full (load, (GDestroyNotify) load_messages_clear);
}

typedef struct
{
  RevolutionMailView *self;
  LoadMessages       *load;
  guint               generation;
  GPtrArray          *items;
  const char         *pass;
  gboolean            reset;
  gboolean            merge;
  gboolean            last;
  /* #414: keine Einträge, nur Beginn eines Durchgangs (vor dem Lesen der UIDs) bzw. Ende eines
   * Durchgangs, der nichts schickt (send_messages). */
  gboolean            begin;
  gboolean            end;
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
  load_messages_release (block->load);

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
  /* %'u: Tausenderpunkt nach Gebietsschema (glibc), z. B. „3.000 E-Mails“. #357: in Konversationen
   * deren Zahl, ungelesen dann die Konversationen mit ungelesenen Nachrichten. */
  if (self->conversations)
    count = g_strdup_printf (ngettext ("%'u Konversation", "%'u Konversationen", total), total);
  else
    count = g_strdup_printf (ngettext ("%'u E-Mail", "%'u E-Mails", total), total);
  if (unread > 0)
    /* Translators: „10 E-Mails, 4 ungelesen“; %s ist die Anzahl der E-Mails. */
    subtitle = g_strdup_printf (_("%s, %'u ungelesen"), count, unread);

  gtk_label_set_label (self->list_subtitle, subtitle != NULL ? subtitle : count);
  gtk_widget_set_visible (GTK_WIDGET (self->list_subtitle), TRUE);
}

/* #418: Reihenfolge der Liste fürs Log (tests/sortierung.sh): Zeilen, die neuer sind als die Zeile
 * darüber, die UIDs der ersten drei Zeilen und die gewählte UID. */
static void
log_order (RevolutionMailView *self)
{
  GListModel *model = G_LIST_MODEL (self->messages);
  RevolutionMailMessage *selected = gtk_single_selection_get_selected_item (self->message_selection);
  g_autoptr (GString) top = g_string_new (NULL);
  gint64 previous = G_MAXINT64;
  guint wrong = 0;

  for (guint i = 0; i < g_list_model_get_n_items (model); i++)
    {
      g_autoptr (RevolutionMailMessage) item = g_list_model_get_item (model, i);
      gint64 timestamp = revolution_mail_message_get_timestamp (item);

      wrong += timestamp > previous;
      previous = timestamp;
      if (i < 3)
        g_string_append_printf (top, " %s", revolution_mail_message_get_uid (item));
    }

  g_debug ("Nachrichten: %s Reihenfolge: %u nicht nach Datum, oben%s, gewählt %s", self->loading, wrong, top->str,
           selected != NULL ? revolution_mail_message_get_uid (selected) : "-");
}

/* #419: Nach einem Abgleich der Liste (Server-Stand oder Live-Änderung): Zeigt der Lesebereich die gewählte
 * Konversation noch als einzelne Nachricht (beim Wählen hatte sie eine, on_message_selected) und hat sie
 * jetzt mehrere, zeigt er sie als Konversation (mail-conversation.c: neueste und ungelesene offen), wie
 * eine schon als Konversation gezeigte neue Nachrichten aufnimmt (on_members_changed). Der Gelesen-Timer
 * der Zeile gälte sonst allen ihren Nachrichten (mark_read an der Konversation), auch der neuen, noch nicht
 * angezeigten; er wird verworfen, die Konversation startet je angezeigter Nachricht einen eigenen
 * (on_conversation_shown, #353). */
static void
follow_conversation (RevolutionMailView *self)
{
  RevolutionMailMessage *message = gtk_single_selection_get_selected_item (self->message_selection);
  GListModel *members = message != NULL ? revolution_mail_message_get_members (message) : NULL;

  if (members == NULL || g_list_model_get_n_items (members) < 2 ||
      g_strcmp0 (gtk_stack_get_visible_child_name (self->message_stack), "content") != 0)
    return;

  g_debug ("Konversation: %s jetzt mit %u Nachrichten", revolution_mail_message_get_uid (message),
           g_list_model_get_n_items (members));
  g_clear_handle_id (&self->seen_id, g_source_remove);
  on_message_selected (self);
}

/* Im UI-Thread: Server-Stand items (ganz, sortiert) als Abgleich auf die angezeigte Liste:
 * nur fehlende Einträge entfernen, neue einfügen, geänderte am bestehenden Eintrag
 * aktualisieren. Auswahl und Scrollposition bleiben erhalten. #414: Abgleich über die UID, nicht
 * über die Reihenfolge allein: Einträge, deren Reihenfolge schon stimmt (längste aufsteigende
 * Teilfolge ihrer Plätze in items, immer mit dem gewählten Eintrag), bleiben stehen; die übrigen
 * werden als dieselben Objekte verschoben, nie neu erzeugt. Sonst fügte eine abweichende
 * Reihenfolge (z. B. gleiches Datum) ab dort alles neu ein und entfernte das Alte. */
static void
merge_messages (RevolutionMailView *self,
                GPtrArray          *items,
                guint              *added,
                guint              *removed,
                guint              *changed,
                guint              *moved)
{
  GListModel *model = G_LIST_MODEL (self->messages);
  gpointer selected = gtk_single_selection_get_selected_item (self->message_selection);
  /* UID → Platz in items + 1 bzw. UID → verschobener Eintrag (Referenz). */
  g_autoptr (GHashTable) wanted = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GHashTable) moving = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);
  g_autofree guint *place = NULL, *tails = NULL, *prev = NULL;
  g_autofree gboolean *keep = NULL;
  guint i, j, n, n_tails = 0, sel = G_MAXUINT;

  for (j = 0; j < items->len; j++)
    g_hash_table_insert (wanted, (gpointer) revolution_mail_message_get_uid (items->pdata[j]), GUINT_TO_POINTER (j + 1));

  /* Nicht mehr vorhandene, zusammenhängende Läufe auf einmal. */
  for (i = g_list_model_get_n_items (model); i > 0;)
    {
      guint end = i;

      while (i > 0)
        {
          g_autoptr (RevolutionMailMessage) old = g_list_model_get_item (model, i - 1);

          if (g_hash_table_contains (wanted, revolution_mail_message_get_uid (old)))
            break;
          i--;
        }

      if (i < end)
        {
          g_list_store_splice (self->messages, i, end - i, NULL, 0);
          *removed += end - i;
        }
      else
        i--;
    }

  /* Stehen bleiben: längste aufsteigende Teilfolge der Plätze (Patience, O(n log n)), nur über
   * Einträge, die mit dem gewählten verträglich sind; jede längste enthält dann ihn. */
  n = g_list_model_get_n_items (model);
  place = g_new (guint, MAX (n, 1));
  tails = g_new (guint, MAX (n, 1));
  prev = g_new (guint, MAX (n, 1));
  keep = g_new0 (gboolean, MAX (n, 1));
  for (i = 0; i < n; i++)
    {
      g_autoptr (RevolutionMailMessage) old = g_list_model_get_item (model, i);

      place[i] = GPOINTER_TO_UINT (g_hash_table_lookup (wanted, revolution_mail_message_get_uid (old))) - 1;
      if (old == selected)
        sel = i;
    }
  for (i = 0; i < n; i++)
    {
      guint lo = 0, hi = n_tails;

      if (sel != G_MAXUINT && ((i < sel && place[i] > place[sel]) || (i > sel && place[i] < place[sel])))
        continue;
      while (lo < hi)
        {
          guint mid = (lo + hi) / 2;

          if (place[tails[mid]] < place[i])
            lo = mid + 1;
          else
            hi = mid;
        }
      prev[i] = lo > 0 ? tails[lo - 1] : G_MAXUINT;
      tails[lo] = i;
      n_tails = MAX (n_tails, lo + 1);
    }
  for (i = n_tails > 0 ? tails[n_tails - 1] : G_MAXUINT; i != G_MAXUINT; i = prev[i])
    keep[i] = TRUE;

  /* Die übrigen herausnehmen (Läufe von hinten) und gleich als dieselben Objekte einfügen. */
  for (i = n; i > 0;)
    {
      guint end = i;

      while (i > 0 && !keep[i - 1])
        {
          RevolutionMailMessage *old = g_list_model_get_item (model, i - 1);

          g_hash_table_replace (moving, (gpointer) revolution_mail_message_get_uid (old), old);
          i--;
        }

      if (i < end)
        g_list_store_splice (self->messages, i, end - i, NULL, 0);
      else
        i--;
    }

  for (i = 0, j = 0; j < items->len;)
    {
      g_autoptr (RevolutionMailMessage) old = i < g_list_model_get_n_items (model) ? g_list_model_get_item (model, i) : NULL;
      const char *old_uid = old != NULL ? revolution_mail_message_get_uid (old) : NULL;
      g_autoptr (GPtrArray) run = NULL;

      if (g_strcmp0 (old_uid, revolution_mail_message_get_uid (items->pdata[j])) == 0)
        {
          /* Am bestehenden Eintrag, nicht per splice: die Auswahl bliebe sonst nicht erhalten. */
          if (revolution_mail_message_update (old, items->pdata[j]))
            (*changed)++;
          i++;
          j++;
          continue;
        }

      run = g_ptr_array_new ();
      for (; j < items->len && g_strcmp0 (old_uid, revolution_mail_message_get_uid (items->pdata[j])) != 0; j++)
        {
          RevolutionMailMessage *same = g_hash_table_lookup (moving, revolution_mail_message_get_uid (items->pdata[j]));

          if (same != NULL)
            {
              *changed += revolution_mail_message_update (same, items->pdata[j]);
              (*moved)++;
              g_ptr_array_add (run, same);
            }
          else
            {
              (*added)++;
              g_ptr_array_add (run, items->pdata[j]);
            }
        }
      g_list_store_splice (self->messages, i, 0, run->pdata, run->len);
      i += run->len;
    }
}

/* #354: Nur für Testläufe (Screenshots des Lesebereichs) wie REVOLUTION_FOLDER: die
 * Umgebungsvariable REVOLUTION_MESSAGE=<Teil des Betreffs> wählt die erste passende Nachricht,
 * sobald sie in der Liste steht und noch nichts gewählt ist. */
static void
select_message_from_env (RevolutionMailView *self)
{
  const char *wanted = g_getenv ("REVOLUTION_MESSAGE");
  GListModel *model = G_LIST_MODEL (self->messages);

  if (wanted == NULL || gtk_single_selection_get_selected_item (self->message_selection) != NULL)
    return;

  for (guint i = 0; i < g_list_model_get_n_items (model); i++)
    {
      g_autoptr (RevolutionMailMessage) message = g_list_model_get_item (model, i);
      g_autofree char *subject = NULL;

      g_object_get (message, "subject", &subject, NULL);
      if (subject != NULL && strstr (subject, wanted) != NULL)
        {
          gtk_single_selection_set_selected (self->message_selection, i);
          return;
        }
    }
}

/* Im UI-Thread: ein Block aus send_messages. Der erste Block eines Durchgangs ersetzt die
 * Liste, die übrigen hängen an; ein Server-Stand nach angezeigtem Cache kommt als ein Block
 * und wird abgeglichen (merge_messages). */
static gboolean
add_messages_block (gpointer data)
{
  MessagesBlock *block = data;
  RevolutionMailView *self = block->self;
  guint n_items;

  if (block->generation != self->load_generation)
    return G_SOURCE_REMOVE;

  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages));

  /* #410: Zwischen dem ersten und dem letzten Block eines Durchgangs keine Live-Änderungen
   * anwenden (start_folder_changes, on_folder_changes_done): eine dabei eingefügte UID hinge ein
   * späterer Block erneut an. Nach dem letzten Block wirken sie am bestehenden Eintrag. Wie
   * Evolution, das während eines Neuaufbaus keine Änderung einzeln einträgt, sondern die Liste
   * neu aufbaut (message_list_folder_changed, message-list.c:5127-5128, 5178-5186).
   * #414: Schon ab dem Lesen der UIDs (begin; beim Cache-Durchgang ab der Auswahl), sonst
   * entfernte bzw. überschriebe der Durchgang eine dazwischen eingefügte neue Mail. Zeigt die
   * Liste dann schon Einträge (auch aus Live-Änderungen ohne Cache), gleicht der
   * Server-Durchgang ab, statt sie zu ersetzen. */
  if (block->begin)
    {
      self->blocks_running = TRUE;
      block->load->shown = n_items > 0;
      return G_SOURCE_REMOVE;
    }
  self->blocks_running = !block->last;
  if (block->end)
    {
      start_folder_changes (self);
      return G_SOURCE_REMOVE;
    }

  if (block->merge)
    {
      GtkAdjustment *vadjustment = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (self->message_list));
      gboolean at_top = gtk_adjustment_get_value (vadjustment) < 1.0;
      guint added = 0, removed = 0, changed = 0, moved = 0;

      merge_messages (self, block->items, &added, &removed, &changed, &moved);
      /* GtkListView hält die bisher erste Zeile; stand die Liste oben, soll neue Post oben
       * sichtbar sein. Ohne Auswahl und Fokus (GTK_LIST_SCROLL_NONE). */
      if (at_top && g_list_model_get_n_items (G_LIST_MODEL (self->messages)) > 0)
        gtk_list_view_scroll_to (self->message_list, 0, GTK_LIST_SCROLL_NONE, NULL);
      set_list_subtitle (self, block->total, block->unread);
      g_debug ("Nachrichten: %s Abgleich %u neu, %u entfernt, %u geändert, %u verschoben", self->loading,
               added, removed, changed, moved);
      log_order (self);
      follow_conversation (self);
    }
  else if (block->reset)
    {
      g_list_store_splice (self->messages, 0, n_items, block->items->pdata, block->items->len);
      set_list_subtitle (self, block->total, block->unread);
      if (n_items == 0 && block->items->len > 0)
        g_debug ("Nachrichten: %s erste Zeile nach %.0f ms (%s)", self->loading,
                 (g_get_monotonic_time () - self->load_started) / 1000.0, block->pass);
    }
  else
    g_list_store_splice (self->messages, n_items, 0, block->items->pdata, block->items->len);

  /* #414: Stand der Liste je Block fürs Log (Prüfung auf Rücksprünge, tests/erstes-oeffnen.sh). */
  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages));
  g_debug ("Nachrichten: %s Liste %u nach %s-Block %u", self->loading, n_items, block->pass, block->items->len);
  gtk_stack_set_visible_child_name (self->list_stack, n_items > 0 ? "list" : "empty");

  select_message_from_env (self);

  if (block->last)
    {
      g_debug ("Nachrichten: %s %s %u (ungelesen %u, mit Vorschau %u) nach %.0f ms", self->loading,
               block->pass, block->total, block->unread, block->previews,
               (g_get_monotonic_time () - self->load_started) / 1000.0);
      start_folder_changes (self);
    }

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

/* Im Worker: Block an den UI-Thread (Standardkontext) und warten, bis er und alle vorigen
 * übernommen sind (siehe MESSAGES_BLOCK); #414: ohne wait nicht warten (Ende eines Durchgangs).
 * Nach Abbruch nicht weiter warten: beim Beenden läuft die Hauptschleife nicht mehr. FALSE nach
 * Abbruch. */
static gboolean
post_block (MessagesBlock *block,
            GCancellable  *cancellable,
            gboolean       wait)
{
  LoadMessages *load = block->load;
  g_autoptr (GSource) source = g_idle_source_new ();
  gboolean done;
  guint sent;

  g_mutex_lock (&load->lock);
  sent = ++load->posted;
  g_mutex_unlock (&load->lock);

  g_source_set_priority (source, G_PRIORITY_DEFAULT_IDLE);
  g_source_set_static_name (source, "[revolution] add_messages_block");
  g_source_set_callback (source, add_messages_block, block, (GDestroyNotify) messages_block_free);
  g_source_attach (source, NULL);

  if (!wait)
    return !g_cancellable_is_cancelled (cancellable);

  g_mutex_lock (&load->lock);
  while (load->consumed < sent && !g_cancellable_is_cancelled (cancellable))
    g_cond_wait_until (&load->cond, &load->lock, g_get_monotonic_time () + 100 * G_TIME_SPAN_MILLISECOND);
  done = load->consumed >= sent;
  g_mutex_unlock (&load->lock);

  return done;
}

/* #414: Beginn (wartet, bis der UI-Thread ihn übernommen hat) bzw. Ende eines Durchgangs ohne
 * Einträge an den UI-Thread (add_messages_block). */
static gboolean
post_frame (GTask    *task,
            gboolean  begin)
{
  LoadMessages *load = g_task_get_task_data (task);
  MessagesBlock *block = g_new0 (MessagesBlock, 1);

  block->self = g_object_ref (g_task_get_source_object (task));
  block->load = g_atomic_rc_box_acquire (load);
  block->generation = load->generation;
  block->items = g_ptr_array_new ();
  block->begin = begin;
  block->end = !begin;
  block->last = !begin;

  return post_block (block, g_task_get_cancellable (task), begin);
}

/* Im Worker: Flags, die eine Nachricht in diesem Ordner ausblenden. Gelöschte und Spam wie
 * Evolution (message_list_get_hide_deleted/_junk, message-list.c:4585-4666), ohne die
 * Ausnahmen für Suchordner (gibt es noch nicht). #353: zweiter Nutzer folder_changes_thread. */
static guint32
hidden_flags (CamelFolder *folder)
{
  guint32 store_flags = camel_store_get_flags (camel_folder_get_parent_store (folder));
  guint32 folder_flags = camel_folder_get_flags (folder);
  guint32 hidden = 0;

  if ((store_flags & CAMEL_STORE_VTRASH) == 0 || (folder_flags & CAMEL_FOLDER_IS_TRASH) == 0)
    hidden |= CAMEL_MESSAGE_DELETED;
  if ((store_flags & (CAMEL_STORE_VJUNK | CAMEL_STORE_REAL_JUNK_FOLDER)) != 0 &&
      (folder_flags & (CAMEL_FOLDER_IS_JUNK | CAMEL_FOLDER_IS_TRASH)) == 0)
    hidden |= CAMEL_MESSAGE_JUNK;

  return hidden;
}

/* Im Worker: alle Nachrichten des Ordners aus der Summary, sortiert, in Blöcken in den
 * UI-Thread. *signature (UIDs und Flags) erkennt einen unveränderten Stand, der dann nicht
 * erneut geschickt wird. Ausgeblendet wird nach hidden_flags. FALSE nach Abbruch. */
static gboolean
send_messages (GTask       *task,
               CamelFolder *folder,
               const char  *pass,
               guint       *signature)
{
  RevolutionMailView *self = g_task_get_source_object (task);
  LoadMessages *load = g_task_get_task_data (task);
  GCancellable *cancellable = g_task_get_cancellable (task);
  guint32 hidden = hidden_flags (folder);
  g_autoptr (GPtrArray) infos = g_ptr_array_new_with_free_func (g_object_unref);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autoptr (GPtrArray) conversations = NULL;
  gboolean merge;
  guint hash = 0, unread = 0, previews = 0, i = 0, n;
  GPtrArray *uids;

  /* #414: Ab hier keine Live-Änderungen mehr in die Liste, bis der Durchgang fertig ist. Beim
   * Cache-Durchgang gilt das schon seit der Auswahl (on_folder_selected); ein Warten auf den beim
   * Start belegten UI-Thread verzögerte sonst die erste Zeile. */
  if (g_str_equal (pass, "Server") && !post_frame (task, TRUE))
    return FALSE;
  merge = load->shown && g_str_equal (pass, "Server");

  camel_folder_summary_prepare_fetch_all (camel_folder_get_folder_summary (folder), NULL);
  uids = camel_folder_get_uids (folder);
  for (guint j = 0; j < uids->len; j++)
    {
      CamelMessageInfo *info = camel_folder_get_message_info (folder, uids->pdata[j]);
      guint32 flags;

      if (info == NULL)
        continue;

      flags = camel_message_info_get_flags (info);
      if ((flags & hidden) != 0)
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
      return post_frame (task, FALSE);
    }
  *signature = hash;

  g_ptr_array_sort_values_with_data (infos, compare_messages, folder);

  /* #357: je Thread eine Zeile (mail-thread.c), ungelesen zählen die Konversationen. */
  if (load->conversations)
    {
      conversations = revolution_mail_thread_build (folder, infos, now);
      unread = 0;
      for (guint j = 0; j < conversations->len; j++)
        unread += revolution_mail_message_get_unread (conversations->pdata[j]);
    }
  n = conversations != NULL ? conversations->len : infos->len;

  do
    {
      MessagesBlock *block;
      /* Abgleich in einem Block: der UI-Thread legt dabei kaum neue Zeilen an. */
      guint end = merge ? n : MIN (i + MESSAGES_BLOCK, n);

      if (g_cancellable_is_cancelled (cancellable))
        return FALSE;

      block = g_new0 (MessagesBlock, 1);
      block->self = g_object_ref (self);
      block->load = g_atomic_rc_box_acquire (load);
      block->generation = load->generation;
      block->items = g_ptr_array_new_full (end - i, g_object_unref);
      block->pass = pass;
      block->reset = i == 0;
      block->merge = merge;
      block->total = n;
      block->unread = unread;

      for (; i < end; i++)
        {
          RevolutionMailMessage *item = conversations != NULL ? g_object_ref (conversations->pdata[i])
                                                              : revolution_mail_message_new (infos->pdata[i], now);

          previews += revolution_mail_message_has_preview (item);
          g_ptr_array_add (block->items, item);
        }

      block->last = i >= n;
      block->previews = previews;
      if (!post_block (block, cancellable, TRUE))
        return FALSE;
    }
  while (i < n);

  return TRUE;
}

/* #353: Ordner für Flags und Live-Änderungen, sobald er geholt ist (vor Cache- und
 * Server-Durchgang, also auch ohne Netz wie Evolution), im UI-Thread (use_folder). */
typedef struct
{
  RevolutionMailView *self;
  CamelFolder        *folder;
  guint               generation;
} FolderReady;

static gboolean use_folder (gpointer data);
static void folder_ready_free (FolderReady *ready);

/* #350: Höchstens ein Server-Abgleich je Ordner gleichzeitig (Prüf-Lauf #350): da ein Wechsel ihn
 * nicht mehr abbricht, startet ein erneutes Wählen sonst einen zweiten. Läuft schon einer, zeigt
 * die Liste den Cache; den Server-Stand bringt CamelFolder::changed (#353) und der Zähler
 * (folder_refreshed) nach dem laufenden Abgleich. Schlüssel „<UID>/<Ordner>“, aus Worker-Threads. */
static GMutex refreshing_lock;
static GHashTable *refreshing;

static gboolean
refresh_begin (const char *key)
{
  gboolean begun;

  g_mutex_lock (&refreshing_lock);
  if (refreshing == NULL)
    refreshing = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  begun = !g_hash_table_contains (refreshing, key);
  if (begun)
    g_hash_table_add (refreshing, g_strdup (key));
  g_mutex_unlock (&refreshing_lock);

  return begun;
}

static void
refresh_end (const char *key)
{
  g_mutex_lock (&refreshing_lock);
  g_hash_table_remove (refreshing, key);
  g_mutex_unlock (&refreshing_lock);
}

static gboolean
folder_refreshed (gpointer data)
{
  FolderReady *ready = data;

  revolution_mail_view_watch_folder (ready->self, ready->folder, FALSE, TRUE);

  return G_SOURCE_REMOVE;
}

static void
load_messages_thread (GTask        *task,
                      gpointer      source_object G_GNUC_UNUSED,
                      gpointer      task_data,
                      GCancellable *cancellable)
{
  LoadMessages *load = task_data;
  g_autoptr (CamelService) service = camel_session_ref_service (load->session, load->uid);
  /* #408: Camel bekommt je Vorgang ein eigenes Cancellable, das am Cancellable des Ladens bzw. der
   * Ansicht hängt (camel_operation_new_proxy, camel-operation.c:254-300), wie Evolution je
   * EActivity. Ein Abbruch von innen (microsoft365: soup_session_abort beim Trennen, Beleg Prüfer
   * #408 R2) trifft so nur diesen Vorgang, nicht alle späteren der Ansicht. */
  g_autoptr (GCancellable) load_op = camel_operation_new_proxy (cancellable);
  g_autoptr (GCancellable) refresh_op = camel_operation_new_proxy (load->refresh_cancellable);
  g_autoptr (CamelFolder) folder = NULL;
  g_autoptr (CamelFolder) list = NULL;
  g_autoptr (CamelFolder) sent = NULL;
  g_autofree char *key = NULL;
  GError *error = NULL;
  guint signature = 0;
  gboolean refreshed_ok;

  if (!CAMEL_IS_STORE (service))
    {
      g_task_return_new_error (task, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE,
                               _("Konto %s nicht verfügbar"), load->uid);
      return;
    }

  /* Ordner immer über camel_store_get_folder_sync (README „Camel-Ordner“). */
  folder = camel_store_get_folder_sync (CAMEL_STORE (service), load->full_name, 0, load_op, &error);
  if (folder == NULL)
    {
      g_task_return_error (task, error);
      return;
    }

  /* #357: Konversation aus Ordner und Gesendet: Liste, Flags und Live-Änderungen über den Suchordner
   * (mail-thread.c); abgeglichen wird weiter der Ordner selbst. #408: eigenes Cancellable je Vorgang. */
  list = load->sent_full_name != NULL ? revolution_mail_thread_folder_sync (folder, load->sent_full_name, &sent,
                                                                            load_op, &error)
                                      : g_object_ref (folder);
  if (list == NULL)
    {
      g_task_return_error (task, error);
      return;
    }

  {
    FolderReady *ready = g_new0 (FolderReady, 1);

    ready->self = g_object_ref (g_task_get_source_object (task));
    ready->folder = g_object_ref (list);
    ready->generation = load->generation;
    g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT, use_folder, ready, (GDestroyNotify) folder_ready_free);
  }

  /* #350: Den Abgleich ein Ordnerwechsel nicht abbrechen lassen, wie Evolution
   * (e_mail_reader_refresh_folder, e-mail-reader-utils.c:861-881: Abbruch nur über die
   * Aktivität, nicht beim Wechsel). Ein abgebrochener Abgleich hinterlässt den halben Stand als
   * Zählerstand des Stores (gemessen: 63 statt 100 ungelesen, auch nach Neustart; EDS
   * camel-imapx-server.c:6590-6600, camel-store-summary.c:1011-1050). Nach dem Wechsel läuft er
   * zu Ende, die Liste bekommt ihn nicht mehr (send_messages prüft cancellable). */
  if (!send_messages (task, list, "Cache", &signature))
    {
      g_task_return_error_if_cancelled (task);
      return;
    }

  key = g_strdup_printf ("%s/%s", load->uid, load->full_name);
  if (!refresh_begin (key))
    {
      g_debug ("Nachrichten: %s %s Abgleich läuft schon", load->uid, load->full_name);
      if (!g_task_return_error_if_cancelled (task))
        g_task_return_boolean (task, TRUE);
      return;
    }

  refreshed_ok = revolution_eds_folder_refresh_info_sync (folder, refresh_op, &error);
  refresh_end (key);
  /* #408: Offline (Pille „Offline“) bleibt es beim Stand des Caches. imapx überspringt den
   * Abgleich dann (camel-imapx-folder.c:878-880), microsoft365 meldet „You must be working
   * online“ (camel-m365-folder.c:961, camel-m365-store.c:1754-1758); das ist kein Fehler.
   * #421: ebenso, wenn main.c den Store nach gescheiterter Verbindung offline geschaltet hat. */
  if (!refreshed_ok && error != NULL &&
      (!camel_session_get_online (load->session) ||
       (CAMEL_IS_OFFLINE_STORE (service) && !camel_offline_store_get_online (CAMEL_OFFLINE_STORE (service)))))
    {
      g_debug ("Nachrichten: %s %s offline, Stand des Caches (%s)", load->uid, load->full_name, error->message);
      g_clear_error (&error);
      if (!g_task_return_error_if_cancelled (task))
        g_task_return_boolean (task, TRUE);
      return;
    }
  /* #357: Gesendet des neuen Suchordners einmal abgleichen (mail-thread.c); die Liste folgt über
   * „changed“ des Suchordners. Ein Fehler hier bleibt im Log, die Liste zeigt dann den Cache. */
  if (refreshed_ok && sent != NULL)
    {
      g_autoptr (GError) sent_error = NULL;

      if (!revolution_eds_folder_refresh_info_sync (sent, refresh_op, &sent_error))
        g_debug ("Konversationen: %s %s nicht abgeglichen: %s", load->uid, camel_folder_get_full_name (sent),
                 sent_error->message);
    }
  if (!refreshed_ok)
    {
      if (error == NULL)
        g_task_return_error_if_cancelled (task);
      else
        g_task_return_error (task, error);
      return;
    }

  /* #350: abgeglichen, jetzt den Ungelesen-Zähler der Seitenleiste setzen (auch nach Wechsel). */
  {
    FolderReady *refreshed = g_new0 (FolderReady, 1);

    refreshed->self = g_object_ref (g_task_get_source_object (task));
    refreshed->folder = g_object_ref (folder);
    g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT, folder_refreshed, refreshed,
                                (GDestroyNotify) folder_ready_free);
  }

  if (g_cancellable_is_cancelled (cancellable) ||
      !send_messages (task, list, "Server", &signature))
    {
      g_task_return_error_if_cancelled (task);
      return;
    }

  g_task_return_boolean (task, TRUE);
}

/* #353: Live-Aktualisierung der Liste über CamelFolder::changed wie Evolution
 * (message_list_folder_changed_cb, message-list.c:5232-5250: im UI-Thread, sonst dorthin
 * weitergereicht). Camel sendet „changed“ über camel_session_idle_add im Hauptkontext
 * (EDS camel-folder.c:2873-2899), bei eingefrorenem Ordner aber sofort im aufrufenden Thread
 * (camel-folder.c:2860-2864); daher kopieren und per g_main_context_invoke in den UI-Thread.
 * Dort sammeln zwei Mengen die UIDs (neu/geändert bzw. entfernt), ein Worker-GTask nach dem
 * anderen liest die Einträge aus der Summary (Architektur 1), der UI-Thread ändert nur das
 * Modell: geänderte am bestehenden Eintrag (Auswahl und Scrollposition bleiben), neue und
 * entfernte per g_list_store_splice. Nacheinander, damit ein älterer Stand nie einen neueren
 * überschreibt. */
typedef struct
{
  RevolutionMailView *self;
  CamelFolder        *folder;
  GPtrArray          *changed;
  GPtrArray          *removed;
  GPtrArray          *items;
  /* Ordnerwechsel A→B→A: Ergebnis zur alten Liste verwerfen (load_generation). */
  guint               generation;
  /* #357: Konversationen: ganze Liste neu threaden und abgleichen (folder_changes_thread). */
  gboolean            full;
} FolderChanges;

static void
folder_changes_free (FolderChanges *changes)
{
  g_object_unref (changes->self);
  g_object_unref (changes->folder);
  g_ptr_array_unref (changes->changed);
  g_ptr_array_unref (changes->removed);
  g_clear_pointer (&changes->items, g_ptr_array_unref);
  g_free (changes);
}

static void
set_list_counts (RevolutionMailView *self)
{
  GListModel *model = G_LIST_MODEL (self->messages);
  guint n_items = g_list_model_get_n_items (model);
  guint unread = 0;

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr (RevolutionMailMessage) item = g_list_model_get_item (model, i);

      unread += revolution_mail_message_get_unread (item);
    }

  set_list_subtitle (self, n_items, unread);
  gtk_stack_set_visible_child_name (self->list_stack, n_items > 0 ? "list" : "empty");
}

/* #410: verschiedene UIDs in der Liste, fürs Log (Prüfung auf doppelte Zeilen,
 * tests/erstes-oeffnen.sh). */
static guint
distinct_uids (RevolutionMailView *self)
{
  GListModel *model = G_LIST_MODEL (self->messages);
  g_autoptr (GHashTable) uids = g_hash_table_new (g_str_hash, g_str_equal);

  for (guint i = 0; i < g_list_model_get_n_items (model); i++)
    {
      g_autoptr (RevolutionMailMessage) item = g_list_model_get_item (model, i);

      g_hash_table_add (uids, (gpointer) revolution_mail_message_get_uid (item));
    }

  return g_hash_table_size (uids);
}

/* Im UI-Thread: Ergebnis eines Durchgangs ins Modell, je ein Durchlauf über die Liste. */
static void
apply_folder_changes (RevolutionMailView *self,
                      GPtrArray          *items,
                      GPtrArray          *removed)
{
  GListModel *model = G_LIST_MODEL (self->messages);
  GtkAdjustment *vadjustment = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (self->message_list));
  gboolean at_top = gtk_adjustment_get_value (vadjustment) < 1.0;
  g_autoptr (GHashTable) remove = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GHashTable) update = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GPtrArray) added = g_ptr_array_new ();
  guint n_added = 0, n_removed = 0, n_changed = 0, i, j;

  for (i = 0; i < removed->len; i++)
    g_hash_table_add (remove, removed->pdata[i]);
  for (i = 0; i < items->len; i++)
    g_hash_table_insert (update, (gpointer) revolution_mail_message_get_uid (items->pdata[i]), items->pdata[i]);

  /* Von hinten: entfernte als zusammenhängende Läufe, geänderte am Eintrag (wie merge_messages). */
  for (i = g_list_model_get_n_items (model); i > 0;)
    {
      guint end = i;

      while (i > 0)
        {
          g_autoptr (RevolutionMailMessage) old = g_list_model_get_item (model, i - 1);
          const char *uid = revolution_mail_message_get_uid (old);
          RevolutionMailMessage *item;

          if (!g_hash_table_contains (remove, uid))
            {
              item = g_hash_table_lookup (update, uid);
              if (item != NULL)
                {
                  n_changed += revolution_mail_message_update (old, item);
                  g_hash_table_remove (update, uid);
                }
              break;
            }
          i--;
        }

      if (i < end)
        {
          g_list_store_splice (self->messages, i, end - i, NULL, 0);
          n_removed += end - i;
        }
      else
        i--;
    }

  /* Übrig sind neue Einträge: nach Datum an ihren Platz, untereinander in der Reihenfolge des
   * Workers (#414: compare_messages wie send_messages, bei gleichem Datum die höhere UID zuerst;
   * sonst ordnete der folgende Abgleich sie um). Bei gleichem Datum wie ein bestehender Eintrag
   * kommen sie davor (meist die höhere UID); UIDs vergleicht nur Camel (Worker). */
  for (i = 0; i < items->len; i++)
    if (g_hash_table_contains (update, revolution_mail_message_get_uid (items->pdata[i])))
      g_ptr_array_add (added, items->pdata[i]);

  for (i = 0, j = 0; j < added->len;)
    {
      guint n_items = g_list_model_get_n_items (model), start = j;
      gint64 before = G_MININT64;

      /* Erster Eintrag, der nicht neuer ist; davor kommen alle neuen ab j, die nicht älter sind. */
      for (; i < n_items; i++)
        {
          g_autoptr (RevolutionMailMessage) old = g_list_model_get_item (model, i);
          gint64 timestamp = revolution_mail_message_get_timestamp (old);

          if (timestamp <= revolution_mail_message_get_timestamp (added->pdata[j]))
            {
              before = timestamp;
              break;
            }
        }

      while (j < added->len && revolution_mail_message_get_timestamp (added->pdata[j]) >= before)
        j++;
      g_list_store_splice (self->messages, i, 0, &added->pdata[start], j - start);
      n_added += j - start;
      i += j - start;
    }

  if (n_added > 0 && at_top)
    gtk_list_view_scroll_to (self->message_list, 0, GTK_LIST_SCROLL_NONE, NULL);
  set_list_counts (self);
  g_debug ("Nachrichten: %s Änderung %u neu, %u entfernt, %u geändert, Liste %u, %u verschiedene UIDs",
           self->loading, n_added, n_removed, n_changed,
           g_list_model_get_n_items (model), distinct_uids (self));
  /* #414: Test-Hook auch für Einträge aus Live-Änderungen (Auswahl vor dem Abgleich). */
  select_message_from_env (self);
}

static void
folder_changes_thread (GTask        *task,
                       gpointer      source_object G_GNUC_UNUSED,
                       gpointer      task_data,
                       GCancellable *cancellable G_GNUC_UNUSED)
{
  FolderChanges *changes = task_data;
  guint32 hidden = hidden_flags (changes->folder);
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();

  g_autoptr (GPtrArray) infos = g_ptr_array_new_with_free_func (g_object_unref);

  /* #357: Eine neue oder entfernte Nachricht kann Threads verbinden oder trennen; daher in
   * Konversationen alles neu threaden wie Evolution im Thread-Modus (message_list_folder_changed,
   * message-list.c:5127-5128, 5178-5186: Neuaufbau statt Einzeländerung) und als Ganzes abgleichen. */
  if (changes->full)
    {
      GPtrArray *uids;

      camel_folder_summary_prepare_fetch_all (camel_folder_get_folder_summary (changes->folder), NULL);
      uids = camel_folder_get_uids (changes->folder);
      for (guint i = 0; i < uids->len; i++)
        {
          CamelMessageInfo *info = camel_folder_get_message_info (changes->folder, uids->pdata[i]);

          if (info != NULL && (camel_message_info_get_flags (info) & hidden) == 0)
            g_ptr_array_add (infos, info);
          else
            g_clear_object (&info);
        }
      camel_folder_free_uids (changes->folder, uids);
      changes->items = revolution_mail_thread_build (changes->folder, infos, now);
      g_task_return_boolean (task, TRUE);
      return;
    }

  for (guint i = 0; i < changes->changed->len; i++)
    {
      const char *uid = changes->changed->pdata[i];
      CamelMessageInfo *info = camel_folder_get_message_info (changes->folder, uid);

      if (info == NULL || (camel_message_info_get_flags (info) & hidden) != 0)
        {
          g_ptr_array_add (changes->removed, g_strdup (uid));
          g_clear_object (&info);
        }
      else
        g_ptr_array_add (infos, info);
    }

  /* #414: sortiert wie send_messages (apply_folder_changes übernimmt die Reihenfolge). */
  g_ptr_array_sort_values_with_data (infos, compare_messages, changes->folder);
  changes->items = g_ptr_array_new_full (infos->len, g_object_unref);
  for (guint i = 0; i < infos->len; i++)
    g_ptr_array_add (changes->items, revolution_mail_message_new (infos->pdata[i], now));

  g_task_return_boolean (task, TRUE);
}

static void
on_folder_changes_done (GObject      *source_object,
                        GAsyncResult *result,
                        gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionMailView *self = REVOLUTION_MAIL_VIEW (source_object);
  FolderChanges *changes = g_task_get_task_data (G_TASK (result));

  self->changes_running = FALSE;
  if (changes->folder != self->camel_folder || changes->generation != self->load_generation)
    ;
  else if (self->flags_running || self->blocks_running)
    {
      /* Inzwischen bedient (mark_message): der gelesene Stand kann älter sein als die Zeile.
       * Später neu lesen, wenn alle Flag-Aufträge durch sind (on_flags_set). #410: ebenso,
       * solange ein Durchgang Blöcke schickt (bis zum letzten Block, add_messages_block). */
      for (guint i = 0; i < changes->changed->len; i++)
        g_hash_table_add (self->pending_changed, g_strdup (changes->changed->pdata[i]));
      for (guint i = 0; i < changes->removed->len; i++)
        if (!g_hash_table_contains (self->pending_changed, changes->removed->pdata[i]))
          g_hash_table_add (self->pending_removed, g_strdup (changes->removed->pdata[i]));
    }
  else if (changes->full)
    {
      GtkAdjustment *vadjustment = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (self->message_list));
      gboolean at_top = gtk_adjustment_get_value (vadjustment) < 1.0;
      guint added = 0, removed = 0, changed = 0, moved = 0;

      merge_messages (self, changes->items, &added, &removed, &changed, &moved);
      /* #418: wie der Abgleich (add_messages_block): stand die Liste oben, bleibt die neueste Konversation
       * sichtbar. Sonst hält GtkListView die bisher erste Zeile, neue und nach oben verschobene
       * Konversationen lägen darüber außerhalb des sichtbaren Teils (Beleg #412). */
      if (at_top && g_list_model_get_n_items (G_LIST_MODEL (self->messages)) > 0)
        gtk_list_view_scroll_to (self->message_list, 0, GTK_LIST_SCROLL_NONE, NULL);
      set_list_counts (self);
      g_debug ("Nachrichten: %s Änderung (Konversationen) %u neu, %u entfernt, %u geändert, %u verschoben, Liste %u",
               self->loading, added, removed, changed, moved, g_list_model_get_n_items (G_LIST_MODEL (self->messages)));
      log_order (self);
      follow_conversation (self);
      select_message_from_env (self);
    }
  else
    apply_folder_changes (self, changes->items, changes->removed);
  start_folder_changes (self);
}

static GPtrArray *
steal_uids (GHashTable *uids)
{
  GPtrArray *array = g_hash_table_steal_all_keys (uids);

  g_ptr_array_set_free_func (array, g_free);

  return array;
}

static void
start_folder_changes (RevolutionMailView *self)
{
  g_autoptr (GTask) task = NULL;
  FolderChanges *changes;

  /* Während Flag-Aufträgen nicht: die Zeilen zeigen schon den bedienten Stand (mark_message).
   * #410: auch nicht zwischen den Blöcken eines Durchgangs (add_messages_block). */
  if (self->changes_running || self->flags_running || self->blocks_running || self->camel_folder == NULL ||
      (g_hash_table_size (self->pending_changed) == 0 && g_hash_table_size (self->pending_removed) == 0))
    return;

  changes = g_new0 (FolderChanges, 1);
  changes->self = g_object_ref (self);
  changes->folder = g_object_ref (self->camel_folder);
  changes->changed = steal_uids (self->pending_changed);
  changes->removed = steal_uids (self->pending_removed);
  changes->generation = self->load_generation;
  changes->full = self->conversations;
  self->changes_running = TRUE;

  task = g_task_new (self, NULL, on_folder_changes_done, NULL);
  g_task_set_source_tag (task, start_folder_changes);
  g_task_set_task_data (task, changes, (GDestroyNotify) folder_changes_free);
  g_task_run_in_thread (task, folder_changes_thread);
}

static gboolean
queue_folder_changes (gpointer data)
{
  FolderChanges *changes = data;
  RevolutionMailView *self = changes->self;

  if (changes->folder != self->camel_folder)
    return G_SOURCE_REMOVE;

  for (guint i = 0; i < changes->changed->len; i++)
    {
      g_hash_table_remove (self->pending_removed, changes->changed->pdata[i]);
      g_hash_table_add (self->pending_changed, g_strdup (changes->changed->pdata[i]));
    }
  for (guint i = 0; i < changes->removed->len; i++)
    {
      g_hash_table_remove (self->pending_changed, changes->removed->pdata[i]);
      g_hash_table_add (self->pending_removed, g_strdup (changes->removed->pdata[i]));
    }
  start_folder_changes (self);

  return G_SOURCE_REMOVE;
}

static void
copy_uids (GPtrArray *to,
           GPtrArray *from)
{
  for (guint i = 0; from != NULL && i < from->len; i++)
    g_ptr_array_add (to, g_strdup (from->pdata[i]));
}

static void
on_folder_changed (CamelFolder           *folder,
                   CamelFolderChangeInfo *info,
                   RevolutionMailView    *self)
{
  FolderChanges *changes;

  if (info == NULL)
    return;

  changes = g_new0 (FolderChanges, 1);
  changes->self = g_object_ref (self);
  changes->folder = g_object_ref (folder);
  changes->changed = g_ptr_array_new_with_free_func (g_free);
  changes->removed = g_ptr_array_new_with_free_func (g_free);
  copy_uids (changes->changed, info->uid_added);
  copy_uids (changes->changed, info->uid_changed);
  copy_uids (changes->removed, info->uid_removed);

  g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT, queue_folder_changes, changes,
                              (GDestroyNotify) folder_changes_free);
}

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
/* #409: echter Ordner und UID eines Auftrags, im Worker beim Start aufgelöst (set_flags_thread). */
typedef struct
{
  CamelFolder *folder;
  char        *uid;
} FlagsLocation;

static void
flags_location_free (FlagsLocation *location)
{
  g_object_unref (location->folder);
  g_free (location->uid);
  g_free (location);
}

typedef struct _SetFlags
{
  CamelFolder *folder;
  char        *uid;
  /* Vom Worker einmal gesetzt (g_atomic_pointer_set), danach unverändert; liest flags_deadline. */
  FlagsLocation *location;
  guint32      mask;
  guint32      set;
  const char  *what;
  char        *loading;
  gint64       started;
} SetFlags;

static void
set_flags_free (SetFlags *data)
{
  g_object_unref (data->folder);
  g_free (data->uid);
  g_clear_pointer (&data->location, flags_location_free);
  g_free (data->loading);
  g_free (data);
}

enum { FLAGS_SAVED, FLAGS_LOCAL };

/* #351: Im Suchordner (Favoriten; #357 auch Konversationen, mail-thread.c) am echten Ordner setzen und
 * speichern wie Evolution (mail_reader_replace_vee_folder_with_real, e-mail-reader.c:425-450,
 * camel_vee_folder_get_location);
 * camel_folder_synchronize_sync bzw. das Speichern der Summary des Suchordners speichert die
 * Quellordner nicht (camel-vee-folder.c:1465-1477). Die Liste folgt über „changed“ der Quelle und des
 * Suchordners. Gibt den echten Ordner (mit Referenz) und in real_uid dessen UID zurück, NULL wenn die
 * Nachricht nicht mehr im Suchordner ist (z. B. „Markiert“ nach Entfernen der Markierung). Im Worker. */
static CamelFolder *
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

  location->folder = real_folder (data->folder, data->uid, &location->uid);
  if (location->folder == NULL)
    {
      g_free (location);
      g_task_return_new_error (task, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID_UID,
                               _("Nachricht %s nicht gefunden"), data->uid);
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
#define FLAGS_DEADLINE_SECONDS 10

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

static gboolean
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

static void start_flags (RevolutionMailView *self);

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
  if (self->cancellable == NULL || g_cancellable_is_cancelled (self->cancellable))
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
      revolution_mail_view_show_error (self, _("Markierung nicht gespeichert"), detail, FALSE);
    }
  self->running = NULL;
  start_flags (self);
  if (!self->flags_running && !self->flags_saving)
    hold_for_flags (self, FALSE);
  if (!self->flags_running && self->cancellable != NULL)
    start_folder_changes (self);
}

static void
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

static void
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
static void
mark_read (RevolutionMailView    *self,
           RevolutionMailMessage *message,
           gboolean               read,
           const char            *what)
{
  mark_message (self, message, CAMEL_MESSAGE_SEEN | (read ? 0 : CAMEL_MESSAGE_DELETED),
                read ? CAMEL_MESSAGE_SEEN : 0, what);
}

static void
mark_starred (RevolutionMailView    *self,
              RevolutionMailMessage *message,
              gboolean               starred,
              const char            *what)
{
  mark_message (self, message, CAMEL_MESSAGE_FLAGGED, starred ? CAMEL_MESSAGE_FLAGGED : 0, what);
}

static RevolutionMailMessage *
selected_message (RevolutionMailView *self)
{
  return gtk_single_selection_get_selected_item (self->message_selection);
}

/* #359: message.reply/reply-all (Strg+R, Strg+Umschalt+R wie Evolution e-mail-reader.c:3354-3387) auf die
 * gewählte Nachricht, in einer Konversation auf die neueste (Mitglieder alt → neu, mail-message.c),
 * geladen wie im Lesebereich (on_message_selected); Absender = Konto des echten Ordners (find_place). */
static void
open_reply (RevolutionMailView     *self,
            RevolutionComposerType  type)
{
  RevolutionMailMessage *message = selected_message (self);
  RevolutionMailFolder *folder = self->folder;
  g_autoptr (RevolutionMailMessage) member = NULL;
  g_autoptr (RevolutionMailFolder) place = NULL;
  g_autoptr (RevolutionMailAccount) account = NULL, place_account = NULL;
  GListModel *members;

  if (message == NULL || folder == NULL || self->session == NULL)
    return;

  members = revolution_mail_message_get_members (message);
  if (members != NULL && g_list_model_get_n_items (members) > 0)
    {
      member = g_list_model_get_item (members, g_list_model_get_n_items (members) - 1);
      message = member;
      if (self->thread_folder != NULL)
        folder = self->thread_folder;
    }
  place = revolution_mail_view_find_place (self, message);
  g_object_get (folder, "account", &account, NULL);
  g_object_get (place, "account", &place_account, NULL);
  if (account == NULL || place_account == NULL)
    return;
  /* Antworten im Gesendet-Ordner geht an alle wie Evolution (em-composer-utils.c:5149-5151). */
  if (type == REVOLUTION_COMPOSER_REPLY &&
      (revolution_mail_folder_get_flags (place) & CAMEL_FOLDER_TYPE_MASK) == CAMEL_FOLDER_TYPE_SENT)
    {
      g_debug ("Composer: Nachricht aus Gesendet, Allen antworten");
      type = REVOLUTION_COMPOSER_REPLY_ALL;
    }
  gtk_window_present (revolution_composer_new (gtk_window_get_application (GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self)))),
                                               self->registry, self->session, type,
                                               revolution_mail_account_get_uid (place_account),
                                               revolution_mail_account_get_uid (account),
                                               revolution_mail_folder_get_full_name (folder),
                                               revolution_mail_message_get_uid (message)));
}

static void
on_reply (GSimpleAction *action G_GNUC_UNUSED,
          GVariant      *parameter G_GNUC_UNUSED,
          gpointer       user_data)
{
  open_reply (user_data, REVOLUTION_COMPOSER_REPLY);
}

static void
on_reply_all (GSimpleAction *action G_GNUC_UNUSED,
              GVariant      *parameter G_GNUC_UNUSED,
              gpointer       user_data)
{
  open_reply (user_data, REVOLUTION_COMPOSER_REPLY_ALL);
}

static void
on_mark_read (GSimpleAction *action G_GNUC_UNUSED,
              GVariant      *parameter G_GNUC_UNUSED,
              gpointer       user_data)
{
  mark_read (user_data, selected_message (user_data), TRUE, "gelesen");
}

static void
on_mark_unread (GSimpleAction *action G_GNUC_UNUSED,
                GVariant      *parameter G_GNUC_UNUSED,
                gpointer       user_data)
{
  mark_read (user_data, selected_message (user_data), FALSE, "ungelesen");
}

static void
on_star_on (GSimpleAction *action G_GNUC_UNUSED,
            GVariant      *parameter G_GNUC_UNUSED,
            gpointer       user_data)
{
  mark_starred (user_data, selected_message (user_data), TRUE, "markiert");
}

static void
on_star_off (GSimpleAction *action G_GNUC_UNUSED,
             GVariant      *parameter G_GNUC_UNUSED,
             gpointer       user_data)
{
  mark_starred (user_data, selected_message (user_data), FALSE, "nicht markiert");
}

/* message.star (Stern-Knopf der Nachricht, mail-view.blp): Zustand folgt dem Eintrag
 * (update_message_actions), Umschalten setzt das Flag. */
static void
on_star_change_state (GSimpleAction *action G_GNUC_UNUSED,
                      GVariant      *state,
                      gpointer       user_data)
{
  gboolean starred = g_variant_get_boolean (state);

  mark_starred (user_data, selected_message (user_data), starred, starred ? "markiert" : "nicht markiert");
}

/* Nur die passenden Aktionen sind aktiv wie Evolution (Gelesen nur bei ungelesener Auswahl,
 * e-mail-reader.c mail_reader_update_actions); Menüeinträge mit hidden-when blenden die
 * übrigen aus. */
static void
update_message_actions (RevolutionMailView *self)
{
  RevolutionMailMessage *message = self->camel_folder != NULL ? selected_message (self) : NULL;
  gboolean unread = message != NULL && revolution_mail_message_get_unread (message);
  gboolean starred = message != NULL && revolution_mail_message_get_starred (message);
  struct { const char *name; gboolean enabled; } actions[] = {
    { "mark-read", unread },
    { "mark-unread", message != NULL && !unread },
    { "star-on", message != NULL && !starred },
    { "star-off", starred },
    { "star", message != NULL },
    { "reply", message != NULL },
    { "reply-all", message != NULL },
  };
  GAction *star = g_action_map_lookup_action (self->message_actions, "star");

  for (gsize i = 0; i < G_N_ELEMENTS (actions); i++)
    g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (self->message_actions, actions[i].name)),
                                 actions[i].enabled);
  g_simple_action_set_state (G_SIMPLE_ACTION (star), g_variant_new_boolean (starred));
}

/* Gelesen nach Verzögerung wie Evolution (schedule_timeout_mark_seen, e-mail-reader.c:3747-3777,
 * mail_reader_message_seen 4586-4598; GSettings mark-seen true, mark-seen-timeout 1500 ms,
 * org.gnome.evolution.mail.gschema.xml.in:424-438). Evolution startet den Timer, sobald die
 * Anzeige geladen ist (mail_reader_load_changed_cb, 3784-3801); hier ebenso über das Signal „shown“
 * des Lesebereichs (#354, mail-reader.c). */
static gboolean
on_mark_seen_timeout (gpointer user_data)
{
  RevolutionMailView *self = user_data;
  RevolutionMailMessage *message = selected_message (self);

  self->seen_id = 0;
  if (message != NULL && message == self->selected && revolution_mail_message_get_unread (message))
    mark_read (self, message, TRUE, "gelesen (automatisch)");

  return G_SOURCE_REMOVE;
}

static void
watch_selected_message (RevolutionMailView *self)
{
  RevolutionMailMessage *message = selected_message (self);

  /* GtkSingleSelection meldet dieselbe Auswahl erneut, wenn sich ihre Position ändert. */
  if (message == self->selected)
    return;

  g_clear_handle_id (&self->seen_id, g_source_remove);
  clear_member_seen (self);
  g_clear_signal_handler (&self->selected_handler, self->selected);
  g_set_object (&self->selected, message);
  if (message != NULL)
    self->selected_handler = g_signal_connect_swapped (message, "notify", G_CALLBACK (update_message_actions), self);
  update_message_actions (self);
}

/* #354: Anzeige der gewählten Nachricht fertig (RevolutionMailReader::shown): Gelesen-Timer starten,
 * wie Evolution erst nach dem Laden (mail_reader_load_changed_cb, e-mail-reader.c:3784-3801). */
static void
on_reader_shown (RevolutionMailView *self)
{
  RevolutionMailMessage *message = self->selected;
  int timeout;

  if (message == NULL || self->seen_id != 0 || !revolution_mail_message_get_unread (message) ||
      !g_settings_get_boolean (self->settings, "mark-seen"))
    return;

  timeout = MAX (g_settings_get_int (self->settings, "mark-seen-timeout"), 0);
  g_debug ("Gelesen: %s %s in %d ms", self->loading, revolution_mail_message_get_uid (message), timeout);
  self->seen_id = g_timeout_add_full (G_PRIORITY_DEFAULT, timeout, on_mark_seen_timeout, self, NULL);
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

/* #357: Gelesen-Timer je Nachricht der Konversation: jede angezeigte (aufgeklappte, geladene) ungelesene
 * Nachricht wird nach mark-seen-timeout gelesen wie oben die gewählte (Evolution e-mail-reader.c:3784-3801).
 * Ein Wechsel der Auswahl verwirft alle (clear_member_seen). */
typedef struct
{
  RevolutionMailView    *self;
  RevolutionMailMessage *member;
} MemberSeen;

static void
member_seen_free (MemberSeen *seen)
{
  g_object_unref (seen->member);
  g_free (seen);
}

static gboolean
on_member_seen_timeout (gpointer user_data)
{
  MemberSeen *seen = user_data;

  if (revolution_mail_message_get_unread (seen->member))
    mark_read (seen->self, seen->member, TRUE, "gelesen (automatisch)");

  return G_SOURCE_REMOVE;
}

static void
clear_member_seen (RevolutionMailView *self)
{
  for (guint i = 0; i < self->member_seen->len; i++)
    g_source_destroy (self->member_seen->pdata[i]);
  g_ptr_array_set_size (self->member_seen, 0);
}

static void
on_conversation_shown (RevolutionMailView    *self,
                       RevolutionMailMessage *member)
{
  MemberSeen *seen;
  GSource *source;
  int timeout;

  if (!revolution_mail_message_get_unread (member) || !g_settings_get_boolean (self->settings, "mark-seen"))
    return;

  timeout = MAX (g_settings_get_int (self->settings, "mark-seen-timeout"), 0);
  g_debug ("Gelesen: %s %s in %d ms", self->loading, revolution_mail_message_get_uid (member), timeout);
  seen = g_new0 (MemberSeen, 1);
  seen->self = self;
  seen->member = g_object_ref (member);
  source = g_timeout_source_new (timeout);
  g_source_set_callback (source, on_member_seen_timeout, seen, (GDestroyNotify) member_seen_free);
  g_source_attach (source, NULL);
  g_ptr_array_add (self->member_seen, source);
}

/* #357: Markieren und Gelesen an einer Nachricht der Konversation (mail-conversation.c). */
static void
on_conversation_mark (RevolutionMailView    *self,
                      RevolutionMailMessage *member,
                      guint                  mask,
                      guint                  set)
{
  const char *what = (mask & CAMEL_MESSAGE_FLAGGED) != 0 ? (set != 0 ? "markiert" : "nicht markiert")
                                                         : (set != 0 ? "gelesen" : "ungelesen");

  /* Ungelesen beendet den Gelesen-Timer wie in der Liste (mark_message, Evolution e-mail-reader.c:1187-1190). */
  if ((mask & CAMEL_MESSAGE_SEEN) != 0 && (set & CAMEL_MESSAGE_SEEN) == 0)
    clear_member_seen (self);
  mark_message (self, member, mask, set, what);
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
  revolution_mail_view_show_error (self, _("Aktualisieren fehlgeschlagen"), detail, FALSE);
}

static void on_folder_selected (RevolutionMailView *self);

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

/* #353: Kontextmenü der Zeile wie GNOME Dateien (Nautilus): Rechtsklick wählt die Zeile und
 * öffnet ein GtkPopoverMenu ohne Pfeil an der Klickstelle, Kürzel rechts (row_menu in
 * mail-view.blp). Rechtsklick verwirft den Gelesen-Timer wie Evolution (e-mail-reader.c:5733-5735).
 * Tastatur: Menü-Taste bzw. Umschalt+F10 (list.context-menu) an der Zeile mit dem Fokus. */
static void
show_row_menu (RevolutionMailView *self,
               GtkWidget          *widget,
               double              x,
               double              y)
{
  graphene_point_t point;

  if (!gtk_widget_compute_point (widget, GTK_WIDGET (self->message_list), &GRAPHENE_POINT_INIT (x, y), &point))
    return;

  g_clear_handle_id (&self->seen_id, g_source_remove);
  gtk_popover_set_pointing_to (GTK_POPOVER (self->row_popover),
                               &(GdkRectangle) { (int) point.x, (int) point.y, 1, 1 });
  gtk_popover_popup (GTK_POPOVER (self->row_popover));
}

static RevolutionMailView *
view_for (GtkEventController *controller)
{
  return REVOLUTION_MAIL_VIEW (gtk_widget_get_ancestor (gtk_event_controller_get_widget (controller),
                                                        REVOLUTION_TYPE_MAIL_VIEW));
}

static void
on_row_secondary_click (GtkListItem     *item,
                        int              n_press G_GNUC_UNUSED,
                        double           x,
                        double           y,
                        GtkGestureClick *gesture)
{
  RevolutionMailView *self = view_for (GTK_EVENT_CONTROLLER (gesture));

  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  gtk_single_selection_set_selected (self->message_selection, gtk_list_item_get_position (item));
  show_row_menu (self, gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture)), x, y);
}

static void
on_context_menu (GSimpleAction *action G_GNUC_UNUSED,
                 GVariant      *parameter G_GNUC_UNUSED,
                 gpointer       user_data)
{
  RevolutionMailView *self = user_data;
  GtkWidget *focus = gtk_root_get_focus (gtk_widget_get_root (GTK_WIDGET (self)));
  graphene_rect_t bounds;

  if (selected_message (self) == NULL || focus == NULL ||
      !gtk_widget_is_ancestor (focus, GTK_WIDGET (self->message_list)) ||
      !gtk_widget_compute_bounds (focus, focus, &bounds))
    return;

  show_row_menu (self, focus, bounds.size.width / 2, bounds.size.height / 2);
}

/* #353: Wischen über eine Zeile wie Apple Mail: nach rechts Gelesen/Ungelesen, nach links
 * Markieren/Markierung entfernen, für diese Zeile ohne Auswahl. GtkGestureSwipe meldet am Ende
 * die Geschwindigkeit (Touch, Touchpad-Ziehen und Maus); gilt nur waagerecht und ab
 * SWIPE_VELOCITY, damit ein Klick oder Ziehen nach unten nichts auslöst. */
#define SWIPE_VELOCITY 600.0

static void
on_row_swipe (GtkListItem     *item,
              double           velocity_x,
              double           velocity_y,
              GtkGestureSwipe *gesture)
{
  RevolutionMailView *self = view_for (GTK_EVENT_CONTROLLER (gesture));
  RevolutionMailMessage *message = gtk_list_item_get_item (item);

  if (message == NULL || ABS (velocity_x) < SWIPE_VELOCITY || ABS (velocity_x) < 2 * ABS (velocity_y))
    return;

  if (velocity_x > 0)
    mark_read (self, message, revolution_mail_message_get_unread (message),
               revolution_mail_message_get_unread (message) ? "gelesen (Wischen)" : "ungelesen (Wischen)");
  else
    mark_starred (self, message, !revolution_mail_message_get_starred (message),
                  revolution_mail_message_get_starred (message) ? "nicht markiert (Wischen)" : "markiert (Wischen)");
}

static void
folder_ready_free (FolderReady *ready)
{
  g_object_unref (ready->self);
  g_object_unref (ready->folder);
  g_free (ready);
}

/* Im UI-Thread: ab jetzt Änderungen des Ordners live in die Liste (on_folder_changed), Flags
 * und Aktualisieren möglich. Der Server-Durchgang danach gleicht ohnehin ab; Änderungen, die
 * dabei auch über „changed“ kommen, wirken am bestehenden Eintrag und doppeln nichts, solange
 * kein Durchgang Blöcke schickt (#410, blocks_running). */
static gboolean
use_folder (gpointer data)
{
  FolderReady *ready = data;
  RevolutionMailView *self = ready->self;

  if (ready->generation != self->load_generation || self->messages == NULL)
    return G_SOURCE_REMOVE;

  g_set_object (&self->camel_folder, ready->folder);
  g_signal_connect_object (ready->folder, "changed", G_CALLBACK (on_folder_changed), self, G_CONNECT_DEFAULT);
  update_message_actions (self);

  return G_SOURCE_REMOVE;
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
  gboolean ok = g_task_propagate_boolean (G_TASK (result), &error);

  /* #414: Kein Durchgang mehr, auf keinem Weg (auch Fehler beim Holen des Ordners, Abbruch des
   * Abgleichs): zurückgestellte Live-Änderungen jetzt anwenden. */
  if (load->generation == self->load_generation)
    {
      self->blocks_running = FALSE;
      start_folder_changes (self);
    }

  /* #408: still nur, wenn wir abgebrochen haben (anderer Ordner, Beenden); einen Abbruch von innen
   * (Camel/EDS) wie jeden Fehler melden. */
  if (!ok && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
      g_cancellable_is_cancelled (g_task_get_cancellable (G_TASK (result))))
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

  g_debug ("Nachrichten: %s vollständig %u nach %.0f ms, %u verschiedene UIDs", self->loading, n_items,
           (g_get_monotonic_time () - self->load_started) / 1000.0, distinct_uids (self));
}

/* #357: Ordner eines Typs (CAMEL_FOLDER_TYPE_*) im Ordnerbaum eines Kontos, neue Referenz oder NULL. */
static RevolutionMailFolder *
find_folder_type (GListModel *model,
                  guint32     type)
{
  for (guint i = 0; model != NULL && i < g_list_model_get_n_items (model); i++)
    {
      g_autoptr (RevolutionMailFolder) folder = g_list_model_get_item (model, i);
      guint32 flags = revolution_mail_folder_get_flags (folder);
      RevolutionMailFolder *found;

      if ((flags & CAMEL_FOLDER_TYPE_MASK) == type && (flags & CAMEL_FOLDER_NOSELECT) == 0)
        return g_steal_pointer (&folder);

      found = find_folder_type (revolution_mail_folder_get_children (folder), type);
      if (found != NULL)
        return found;
    }

  return NULL;
}

/* #357: Gesendet-Ordner, den die Konversation dieses Ordners mischt (Dokument #10 „Eingang und Gesendet
 * gemischt“), oder NULL: nicht für Favoriten (Suchordner über alle Konten), nicht für Gesendet, Entwürfe,
 * Postausgang, Papierkorb und Spam selbst, und nicht ohne erkannten Gesendet-Ordner (Gmail, #350). */
static RevolutionMailFolder *
conversation_sent_folder (RevolutionMailView    *self,
                          RevolutionMailAccount *account,
                          RevolutionMailFolder  *folder)
{
  guint32 type = revolution_mail_folder_get_flags (folder) & CAMEL_FOLDER_TYPE_MASK;
  RevolutionMailFolder *sent;

  if (account == self->favorites || type == CAMEL_FOLDER_TYPE_SENT || type == CAMEL_FOLDER_TYPE_DRAFTS ||
      type == CAMEL_FOLDER_TYPE_OUTBOX || type == CAMEL_FOLDER_TYPE_TRASH || type == CAMEL_FOLDER_TYPE_JUNK)
    return NULL;

  sent = find_folder_type (gtk_tree_list_model_get_model (account_tree (account)), CAMEL_FOLDER_TYPE_SENT);
  if (sent == NULL)
    g_debug ("Konversationen: %s %s ohne Gesendet-Ordner", revolution_mail_account_get_uid (account),
             revolution_mail_folder_get_full_name (folder));

  return sent;
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
  self->blocks_running = FALSE;
  /* #353: Änderungen des bisherigen Ordners nicht mehr anwenden. */
  if (self->camel_folder != NULL)
    g_signal_handlers_disconnect_by_func (self->camel_folder, on_folder_changed, self);
  g_clear_object (&self->camel_folder);
  g_hash_table_remove_all (self->pending_changed);
  g_hash_table_remove_all (self->pending_removed);
  g_list_store_remove_all (self->messages);
  update_message_actions (self);
  gtk_widget_set_visible (GTK_WIDGET (self->list_subtitle), FALSE);

  if (folder != NULL)
    g_object_get (folder, "account", &account, "display-name", &display_name, NULL);

  /* #350: Wahl merken wie Evolution den zuletzt gewählten Ordner (e-mail-shell-view.c, Zustand
   * „SelectedFolder“); nicht bei REVOLUTION_FOLDER (Test-Hook wie REVOLUTION_THEME, main.c). */
  if (folder != NULL)
    self->initial_done = TRUE;
  if (account != NULL && g_getenv ("REVOLUTION_FOLDER") == NULL)
    {
      g_autofree char *key = g_strdup_printf ("%s/%s", revolution_mail_account_get_uid (account),
                                              revolution_mail_folder_get_full_name (folder));

      g_settings_set_string (self->settings, "selected-folder", key);
    }

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

  load = g_atomic_rc_box_new0 (LoadMessages);
  g_mutex_init (&load->lock);
  g_cond_init (&load->cond);
  load->session = g_object_ref (self->session);
  load->uid = g_strdup (revolution_mail_account_get_uid (account));
  load->full_name = g_strdup (revolution_mail_folder_get_full_name (folder));
  load->generation = self->load_generation;
  load->refresh_cancellable = self->cancellable != NULL ? g_object_ref (self->cancellable) : NULL;

  /* #357: Konversationen; mit Gesendet lädt der Lesebereich ihre Nachrichten aus dem Suchordner. */
  self->conversations = g_settings_get_boolean (self->settings, "conversations");
  load->conversations = self->conversations;
  g_clear_object (&self->thread_folder);
  if (self->conversations)
    {
      g_autoptr (RevolutionMailFolder) sent = conversation_sent_folder (self, account, folder);

      if (sent != NULL)
        {
          g_autofree char *name = revolution_mail_thread_folder_name (load->uid, load->full_name);

          load->sent_full_name = g_strdup (revolution_mail_folder_get_full_name (sent));
          self->thread_folder = revolution_mail_folder_new (name, display_name, NULL, self->favorites);
        }
    }

  g_free (self->loading);
  self->loading = g_strdup_printf ("%s %s", load->uid, load->full_name);
  self->load_started = g_get_monotonic_time ();
  g_debug ("Nachrichten: %s lädt", self->loading);

  /* #414: Live-Änderungen bis nach dem Cache-Durchgang zurückstellen (add_messages_block). */
  self->blocks_running = TRUE;
  self->load_cancellable = g_cancellable_new ();
  task = g_task_new (self, self->load_cancellable, on_messages_loaded, NULL);
  g_task_set_source_tag (task, on_folder_selected);
  g_task_set_priority (task, G_PRIORITY_DEFAULT_IDLE);
  g_task_set_task_data (task, load, (GDestroyNotify) load_messages_release);
  g_task_run_in_thread (task, load_messages_thread);
}

/* #354: Auswahl in der Liste zeigt die Nachricht im Lesebereich (mail-reader.c), ohne
 * Auswahl den Leerzustand. Ein Abgleich, der den Eintrag behält, löst nichts aus. */
static void
on_message_selected (RevolutionMailView *self)
{
  RevolutionMailMessage *message = gtk_single_selection_get_selected_item (self->message_selection);
  g_autoptr (RevolutionMailFolder) place = NULL;
  GListModel *members;

  if (message == NULL || self->folder == NULL || self->session == NULL)
    {
      revolution_mail_reader_clear (self->reader);
      revolution_mail_conversation_clear (self->conversation_view);
      gtk_stack_set_visible_child_name (self->message_stack, "empty");
      return;
    }

  /* #357: Konversation: alle Nachrichten untereinander (mail-conversation.c), geladen aus dem Ordner der
   * Liste bzw. dem Suchordner aus Ordner und Gesendet. */
  members = revolution_mail_message_get_members (message);
  if (members != NULL && g_list_model_get_n_items (members) > 1)
    {
      revolution_mail_reader_clear (self->reader);
      gtk_stack_set_visible_child_name (self->message_stack, "conversation");
      revolution_mail_conversation_show (self->conversation_view, self->session,
                                         self->thread_folder != NULL ? self->thread_folder : self->folder, message);
      return;
    }
  revolution_mail_conversation_clear (self->conversation_view);

  /* #357: Konversation aus einer Nachricht: wie eine einzelne Nachricht anzeigen (UID der Nachricht, aus
   * dem Suchordner der Konversation, falls gemischt). */
  if (members != NULL)
    {
      g_autoptr (RevolutionMailMessage) member = g_list_model_get_item (members, 0);

      place = revolution_mail_view_find_place (self, member);
      gtk_stack_set_visible_child_name (self->message_stack, "content");
      revolution_mail_reader_show (self->reader, self->session, self->thread_folder != NULL ? self->thread_folder : self->folder,
                                   place, member);
      return;
    }

  /* #351: In den Favoriten zeigt der Kopf den echten Ordner (Prototyp „Eingang — Privat“). */
  place = revolution_mail_view_find_place (self, message);
  gtk_stack_set_visible_child_name (self->message_stack, "content");
  revolution_mail_reader_show (self->reader, self->session, self->folder, place, message);
}

/* #351/#357: Ort einer Nachricht für den Kopf: der echte Ordner (Nachricht aus einem Suchordner, Favoriten
 * oder Konversation mit Gesendet), sonst der gewählte Ordner. Neue Referenz, NULL ohne Ordner. */
RevolutionMailFolder *
revolution_mail_view_find_place (RevolutionMailView    *self,
                                 RevolutionMailMessage *message)
{
  g_autoptr (RevolutionMailAccount) account = NULL;
  const char *source_uid, *source_folder;

  g_return_val_if_fail (REVOLUTION_IS_MAIL_VIEW (self), NULL);
  g_return_val_if_fail (REVOLUTION_IS_MAIL_MESSAGE (message), NULL);

  if (revolution_mail_message_get_source (message, &source_uid, &source_folder) &&
      (account = find_account (self, source_uid)) != NULL)
    {
      RevolutionMailFolder *place = revolution_mail_folder_find (gtk_tree_list_model_get_model (account_tree (account)),
                                                                 source_folder);

      if (place != NULL)
        return place;
    }

  return self->folder != NULL ? g_object_ref (self->folder) : NULL;
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
  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_VIEW);
  g_clear_object (&self->stack);
  g_clear_object (&self->registry);
  g_clear_object (&self->accounts);
  g_clear_object (&self->favorites);
  g_clear_object (&self->messages);
  g_clear_object (&self->folder);
  g_clear_object (&self->session);
  g_clear_pointer (&self->loading, g_free);

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
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, split);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, row_menu);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, message_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, reader);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, conversation_view);
  gtk_widget_class_bind_template_callback (widget_class, on_search_toggled);
  gtk_widget_class_bind_template_callback (widget_class, on_row_secondary_click);
  gtk_widget_class_bind_template_callback (widget_class, on_row_swipe);
  gtk_widget_class_bind_template_callback (widget_class, on_search_started);
  gtk_widget_class_bind_template_callback (widget_class, on_search_stop);
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
  g_autoptr (GPropertyAction) sidebar_action = NULL;
  AdwStyleManager *style_manager = adw_style_manager_get_default ();

  gtk_widget_init_template (GTK_WIDGET (self));

  /* CR #397: Ordnerbaum als GtkTreeListModel je Konto (#348), Konten als Abschnitte (#345). */
  self->accounts = g_list_store_new (REVOLUTION_TYPE_MAIL_ACCOUNT);
  /* #351: Favoriten als erster Abschnitt vor den Konten (Prototyp), außerhalb von self->accounts
   * (sync_accounts gleicht nur die Konten der Registry ab); Ordner sind gleich da. */
  self->favorites = revolution_mail_account_new (REVOLUTION_MAIL_FAVORITES_UID, _("Favoriten"));
  revolution_mail_favorites_fill (G_LIST_STORE (gtk_tree_list_model_get_model (account_tree (self->favorites))),
                                  self->favorites);
  g_object_set_data (G_OBJECT (self->favorites), "folders-noted", GINT_TO_POINTER (TRUE));
  g_list_store_append (favorites, self->favorites);
  g_list_store_append (sections, favorites);
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
    /* Translators: Titel des Fehlerbanners, z. B. „Keine Verbindung (7 Konten)“:
     * Fehlerart und Zahl der Konten mit dieser Fehlerart. */
    shown = g_strdup_printf (ngettext ("%s (%u Konto)", "%s (%u Konten)", best_count),
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
    if (g_strcmp0 (((BannerError *) g_ptr_array_index (errors, i))->title, _("Keine Verbindung")) != 0)
      return;

  g_object_set_data (G_OBJECT (self), "banner-errors", NULL);
  update_banner (self, _("Keine Verbindung"));
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
static void
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

static GListModel *
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
  const char *wanted = g_getenv ("REVOLUTION_FOLDER");

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
