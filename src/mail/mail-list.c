/* mail-list.c – Nachrichtenliste der Mail-Ansicht: Laden aus Cache und Server, Live-Änderungen, Sortier-/
 * UID-Log, Konversation/Gesendet-Ordner, Auswahl → Lesebereich (#352, #353, #350, #357, #410, #414, #418, #419).
 * Aus mail-view.c (#444); gemeinsamer Zustand in mail-view-private.h.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <string.h>
#include <glib/gi18n.h>

#include "eds-compat.h"
#include "mail-favorites.h"
#include "mail-search.h"
#include "mail-thread.h"
#include "mail-view-private.h"

/* #352: Nachrichten des gewählten Ordners. Ablauf wie Evolution beim Wählen eines Ordners:
 * Liste aus der Summary (message_list_regen_thread, message-list.c:6200-6380:
 * camel_folder_get_uids, camel_folder_summary_prepare_fetch_all, camel_folder_get_message_info)
 * und Abgleich mit dem Server (e_mail_reader_refresh_folder, e-mail-paned-view.c:690-693).
 * Hier beides in einem Worker-GTask (Architektur 1): erst der Stand im Cache, nach
 * camel_folder_refresh_info_sync der Stand des Servers, dieser nur, wenn er sich vom Cache
 * unterscheidet. Die Einträge entstehen im Worker und kommen in Blöcken in den UI-Thread
 * (g_main_context_invoke_full), dort nur g_list_store_splice (docs/ENTWICKLUNG.md „Datenmodell“). Ein
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
  /* #365: Suche (update_search), Fehler der Suche im aktuellen Ordner (send_messages). */
  char         *search;
  gboolean      search_all;
  GError       *error;
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
  g_free (load->search);
  g_clear_error (&load->error);
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
    count = g_strdup_printf (ngettext ("%'u conversation", "%'u conversations", total), total);
  else
    count = g_strdup_printf (ngettext ("%'u message", "%'u messages", total), total);
  if (unread > 0)
    /* Translators: “10 messages, 4 unread”; %s is the number of messages. */
    subtitle = g_strdup_printf (_("%s, %'u unread"), count, unread);

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

/* #366: Die Nachricht aus der Benachrichtigung wählen, sobald ihr Ordner die Liste ist und ihr Block darin
 * steht; TRUE, wenn gewählt. In einer Konversation die Zeile, die sie enthält. Konversationen aus Ordner
 * und Gesendet sind ein Suchordner: UID = 8 Zeichen Kennung des echten Ordners + UID dort (EDS
 * camel-vee-data-cache.c:82-114), der echte Ordner steht in der Quelle der Nachricht. */
gboolean
select_shown (RevolutionMailView *self)
{
  GListModel *model = G_LIST_MODEL (self->messages);
  g_autoptr (RevolutionMailAccount) account = NULL;
  g_autofree char *place = NULL;

  if (self->show_uid == NULL || self->folder == NULL)
    return FALSE;
  g_object_get (self->folder, "account", &account, NULL);
  place = g_strdup_printf ("%s/%s", account != NULL ? revolution_mail_account_get_uid (account) : "",
                           revolution_mail_folder_get_full_name (self->folder));
  if (g_strcmp0 (place, self->show_folder) != 0)
    return FALSE;

  for (guint i = 0; i < g_list_model_get_n_items (model); i++)
    {
      g_autoptr (RevolutionMailMessage) row = g_list_model_get_item (model, i);
      GListModel *members = revolution_mail_message_get_members (row);
      guint n = members != NULL ? g_list_model_get_n_items (members) : 1;

      for (guint j = 0; j < n; j++)
        {
          g_autoptr (RevolutionMailMessage) message = members != NULL ? g_list_model_get_item (members, j) : g_object_ref (row);
          const char *uid = revolution_mail_message_get_uid (message), *source_uid, *source_folder;
          g_autofree char *source = NULL;
          gboolean found;

          if (revolution_mail_message_get_source (message, &source_uid, &source_folder))
            {
              source = g_strdup_printf ("%s/%s", source_uid, source_folder);
              found = g_strcmp0 (source, place) == 0 && strlen (uid) == 8 + strlen (self->show_uid) &&
                      strcmp (uid + 8, self->show_uid) == 0;
            }
          else
            found = g_strcmp0 (uid, self->show_uid) == 0;

          if (found)
            {
              g_debug ("Benachrichtigung: Nachricht %s gewählt (Zeile %s)", self->show_uid, revolution_mail_message_get_uid (row));
              g_clear_pointer (&self->show_folder, g_free);
              g_clear_pointer (&self->show_uid, g_free);
              gtk_single_selection_set_selected (self->message_selection, i);
              return TRUE;
            }
        }
    }

  return FALSE;
}

/* #354: Nur für Testläufe (Screenshots des Lesebereichs) wie REVOLUTION_FOLDER: die
 * Umgebungsvariable REVOLUTION_MESSAGE=<Teil des Betreffs> wählt die erste passende Nachricht,
 * sobald sie in der Liste steht und noch nichts gewählt ist. */
static void
select_message_from_env (RevolutionMailView *self)
{
  const char *wanted = g_getenv ("REVOLUTION_MESSAGE");
  GListModel *model = G_LIST_MODEL (self->messages);

  if (select_shown (self) || gtk_single_selection_get_selected_item (self->message_selection) != NULL)
    return;

  /* #365: nach dem Ende der Suche die vorher gewählte Nachricht, sobald ihr Block in der Liste steht. */
  for (guint i = 0; self->returning && i < g_list_model_get_n_items (model); i++)
    {
      g_autoptr (RevolutionMailMessage) message = g_list_model_get_item (model, i);

      if (g_strcmp0 (revolution_mail_message_get_uid (message), self->return_uid) == 0)
        {
          g_debug ("Suche: beendet, Auswahl %s wieder gewählt", self->return_uid);
          self->returning = FALSE;
          gtk_single_selection_set_selected (self->message_selection, i);
          return;
        }
    }

  if (wanted == NULL)
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
  /* #365: Suche im aktuellen Ordner wie Evolution (message-list.c:6296-6300); „Alle Ordner“ hat sie schon der
   * Suchordner gemacht (mail-search.c). */
  if (load->search != NULL && !load->search_all)
    {
      g_autoptr (GCancellable) op = camel_operation_new_proxy (cancellable);

      uids = camel_folder_search_by_expression (folder, load->search, op, &load->error);
      if (uids == NULL && load->error == NULL)
        g_set_error_literal (&load->error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID, _("Search Failed"));
      if (uids == NULL)
        return FALSE;
    }
  else
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
  if (load->search != NULL && !load->search_all)
    camel_folder_search_free (folder, uids);
  else
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
                               _("Account %s unavailable"), load->uid);
      return;
    }

  /* Ordner immer über camel_store_get_folder_sync (docs/ENTWICKLUNG.md „Camel-Ordner“). */
  folder = camel_store_get_folder_sync (CAMEL_STORE (service), load->full_name, 0, load_op, &error);
  if (folder == NULL)
    {
      g_task_return_error (task, error);
      return;
    }

  /* #357: Konversation aus Ordner und Gesendet: Liste, Flags und Live-Änderungen über den Suchordner
   * (mail-thread.c); abgeglichen wird weiter der Ordner selbst. #408: eigenes Cancellable je Vorgang. */
  /* #365: „Alle Ordner“ aus dem Suchordner (mail-search.c), sonst ohne Quellen wie Evolution beim Ende der Suche. */
  if (!load->search_all)
    revolution_mail_search_clear_sync (load->session);
  list = load->search_all ? revolution_mail_search_folder_sync (load->session, load->search, load_op, &error)
       : load->sent_full_name != NULL ? revolution_mail_thread_folder_sync (folder, load->sent_full_name, &sent,
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
  /* #365: Suche ohne Abgleich, ein Durchgang wie Evolution (mail_regen_list sucht nur, message-list.c:6296-6316). */
  if (load->search != NULL)
    {
      if (send_messages (task, list, "Suche", &signature))
        g_task_return_boolean (task, TRUE);
      else if (load->error != NULL)
        g_task_return_error (task, g_steal_pointer (&load->error));
      else
        g_task_return_error_if_cancelled (task);
      return;
    }
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
  /* #365: Suche im aktuellen Ordner: neu suchen, die gewählte Nachricht (keep) bleibt. */
  gboolean            conversations;
  char               *search;
  char               *keep;
} FolderChanges;

static void
folder_changes_free (FolderChanges *changes)
{
  g_free (changes->search);
  g_free (changes->keep);
  g_object_unref (changes->self);
  g_object_unref (changes->folder);
  g_ptr_array_unref (changes->changed);
  g_ptr_array_unref (changes->removed);
  g_clear_pointer (&changes->items, g_ptr_array_unref);
  g_free (changes);
}

void
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
      gboolean kept = changes->keep == NULL;

      camel_folder_summary_prepare_fetch_all (camel_folder_get_folder_summary (changes->folder), NULL);
      /* #365: Suche im aktuellen Ordner: neu suchen wie Evolution (message_list_folder_changed → mail_regen_list,
       * message-list.c:5178-5186, 6296-6300); die gewählte Nachricht bleibt, auch wenn sie nicht mehr passt
       * (message_list_regen_tweak_search_results, message-list.c:6138-6197), sonst verschwände eine eben
       * gelesene unter „Ungelesen“. Scheitert die Suche, bleibt die Liste (verworfen wie eine Änderung zur alten
       * Liste, on_folder_changes_done). */
      uids = changes->search != NULL ? camel_folder_search_by_expression (changes->folder, changes->search, NULL, NULL)
                                     : camel_folder_get_uids (changes->folder);
      if (uids == NULL)
        {
          g_debug ("Suche: Änderung nicht angewendet, Suche gescheitert");
          changes->generation = G_MAXUINT;
          g_task_return_boolean (task, TRUE);
          return;
        }
      for (guint i = 0; i < uids->len; i++)
        {
          CamelMessageInfo *info = camel_folder_get_message_info (changes->folder, uids->pdata[i]);

          kept |= g_strcmp0 (uids->pdata[i], changes->keep) == 0;
          if (info != NULL && (camel_message_info_get_flags (info) & hidden) == 0)
            g_ptr_array_add (infos, info);
          else
            g_clear_object (&info);
        }
      if (!kept)
        {
          CamelMessageInfo *info = camel_folder_get_message_info (changes->folder, changes->keep);

          if (info != NULL && (camel_message_info_get_flags (info) & hidden) == 0)
            g_ptr_array_add (infos, info);
          else
            g_clear_object (&info);
        }
      if (changes->search != NULL)
        camel_folder_search_free (changes->folder, uids);
      else
        camel_folder_free_uids (changes->folder, uids);
      if (changes->conversations)
        {
          changes->items = revolution_mail_thread_build (changes->folder, infos, now);
          g_task_return_boolean (task, TRUE);
          return;
        }
    }

  for (guint i = 0; !changes->full && i < changes->changed->len; i++)
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

void
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
  changes->conversations = self->conversations;
  /* #365: „Alle Ordner“ prüft der Suchordner selbst (camel-vee-folder.c:367-540). */
  if (self->search != NULL && !self->search_all)
    {
      RevolutionMailMessage *selected = gtk_single_selection_get_selected_item (self->message_selection);

      changes->search = g_strdup (self->search);
      changes->keep = selected != NULL ? g_strdup (revolution_mail_message_get_uid (selected)) : NULL;
    }
  changes->full = self->conversations || changes->search != NULL;
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

  /* #365: „Alle Ordner“ fertig (auch nach Fehler): Leerzustand ohne „Suche läuft …“. */
  set_empty_page (self, FALSE);
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
      revolution_mail_view_show_error (self, _("Messages Not Loaded"), detail, FALSE);
    }

  g_debug ("Nachrichten: %s vollständig %u nach %.0f ms, %u verschiedene UIDs", self->loading, n_items,
           (g_get_monotonic_time () - self->load_started) / 1000.0, distinct_uids (self));
}

/* #357: Ordner eines Typs (CAMEL_FOLDER_TYPE_*) im Ordnerbaum eines Kontos, neue Referenz oder NULL. */
RevolutionMailFolder *
find_folder_type (GListModel *model,
                  guint32     type)
{
  for (guint i = 0; model != NULL && i < g_list_model_get_n_items (model); i++)
    {
      g_autoptr (RevolutionMailFolder) folder = g_list_model_get_item (model, i);
      guint32 flags = revolution_mail_folder_get_flags (folder);
      RevolutionMailFolder *found;

      /* #363: ohne die virtuellen Ordner von imapx (.#evolution/Trash, /Junk). */
      if ((flags & CAMEL_FOLDER_TYPE_MASK) == type && (flags & (CAMEL_FOLDER_NOSELECT | CAMEL_FOLDER_VIRTUAL)) == 0)
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
void
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
  update_folder_actions (self);
  /* #365: anderer Ordner, die vor der Suche gewählte Nachricht gilt nicht mehr (update_search). */
  g_clear_pointer (&self->return_uid, g_free);
  self->returning = FALSE;

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

  /* #365: Suche (update_search); „Alle Ordner“ zeigt der Lesebereich aus dem Suchordner (mail-search.c). */
  load->search = g_strdup (self->search);
  load->search_all = self->search_all;
  if (self->search_all)
    {
      g_clear_pointer (&load->sent_full_name, g_free);
      g_clear_object (&self->thread_folder);
      self->thread_folder = revolution_mail_folder_new (REVOLUTION_MAIL_SEARCH_NAME, _("All Folders"), NULL,
                                                        self->favorites);
      gtk_label_set_label (self->list_title, _("All Folders"));
      set_empty_page (self, TRUE);
      gtk_stack_set_visible_child_name (self->list_stack, "empty");
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
void
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
  /* #365: flach aus dem Suchordner „Alle Ordner“ (thread_folder, on_folder_selected). */
  revolution_mail_reader_show (self->reader, self->session, self->thread_folder != NULL ? self->thread_folder : self->folder,
                               place, message);
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
