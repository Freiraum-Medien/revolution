/* contacts-view.c – Kontakte-Ansicht (Prototyp #338)
 *
 * Vorlage: quelltext/stamp/src/views/contact/stamp-contact-view.c (Template-
 * Klasse mit Eigenschaft „stack“ für den Ansichtsumschalter), Muster wie
 * mail-view.c. Oberfläche in contacts-view.blp. Aktionen ohne Funktion sind nicht
 * registriert, ihre Knöpfe und Menüeinträge sind daher ausgegraut (#342).
 * #402: Suche, Seitenleiste und Klasse .dark übernommen aus mail-view.c (#399–#401).
 *
 * #368: Adressbücher aus der ESourceRegistry, je Konto ein Abschnitt (contacts-book.c); die Liste zeigt
 * das gewählte Buch live über eine EBookClientView wie Evolution (e-addressbook-model.c:148-300, 347-405:
 * objects-added/-modified/-removed, complete), nicht einmalig wie Stamp (stamp-contact-list.c:120-152,
 * e_book_client_get_contacts). Suche: neue Ansicht mit EBookQuery (open_view). Jeder EDS-Aufruf
 * (Verbinden, Ansicht anlegen, starten) im Worker-GTask (Architektur 1), Begründung für den
 * UI-Thread der Signale bei start_thread.
 * #370: Anlegen, Bearbeiten, Löschen (Abschnitt unten): Formular contacts-editor.c im Detailbereich wie
 * GNOME Contacts, Schreiben per e_book_client_add/modify/remove_contact_sync im Worker-GTask; die Änderung
 * kommt danach über die Live-Ansicht ins Modell (kein zweites Einfügen). Löschen ohne Dialog mit
 * Rückgängig-Toast wie GNOME Contacts (Ausführung nach Ablauf, beim Beenden sofort).
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <string.h>

#include <glib/gi18n.h>
#include <libebook/libebook.h>

#include "contacts-book.h"
#include "contacts-contact.h"
#include "contacts-detail.h"
#include "contacts-editor.h"
#include "contacts-view.h"

/* #370: vorgemerktes Löschen (Rückgängig bis zum Ablauf). */
typedef struct
{
  RevolutionContactsBook    *book;
  EBookClient               *client;
  RevolutionContactsContact *item;
  AdwToast                  *toast;
  guint                      timeout_id;
} PendingDelete;

struct _RevolutionContactsView
{
  AdwBreakpointBin        parent_instance;
  AdwViewStack           *stack;
  GtkSingleSelection     *book_selection;
  GtkSortListModel       *book_sorted;
  GtkSingleSelection     *contact_selection;
  GtkSortListModel       *contact_sorted;
  GtkListView            *contact_list;
  GtkListView            *book_list;
  GtkStack               *contact_stack;
  GtkLabel               *title_label;
  GtkLabel               *subtitle_label;
  AdwBanner              *banner;
  RevolutionContactsDetail *detail;
  GtkStack               *header_stack;
  GtkSearchEntry         *search_entry;
  GtkToggleButton        *search_button;
  AdwOverlaySplitView    *split;
  GtkWidget              *letter_bar;
  GtkStack               *pane_stack;
  RevolutionContactsEditor *editor;
  GtkLabel               *edit_title;
  GtkButton              *done_button;
  GtkWidget              *edit_button;
  GtkWidget              *delete_button;
  GActionMap             *contact_actions;

  ESourceRegistry        *registry;
  GListStore             *books;        /* RevolutionContactsBook */
  GListStore             *contacts;     /* RevolutionContactsContact des gewählten Buchs */
  GHashTable             *by_uid;       /* Kontakt-UID → Eintrag in contacts (geliehen) */
  GHashTable             *clients;      /* Buch-UID → EBookClient (verbunden) */
  GHashTable             *connecting;   /* Buch-UID, Verbinden läuft */
  GCancellable           *cancellable;  /* Verbinden, beim Beenden abgebrochen */
  GCancellable           *view_cancellable;
  RevolutionContactsBook *book;         /* gewählt */
  EBookClientView        *view;         /* Live-Ansicht des gewählten Buchs (Suche: Abfrage) */
  guint                   generation;
  guint                   lists;        /* ausgelassene Kontaktlisten (Gruppen/Verteiler, nicht #368) */
  gint64                  view_started;
  gboolean                complete;
  gboolean                search_hint;  /* Buch ohne „do-initial-query“: Kontakte erst nach Suche */
  gboolean                connect_all;  /* ab der ersten Anzeige alle Bücher verbinden */
  guint                   jump_target;  /* Zeile für contacts.jump nach dem Layout */
  gboolean                syncing;      /* sync_books ersetzt die Liste, Auswahl folgt danach */
  /* #370 */
  gboolean                editing;      /* Formular offen */
  gboolean                saving;       /* Schreiben läuft */
  RevolutionContactsBook *edit_book;    /* Bearbeiten: Buch des Kontakts */
  GPtrArray              *edit_books;   /* Anlegen: wählbare Bücher in der Reihenfolge der Auswahl */
  char                   *select_uid;   /* nach dem Anlegen wählen, sobald die Ansicht ihn meldet */
  PendingDelete          *pending;
};

G_DEFINE_FINAL_TYPE (RevolutionContactsView, revolution_contacts_view, ADW_TYPE_BREAKPOINT_BIN)

static void open_view (RevolutionContactsView *self);
static gboolean on_close_request (RevolutionContactsView *self);
static void update_actions (RevolutionContactsView *self);
static void select_pending (RevolutionContactsView *self);

/* ------------------------------------------------------------------ Zustand und Fehler */

/* Fehler als AdwBanner über der Liste (Architektur 3), Einzelheiten im Tooltip und im Log. */
static void
show_error (RevolutionContactsView *self,
            const char             *title,
            const char             *detail)
{
  g_debug ("Kontakte: Banner: %s: %s", title, detail);
  adw_banner_set_title (self->banner, title);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self->banner), detail);
  adw_banner_set_revealed (self->banner, TRUE);
}

static void
update_state (RevolutionContactsView *self)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->contacts));
  const char *search = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
  g_autofree char *subtitle = g_strdup_printf (ngettext ("%u contact", "%u contacts", n), n);
  const char *page;

  gtk_label_set_label (self->title_label, self->book != NULL
                       ? e_source_get_display_name (revolution_contacts_book_get_source (self->book)) : _("Contacts"));
  gtk_label_set_label (self->subtitle_label, subtitle);
  gtk_widget_set_visible (GTK_WIDGET (self->subtitle_label), self->complete);
  if (self->search_hint)
    page = "search-hint";
  else if (self->view_cancellable != NULL && !self->complete)
    page = "loading";
  else if (n > 0)
    page = "list";
  else if (*search != '\0')
    page = "no-results";
  else
    page = "empty";
  gtk_stack_set_visible_child_name (self->contact_stack, page);
  gtk_widget_set_sensitive (self->letter_bar, n > 0);
}

/* ------------------------------------------------------------------ Live-Ansicht (EBookClientView) */

/* Signale der Ansicht kommen im UI-Thread an: e_book_client_view_start merkt sich den thread-default
 * GMainContext des Aufrufers und hängt jede Meldung als Idle-Quelle dort an (EDS
 * e-book-client-view.c:1382-1385, 270-288). start_thread ruft es im GTask-Worker auf, der keinen eigenen
 * Kontext hat; g_main_context_ref_thread_default liefert dann den globalen Standard-Kontext, den der
 * UI-Thread ausführt (wie die Registry, docs/ENTWICKLUNG.md „Worker“). Die Handler hängen vor dem Start
 * (on_view_created), damit keine Meldung verloren geht. */

/* Beleg im Log, dass die Signale der Ansicht im UI-Thread ankommen (Besitzer des Standard-Kontexts). */
static const char *
ui_thread (void)
{
  return g_main_context_is_owner (g_main_context_default ()) ? "ja" : "nein";
}

static gboolean
is_list (EContact *contact)
{
  return GPOINTER_TO_INT (e_contact_get (contact, E_CONTACT_IS_LIST));
}

static void
add_contacts (RevolutionContactsView *self,
              const GSList           *contacts,
              guint                  *added,
              guint                  *modified)
{
  g_autoptr (GPtrArray) items = g_ptr_array_new_with_free_func (g_object_unref);

  for (const GSList *l = contacts; l != NULL; l = l->next)
    {
      RevolutionContactsContact *item;
      const char *uid = e_contact_get_const (l->data, E_CONTACT_UID);
      guint position;

      if (uid == NULL)
        continue;
      /* #370: zum Löschen vorgemerkt, bis Rückgängig oder Ablauf verborgen. */
      if (self->pending != NULL && g_strcmp0 (uid, revolution_contacts_contact_get_uid (self->pending->item)) == 0)
        continue;
      item = g_hash_table_lookup (self->by_uid, uid);
      if (is_list (l->data))
        {
          /* Gruppen/Verteiler sind nicht Teil von #368; eine Kontaktliste fällt heraus. */
          if (item != NULL && g_list_store_find (self->contacts, item, &position))
            {
              g_hash_table_remove (self->by_uid, uid);
              g_list_store_remove (self->contacts, position);
            }
          self->lists++;
          continue;
        }
      if (item != NULL)
        {
          /* Gleicher Eintrag, neue Daten; Ersetzen an derselben Stelle sortiert ihn neu ein (eigene
           * Referenz, weil splice den Eintrag zuerst entfernt). */
          g_autoptr (RevolutionContactsContact) keep = g_object_ref (item);

          revolution_contacts_contact_set_contact (item, l->data);
          if (g_list_store_find (self->contacts, item, &position))
            g_list_store_splice (self->contacts, position, 1, (gpointer *) &keep, 1);
          (*modified)++;
          continue;
        }
      item = revolution_contacts_contact_new (l->data);
      g_hash_table_insert (self->by_uid, g_strdup (uid), item);
      g_ptr_array_add (items, item);
    }
  g_list_store_splice (self->contacts, g_list_model_get_n_items (G_LIST_MODEL (self->contacts)), 0,
                       items->pdata, items->len);
  *added += items->len;
}

static const char *
book_uid (RevolutionContactsView *self)
{
  return e_source_get_uid (revolution_contacts_book_get_source (self->book));
}

static void
on_objects_added (EBookClientView        *view,
                  const GSList           *contacts,
                  RevolutionContactsView *self)
{
  guint added = 0, modified = 0;

  if (view != self->view)
    return;
  add_contacts (self, contacts, &added, &modified);
  /* Nach dem ersten Laden je Kontakt eine Zeile (Live-Änderung von außen, tests/kontakte.sh). */
  for (const GSList *l = contacts; self->complete && l != NULL; l = l->next)
    g_debug ("Kontakte: %s neu %s „%s“", book_uid (self), (const char *) e_contact_get_const (l->data, E_CONTACT_UID),
             (const char *) e_contact_get_const (l->data, E_CONTACT_FULL_NAME));
  g_debug ("Kontakte: %s neu %u, geändert %u, Liste %u (UI-Thread %s)", book_uid (self), added, modified,
           g_list_model_get_n_items (G_LIST_MODEL (self->contacts)), ui_thread ());
  update_state (self);
  select_pending (self);
}

static void
on_objects_modified (EBookClientView        *view,
                     const GSList           *contacts,
                     RevolutionContactsView *self)
{
  RevolutionContactsContact *selected;
  guint added = 0, modified = 0;

  if (view != self->view)
    return;
  add_contacts (self, contacts, &added, &modified);
  for (const GSList *l = contacts; l != NULL; l = l->next)
    g_debug ("Kontakte: %s geändert %s „%s“", book_uid (self), (const char *) e_contact_get_const (l->data, E_CONTACT_UID),
             (const char *) e_contact_get_const (l->data, E_CONTACT_FULL_NAME));
  /* #369: neue Daten des gewählten Kontakts ins Detail (derselbe Eintrag, revolution_contacts_contact_set_contact). */
  selected = gtk_single_selection_get_selected_item (self->contact_selection);
  for (const GSList *l = contacts; selected != NULL && l != NULL; l = l->next)
    if (g_strcmp0 (e_contact_get_const (l->data, E_CONTACT_UID), revolution_contacts_contact_get_uid (selected)) == 0)
      revolution_contacts_detail_set_contact (self->detail, selected);
  g_debug ("Kontakte: %s neu %u, geändert %u, Liste %u (UI-Thread %s)", book_uid (self), added, modified,
           g_list_model_get_n_items (G_LIST_MODEL (self->contacts)), ui_thread ());
  update_state (self);
}

static void
on_objects_removed (EBookClientView        *view,
                    const GSList           *uids,
                    RevolutionContactsView *self)
{
  guint removed = 0;

  if (view != self->view)
    return;
  for (const GSList *l = uids; l != NULL; l = l->next)
    {
      RevolutionContactsContact *item = g_hash_table_lookup (self->by_uid, l->data);
      guint position;

      g_debug ("Kontakte: %s entfernt %s", book_uid (self), (const char *) l->data);
      if (item == NULL || !g_list_store_find (self->contacts, item, &position))
        continue;
      g_hash_table_remove (self->by_uid, l->data);
      g_list_store_remove (self->contacts, position);
      removed++;
    }
  g_debug ("Kontakte: %s entfernt %u, Liste %u (UI-Thread %s)", book_uid (self), removed,
           g_list_model_get_n_items (G_LIST_MODEL (self->contacts)), ui_thread ());
  update_state (self);
}

/* Nach dem ersten Layout ganz nach oben: GtkListView legt sonst die erste Zeile an die Oberkante und
 * blendet den ersten Abschnittskopf darüber aus (wie mail-view.blp „Viewport“, dort ohne Virtualisierung). */
static gboolean
scroll_to_top (gpointer user_data)
{
  RevolutionContactsView *self = user_data;
  GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (self->contact_list));

  g_debug ("Kontakte: Liste nach oben (von %.0f)", gtk_adjustment_get_value (adjustment));
  gtk_adjustment_set_value (adjustment, 0);
  return G_SOURCE_REMOVE;
}

static void
on_view_complete (EBookClientView        *view,
                  const GError           *error,
                  RevolutionContactsView *self)
{
  if (view != self->view)
    return;
  self->complete = TRUE;
  /* Erst jetzt an die Liste: GtkListView 4.22 zeichnete beim blockweisen Einsortieren (je 16 Kontakte)
   * einen zweiten Kopf „A“ mitten im Abschnitt und begann unterhalb des ersten Kopfes, obwohl das Modell
   * richtige Abschnitte meldet (gtk_section_model_get_section, headless build-devel/m6-368/probe/probe2.png,
   * probe4.png). Danach kommen Änderungen einzeln. */
  gtk_sort_list_model_set_model (self->contact_sorted, G_LIST_MODEL (self->contacts));
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, scroll_to_top, g_object_ref (self), g_object_unref);
  g_debug ("Kontakte: %s fertig, Liste %u, %u Kontaktlisten ausgelassen, nach %" G_GINT64_FORMAT " ms%s%s",
           book_uid (self), g_list_model_get_n_items (G_LIST_MODEL (self->contacts)), self->lists,
           (g_get_monotonic_time () - self->view_started) / 1000, error != NULL ? ", Fehler: " : "",
           error != NULL ? error->message : "");
  if (error != NULL)
    show_error (self, _("Address Book Incomplete"), error->message);
  update_state (self);
  select_pending (self);
}

/* Ansicht abhängen: Handler trennen, Referenz abgeben. dispose der Ansicht meldet dem Server das Ende
 * asynchron (EDS e-book-client-view.c:940-944), daher ohne Aufruf im UI-Thread, der blockiert. */
static void
drop_view (RevolutionContactsView *self)
{
  g_cancellable_cancel (self->view_cancellable);
  g_clear_object (&self->view_cancellable);
  if (self->view != NULL)
    g_signal_handlers_disconnect_by_data (self->view, self);
  g_clear_object (&self->view);
  /* Auch nach dispose aufrufbar (Felder dann NULL). */
  if (self->by_uid != NULL)
    g_hash_table_remove_all (self->by_uid);
  if (self->contact_sorted != NULL)
    gtk_sort_list_model_set_model (self->contact_sorted, NULL);
  if (self->contacts != NULL)
    g_list_store_remove_all (self->contacts);
  self->lists = 0;
  self->complete = FALSE;
  self->search_hint = FALSE;
}

static void
start_thread (GTask        *task,
              gpointer      source_object G_GNUC_UNUSED,
              gpointer      task_data,
              GCancellable *cancellable G_GNUC_UNUSED)
{
  GError *error = NULL;

  e_book_client_view_start (task_data, &error);
  if (error != NULL)
    g_task_return_error (task, error);
  else
    g_task_return_boolean (task, TRUE);
}

static void
on_view_started (GObject      *source_object,
                 GAsyncResult *result,
                 gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionContactsView *self = REVOLUTION_CONTACTS_VIEW (source_object);
  g_autoptr (GError) error = NULL;

  if (g_task_get_task_data (G_TASK (result)) != self->view)
    return;
  if (!g_task_propagate_boolean (G_TASK (result), &error))
    {
      g_debug ("Kontakte: %s Ansicht nicht gestartet: %s", book_uid (self), error->message);
      show_error (self, _("Address Book Unavailable"), error->message);
      /* Kein Laden mehr: Leerzustand unter dem Banner statt Spinner. */
      g_clear_object (&self->view_cancellable);
      update_state (self);
      return;
    }
  g_debug ("Kontakte: %s Ansicht gestartet", book_uid (self));
}

static void
view_thread (GTask        *task,
             gpointer      source_object G_GNUC_UNUSED,
             gpointer      task_data,
             GCancellable *cancellable)
{
  EBookClient *client = g_object_get_data (G_OBJECT (task), "client");
  EBookClientView *view = NULL;
  GError *error = NULL;

  if (e_book_client_get_view_sync (client, task_data, &view, cancellable, &error))
    g_task_return_pointer (task, view, g_object_unref);
  else
    g_task_return_error (task, error);
}

static void
on_view_created (GObject      *source_object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  RevolutionContactsView *self = REVOLUTION_CONTACTS_VIEW (source_object);
  g_autoptr (GError) error = NULL;
  g_autoptr (EBookClientView) view = g_task_propagate_pointer (G_TASK (result), &error);
  g_autoptr (GTask) task = NULL;

  if (GPOINTER_TO_UINT (user_data) != self->generation)
    return;
  if (view == NULL)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
      g_debug ("Kontakte: %s keine Ansicht: %s", book_uid (self), error->message);
      show_error (self, _("Address Book Unavailable"), error->message);
      g_clear_object (&self->view_cancellable);
      update_state (self);
      return;
    }
  self->view = g_steal_pointer (&view);
  g_signal_connect (self->view, "objects-added", G_CALLBACK (on_objects_added), self);
  g_signal_connect (self->view, "objects-modified", G_CALLBACK (on_objects_modified), self);
  g_signal_connect (self->view, "objects-removed", G_CALLBACK (on_objects_removed), self);
  g_signal_connect (self->view, "complete", G_CALLBACK (on_view_complete), self);
  update_state (self);

  task = g_task_new (self, NULL, on_view_started, NULL);
  g_task_set_source_tag (task, on_view_created);
  g_task_set_task_data (task, g_object_ref (self->view), g_object_unref);
  g_task_run_in_thread (task, start_thread);
}

/* Ansicht des gewählten Buchs (neu) anlegen: ohne Suchtext alle Kontakte, mit Suchtext „irgendein Feld
 * enthält“ wie Evolutions Schnellsuche (e-book-shell-view.c:146-186, ohne Text dieselbe Abfrage mit „“).
 * Wie Evolution (e-addressbook-model.c:419-433) ohne Suche nur, wenn das Backend „do-initial-query“ meldet;
 * sonst (z. B. microsoft365-Organisationsbenutzer ohne Offline-Kopie, e-book-backend-m365.c:2571-2576)
 * erst nach einer Suche, dazwischen ein Hinweis. */
static void
open_view (RevolutionContactsView *self)
{
  const char *search = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
  EBookClient *client;
  g_autoptr (EBookQuery) query = NULL;
  g_autoptr (GTask) task = NULL;
  char *sexp;

  drop_view (self);
  self->generation++;
  adw_banner_set_revealed (self->banner, FALSE);
  client = self->book != NULL ? g_hash_table_lookup (self->clients, book_uid (self)) : NULL;
  if (client == NULL)
    {
      update_state (self);
      return;
    }
  if (*search == '\0' && !e_client_check_capability (E_CLIENT (client), "do-initial-query"))
    {
      g_debug ("Kontakte: %s ohne do-initial-query, Kontakte erst nach Suche", book_uid (self));
      self->search_hint = TRUE;
      update_state (self);
      return;
    }
  query = e_book_query_any_field_contains (search);
  sexp = e_book_query_to_string (query);
  g_debug ("Kontakte: %s Ansicht „%s“", book_uid (self), sexp);
  self->view_started = g_get_monotonic_time ();
  self->view_cancellable = g_cancellable_new ();
  task = g_task_new (self, self->view_cancellable, on_view_created, GUINT_TO_POINTER (self->generation));
  g_task_set_source_tag (task, open_view);
  g_task_set_task_data (task, sexp, g_free);
  g_object_set_data_full (G_OBJECT (task), "client", g_object_ref (client), g_object_unref);
  g_task_run_in_thread (task, view_thread);
  update_state (self);
}

/* ------------------------------------------------------------------ Adressbücher */

/* Warten auf „verbunden“ wie Evolutions Adressbuch-Ansicht: gar nicht ((guint32) -1,
 * e-book-shell-view-private.c:338-342); mit Wartezeit dauerte jedes Netz-Buch hier 10 s. */
static void
connect_thread (GTask        *task,
                gpointer      source_object G_GNUC_UNUSED,
                gpointer      task_data,
                GCancellable *cancellable)
{
  GError *error = NULL;
  EClient *client = e_book_client_connect_sync (revolution_contacts_book_get_source (task_data), (guint32) -1,
                                                cancellable, &error);

  if (client == NULL)
    {
      g_task_return_error (task, error);
      return;
    }
  /* Fähigkeiten hier einmal lesen: danach liegen sie im EClient, e_client_check_capability im UI-Thread
   * liest nur noch die Liste (EDS e-client.c:916-931, 991-1015). */
  e_client_check_capability (client, "do-initial-query");
  g_task_return_pointer (task, client, g_object_unref);
}

/* „nur lesend“ kann sich ändern (Backend meldet es z. B. nach dem Verbinden mit dem Server neu). */
static void
on_readonly_changed (EClient                *client,
                     GParamSpec             *pspec G_GNUC_UNUSED,
                     RevolutionContactsView *self)
{
  GListModel *books = G_LIST_MODEL (self->books);

  if (books == NULL)
    return;
  for (guint i = 0; i < g_list_model_get_n_items (books); i++)
    {
      g_autoptr (RevolutionContactsBook) book = g_list_model_get_item (books, i);

      if (e_source_equal (revolution_contacts_book_get_source (book), e_client_get_source (client)))
        revolution_contacts_book_set_readonly (book, e_client_is_readonly (client));
    }
  g_debug ("Kontakte: %s nur lesend %s", e_source_get_uid (e_client_get_source (client)),
           e_client_is_readonly (client) ? "ja" : "nein");
  update_actions (self);
}

static void
on_book_connected (GObject      *source_object,
                   GAsyncResult *result,
                   gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionContactsView *self = REVOLUTION_CONTACTS_VIEW (source_object);
  RevolutionContactsBook *book = g_task_get_task_data (G_TASK (result));
  const char *uid = e_source_get_uid (revolution_contacts_book_get_source (book));
  g_autoptr (GError) error = NULL;
  EClient *client = g_task_propagate_pointer (G_TASK (result), &error);

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  g_hash_table_remove (self->connecting, uid);
  if (client == NULL)
    {
      g_debug ("Kontakte: %s nicht verbunden: %s", uid, error->message);
      if (book == self->book)
        show_error (self, _("Address Book Unavailable"), error->message);
      return;
    }
  g_hash_table_insert (self->clients, g_strdup (uid), client);
  revolution_contacts_book_set_readonly (book, e_client_is_readonly (client));
  g_signal_connect_object (client, "notify::readonly", G_CALLBACK (on_readonly_changed), self, G_CONNECT_DEFAULT);
  g_debug ("Kontakte: %s verbunden, nur lesend %s, do-initial-query %s", uid,
           e_client_is_readonly (client) ? "ja" : "nein",
           e_client_check_capability (client, "do-initial-query") ? "ja" : "nein");
  update_actions (self);
  if (book == self->book)
    open_view (self);
}

static void
connect_book (RevolutionContactsView *self,
              RevolutionContactsBook *book)
{
  const char *uid = e_source_get_uid (revolution_contacts_book_get_source (book));
  g_autoptr (GTask) task = NULL;

  if (g_hash_table_contains (self->clients, uid) || g_hash_table_contains (self->connecting, uid))
    return;
  g_hash_table_add (self->connecting, g_strdup (uid));
  task = g_task_new (self, self->cancellable, on_book_connected, NULL);
  g_task_set_source_tag (task, connect_book);
  g_task_set_task_data (task, g_object_ref (book), g_object_unref);
  g_task_run_in_thread (task, connect_thread);
}

static void
on_book_selected (RevolutionContactsView *self)
{
  RevolutionContactsBook *book = gtk_single_selection_get_selected_item (self->book_selection);

  if (self->syncing || book == self->book)
    return;
  g_set_object (&self->book, book);
  if (book == NULL)
    {
      open_view (self);
      return;
    }
  g_debug ("Kontakte: Buch %s gewählt", book_uid (self));
  update_actions (self);
  open_view (self);
  if (self->connect_all)
    connect_book (self, book);
}

static gboolean
client_gone (gpointer key,
             gpointer value G_GNUC_UNUSED,
             gpointer user_data)
{
  GPtrArray *items = user_data;

  for (guint i = 0; i < items->len; i++)
    if (g_strcmp0 (key, e_source_get_uid (revolution_contacts_book_get_source (g_ptr_array_index (items, i)))) == 0)
      return FALSE;
  g_debug ("Kontakte: %s nicht mehr in der Registry, Verbindung frei", (const char *) key);
  return TRUE;
}

/* Bücher aus der Registry (Lesen im Speicher, im UI-Thread zulässig, docs/ENTWICKLUNG.md „Worker“), nur
 * aktivierte wie Evolution (e_source_registry_list_enabled prüft auch die Eltern-Sammlung). Auswahl bleibt
 * über die UID erhalten; beim ersten Mal REVOLUTION_BOOK (nur für Tests), sonst der erste Eintrag. */
static void
sync_books (RevolutionContactsView *self)
{
  g_autolist (ESource) sources = e_source_registry_list_enabled (self->registry, E_SOURCE_EXTENSION_ADDRESS_BOOK);
  g_autoptr (GPtrArray) items = g_ptr_array_new_with_free_func (g_object_unref);
  g_autofree char *keep = g_strdup (self->book != NULL ? book_uid (self) : g_getenv ("REVOLUTION_BOOK"));
  GListModel *sorted = G_LIST_MODEL (self->book_sorted);
  guint selected = 0;

  for (GList *l = sources; l != NULL; l = l->next)
    g_ptr_array_add (items, revolution_contacts_book_new (self->registry, l->data));
  /* Bestehende Einträge behalten (Markierung „nur lesend“, gewähltes Buch bleibt dasselbe Objekt). */
  for (guint i = 0; i < items->len; i++)
    for (guint j = 0; j < g_list_model_get_n_items (G_LIST_MODEL (self->books)); j++)
      {
        RevolutionContactsBook *new_book = g_ptr_array_index (items, i);
        g_autoptr (RevolutionContactsBook) old = g_list_model_get_item (G_LIST_MODEL (self->books), j);

        if (e_source_equal (revolution_contacts_book_get_source (old), revolution_contacts_book_get_source (new_book)) &&
            g_strcmp0 (revolution_contacts_book_get_group (old), revolution_contacts_book_get_group (new_book)) == 0)
          {
            g_ptr_array_index (items, i) = g_steal_pointer (&old);
            g_object_unref (new_book);
            break;
          }
      }
  self->syncing = TRUE;
  g_list_store_splice (self->books, 0, g_list_model_get_n_items (G_LIST_MODEL (self->books)), items->pdata, items->len);
  /* Verbindungen entfernter oder abgeschalteter Bücher freigeben. */
  g_hash_table_foreach_remove (self->clients, client_gone, items);

  for (guint i = 0; i < g_list_model_get_n_items (sorted); i++)
    {
      g_autoptr (RevolutionContactsBook) book = g_list_model_get_item (sorted, i);
      ESource *source = revolution_contacts_book_get_source (book);

      EClient *client = g_hash_table_lookup (self->clients, e_source_get_uid (source));

      g_debug ("Adressbuch: %u %s %s / %s", i, e_source_get_uid (source), revolution_contacts_book_get_group (book),
               e_source_get_display_name (source));
      if (client != NULL)
        revolution_contacts_book_set_readonly (book, e_client_is_readonly (client));
      if (g_strcmp0 (keep, e_source_get_uid (source)) == 0)
        selected = i;
      if (self->connect_all)
        connect_book (self, book);
    }
  gtk_single_selection_set_selected (self->book_selection, selected);
  self->syncing = FALSE;
  on_book_selected (self);
}

/* Erst wenn die Kontakte-Seite erscheint, alle Bücher verbinden (Markierung „nur lesend“); vorher nichts,
 * damit der Mail-Start keine Adressbuch-Backends weckt. */
static void
on_map (RevolutionContactsView *self,
        gpointer                user_data G_GNUC_UNUSED)
{
  GListModel *books = G_LIST_MODEL (self->books);
  GtkWidget *focus = gtk_root_get_focus (gtk_widget_get_root (GTK_WIDGET (self)));

  /* Nach Strg+3 liegt der Fokus noch in der verborgenen Mail-Ansicht oder nirgends; die Kürzel der
   * Kontakte (Strg+F, Alt+Buchstabe, contacts-view.blp) wirken nur mit Fokus hier. */
  if (focus == NULL || !gtk_widget_is_ancestor (focus, GTK_WIDGET (self)))
    if (!gtk_widget_grab_focus (GTK_WIDGET (self->contact_list)))
      gtk_widget_grab_focus (GTK_WIDGET (self->book_list));
  if (self->connect_all)
    return;
  self->connect_all = TRUE;
  /* #370: Schließen des Fensters beendet die Anwendung, ohne dass die Ansicht zerstört wird (gemessen:
   * build-devel/m6-370/beenden.txt); vorgemerktes Löschen daher schon hier ausführen. */
  g_signal_connect_object (gtk_widget_get_root (GTK_WIDGET (self)), "close-request", G_CALLBACK (on_close_request), self,
                           G_CONNECT_SWAPPED);
  for (guint i = 0; i < g_list_model_get_n_items (books); i++)
    {
      g_autoptr (RevolutionContactsBook) book = g_list_model_get_item (books, i);

      connect_book (self, book);
    }
}

/* Nur Adressbücher und deren Konten (Eltern-Source, z. B. umbenannte Sammlung) bauen die Liste neu, nicht
 * jede Änderung an Mail-Quellen (Signale der Registry kommen auch für Mail, Kalender, Identitäten). */
static void
on_source_changed (ESourceRegistry        *registry G_GNUC_UNUSED,
                   ESource                *source,
                   RevolutionContactsView *self)
{
  GListModel *books = G_LIST_MODEL (self->books);
  gboolean relevant = e_source_has_extension (source, E_SOURCE_EXTENSION_ADDRESS_BOOK);

  if (self->registry == NULL)
    return;
  for (guint i = 0; !relevant && i < g_list_model_get_n_items (books); i++)
    {
      g_autoptr (RevolutionContactsBook) book = g_list_model_get_item (books, i);

      relevant = g_strcmp0 (e_source_get_parent (revolution_contacts_book_get_source (book)), e_source_get_uid (source)) == 0;
    }
  if (relevant)
    sync_books (self);
}

void
revolution_contacts_view_set_registry (RevolutionContactsView *self,
                                       ESourceRegistry        *registry)
{
  const char *signals[] = { "source-added", "source-removed", "source-enabled", "source-disabled", "source-changed" };

  g_return_if_fail (REVOLUTION_IS_CONTACTS_VIEW (self));
  g_return_if_fail (E_IS_SOURCE_REGISTRY (registry));
  g_return_if_fail (self->registry == NULL);

  self->registry = g_object_ref (registry);
  for (gsize i = 0; i < G_N_ELEMENTS (signals); i++)
    g_signal_connect_object (registry, signals[i], G_CALLBACK (on_source_changed), self, G_CONNECT_DEFAULT);
  sync_books (self);
  if (gtk_widget_get_mapped (GTK_WIDGET (self)))
    on_map (self, NULL);
}

/* ------------------------------------------------------------------ Liste, Suche */

static void
on_contact_selected (RevolutionContactsView *self)
{
  RevolutionContactsContact *item = gtk_single_selection_get_selected_item (self->contact_selection);

  /* #369: Detail der Auswahl, ohne Auswahl (auch nach Entfernen des Eintrags) leer. */
  revolution_contacts_detail_set_contact (self->detail, item);
  update_actions (self);
}

static void
on_search_changed (RevolutionContactsView *self)
{
  if (self->book != NULL)
    open_view (self);
}

static gboolean
jump_idle (gpointer user_data)
{
  RevolutionContactsView *self = user_data;

  if (self->jump_target < g_list_model_get_n_items (G_LIST_MODEL (self->contact_sorted)))
    gtk_list_view_scroll_to (self->contact_list, self->jump_target, GTK_LIST_SCROLL_FOCUS, NULL);
  return G_SOURCE_REMOVE;
}

/* contacts.jump: erster Kontakt, dessen Anfangsbuchstabe nicht vor dem gewählten liegt (A–Z-Leiste, Alt+Buchstabe). */
static void
on_jump (GSimpleAction *action G_GNUC_UNUSED,
         GVariant      *parameter,
         gpointer       user_data)
{
  RevolutionContactsView *self = user_data;
  GListModel *model = G_LIST_MODEL (self->contact_sorted);
  const char *letter = g_variant_get_string (parameter, NULL);
  guint n = g_list_model_get_n_items (model), i;

  if (n == 0)
    return;
  for (i = 0; i < n; i++)
    {
      g_autoptr (RevolutionContactsContact) item = g_list_model_get_item (model, i);

      if (g_utf8_collate (revolution_contacts_contact_get_initial (item), letter) >= 0)
        break;
    }
  if (i == n)
    i = n - 1;
  g_debug ("Kontakte: Sprung %s → Zeile %u", letter, i);
  /* scroll_to bringt die Zeile nur ins Bild (unten, wenn sie darunter lag); erst ans Ende, nach dem Layout
   * zurück auf die Zeile: dann steht der Buchstabe oben wie in einem Register. */
  gtk_list_view_scroll_to (self->contact_list, n - 1, GTK_LIST_SCROLL_NONE, NULL);
  self->jump_target = i;
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, jump_idle, g_object_ref (self), g_object_unref);
}

/* Fokus ins Suchfeld ohne alles zu markieren (wie mail-view.c). */
static void
focus_search_entry (RevolutionContactsView *self)
{
  GtkEditable *text = gtk_editable_get_delegate (GTK_EDITABLE (self->search_entry));

  gtk_text_grab_focus_without_selecting (GTK_TEXT (text));
}

/* Suchknopf schaltet die Kopfleiste des Kontakts zwischen den Aktionen und dem
 * Suchfeld um („Lights off“, wie mail-view.c), Fokus wandert mit. */
static void
on_search_toggled (RevolutionContactsView *self)
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
on_search_started (RevolutionContactsView *self)
{
  gtk_toggle_button_set_active (self->search_button, TRUE);
}

/* Escape im Suchfeld (GtkSearchEntry::stop-search). */
static void
on_search_stop (RevolutionContactsView *self)
{
  gtk_toggle_button_set_active (self->search_button, FALSE);
}

/* contacts.search für Strg+F: öffnet nur, schließt nicht (wie mail.search). */
static void
on_search (GSimpleAction *action G_GNUC_UNUSED,
           GVariant      *parameter G_GNUC_UNUSED,
           gpointer       user_data)
{
  RevolutionContactsView *self = user_data;

  if (gtk_toggle_button_get_active (self->search_button))
    focus_search_entry (self);
  else
    gtk_toggle_button_set_active (self->search_button, TRUE);
}

/* contacts.toggle-sidebar fürs Hauptmenü: zustandslos wie mail.toggle-sidebar. */
static void
on_toggle_sidebar (GSimpleAction *action G_GNUC_UNUSED,
                   GVariant      *parameter G_GNUC_UNUSED,
                   gpointer       user_data)
{
  RevolutionContactsView *self = user_data;

  adw_overlay_split_view_set_show_sidebar (self->split, !adw_overlay_split_view_get_show_sidebar (self->split));
}

/* ------------------------------------------------------------------ Anlegen, Bearbeiten, Löschen (#370) */

/* Schreiben, Löschen und Suche nach der Adresse im Worker (Architektur 1). Ohne verbundenen Client verbindet der
 * Worker selbst (z. B. „Absender übernehmen“ vor dem ersten Anzeigen der Kontakte). */
typedef enum { JOB_ADD, JOB_MODIFY, JOB_REMOVE, JOB_FIND } JobKind;

static const char *const job_names[] = { "anlegen", "ändern", "löschen", "suchen" };

typedef struct
{
  JobKind      kind;
  ESource     *source;
  EBookClient *client;
  EContact    *contact;  /* anlegen/ändern/löschen: der Kontakt; suchen: Vorlage, danach der Treffer oder NULL */
  char        *address;  /* suchen */
  char        *uid;      /* anlegen: neue UID */
} Job;

static void
job_free (Job *job)
{
  g_clear_object (&job->source);
  g_clear_object (&job->client);
  g_clear_object (&job->contact);
  g_free (job->address);
  g_free (job->uid);
  g_free (job);
}

static void
job_thread (GTask        *task,
            gpointer      source_object G_GNUC_UNUSED,
            gpointer      task_data,
            GCancellable *cancellable G_GNUC_UNUSED)
{
  Job *job = task_data;
  GError *error = NULL;
  gboolean done = FALSE;

  if (job->client == NULL)
    job->client = E_BOOK_CLIENT (e_book_client_connect_sync (job->source, (guint32) -1, NULL, &error));
  if (job->client == NULL)
    {
      g_task_return_error (task, error);
      return;
    }
  switch (job->kind)
    {
    case JOB_ADD:
      done = e_book_client_add_contact_sync (job->client, job->contact, E_BOOK_OPERATION_FLAG_NONE, &job->uid, NULL, &error);
      break;
    case JOB_MODIFY:
      done = e_book_client_modify_contact_sync (job->client, job->contact, E_BOOK_OPERATION_FLAG_NONE, NULL, &error);
      break;
    case JOB_REMOVE:
      done = e_book_client_remove_contact_sync (job->client, job->contact, E_BOOK_OPERATION_FLAG_NONE, NULL, &error);
      break;
    case JOB_FIND:
      {
        g_autoptr (EBookQuery) query = e_book_query_field_test (E_CONTACT_EMAIL, E_BOOK_QUERY_IS, job->address);
        g_autofree char *sexp = e_book_query_to_string (query);
        GSList *found = NULL;

        done = e_book_client_get_contacts_sync (job->client, sexp, &found, NULL, &error);
        g_clear_object (&job->contact);
        for (GSList *l = found; done && l != NULL && job->contact == NULL; l = l->next)
          if (!is_list (l->data))
            job->contact = g_object_ref (l->data);
        g_slist_free_full (found, g_object_unref);
      }
      break;
    }
  if (done)
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
}

static void
run_job (gpointer             source_object,
         Job                 *job,
         GAsyncReadyCallback  callback)
{
  g_autoptr (GTask) task = g_task_new (source_object, NULL, callback, NULL);

  g_debug ("Kontakt: %s %s %s", job_names[job->kind], e_source_get_uid (job->source),
           job->contact != NULL && e_contact_get_const (job->contact, E_CONTACT_UID) != NULL
           ? (const char *) e_contact_get_const (job->contact, E_CONTACT_UID) : "-");
  g_task_set_source_tag (task, run_job);
  g_task_set_task_data (task, job, (GDestroyNotify) job_free);
  g_task_run_in_thread (task, job_thread);
}

static Job *
job_new (RevolutionContactsView *self,
         JobKind                 kind,
         RevolutionContactsBook *book,
         EContact               *contact)
{
  Job *job = g_new0 (Job, 1);
  ESource *source = revolution_contacts_book_get_source (book);
  EBookClient *client = self->clients != NULL ? g_hash_table_lookup (self->clients, e_source_get_uid (source)) : NULL;

  job->kind = kind;
  job->source = g_object_ref (source);
  job->client = client != NULL ? g_object_ref (client) : NULL;
  job->contact = contact != NULL ? g_object_ref (contact) : NULL;
  return job;
}

static gboolean
book_readonly (RevolutionContactsBook *book)
{
  gboolean readonly = TRUE;

  if (book != NULL)
    g_object_get (book, "readonly", &readonly, NULL);
  return readonly;
}

static void
set_enabled (RevolutionContactsView *self,
             const char             *name,
             gboolean                enabled)
{
  g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (self->contact_actions, name)), enabled);
}

/* Nur-lesend-Bücher bieten kein Bearbeiten und Löschen an (Dokument #19); vor dem Verbinden ist „nur lesend“
 * unbekannt, die Knöpfe sind dann sichtbar, aber aus. */
static void
update_actions (RevolutionContactsView *self)
{
  gboolean writable = self->book != NULL && !book_readonly (self->book);
  gboolean connected = self->book != NULL && self->clients != NULL &&
                       g_hash_table_contains (self->clients, book_uid (self));
  gboolean selected = gtk_single_selection_get_selected_item (self->contact_selection) != NULL;
  gboolean any_writable = FALSE;
  GListModel *books = G_LIST_MODEL (self->books);

  if (self->contact_actions == NULL)
    return;
  for (guint i = 0; books != NULL && !any_writable && i < g_list_model_get_n_items (books); i++)
    {
      g_autoptr (RevolutionContactsBook) book = g_list_model_get_item (books, i);

      any_writable = !book_readonly (book);
    }
  set_enabled (self, "new", !self->editing && any_writable);
  set_enabled (self, "edit", !self->editing && writable && connected && selected);
  set_enabled (self, "delete", !self->editing && writable && connected && selected);
  set_enabled (self, "save", self->editing && !self->saving && revolution_contacts_editor_is_valid (self->editor));
  set_enabled (self, "cancel", self->editing && !self->saving);
  gtk_widget_set_visible (self->edit_button, writable);
  gtk_widget_set_visible (self->delete_button, writable);
}

static void
on_editor_valid (RevolutionContactsView *self)
{
  update_actions (self);
}

/* Formular im Detailbereich, Kopfleiste Abbrechen | Titel | Hinzufügen/Fertig (GNOME Contacts); Liste und
 * Seitenleiste ruhen, bis das Formular zu ist. */
static void
show_editor (RevolutionContactsView *self,
             gboolean                editing,
             gboolean                is_new)
{
  self->editing = editing;
  gtk_label_set_label (self->edit_title, is_new ? _("New Contact") : _("Edit Contact"));
  gtk_button_set_label (self->done_button, is_new ? _("_Add") : _("_Done"));
  gtk_stack_set_visible_child_name (self->header_stack, editing ? "edit" : "actions");
  gtk_stack_set_visible_child_name (self->pane_stack, editing ? "editor" : "detail");
  gtk_widget_set_visible (GTK_WIDGET (self->search_button), !editing);
  gtk_widget_set_sensitive (GTK_WIDGET (self->contact_list), !editing);
  gtk_widget_set_sensitive (GTK_WIDGET (self->book_list), !editing);
  gtk_widget_set_sensitive (self->letter_bar, !editing);
  update_actions (self);
  if (!editing)
    {
      g_clear_object (&self->edit_book);
      g_clear_pointer (&self->edit_books, g_ptr_array_unref);
      if (!gtk_widget_grab_focus (GTK_WIDGET (self->contact_list)))
        gtk_widget_grab_focus (GTK_WIDGET (self->book_list));
    }
}

/* Anlegen: schreibbare Bücher wie Evolutions Quellen-Auswahl (Bücher, die nicht als nur lesend bekannt sind);
 * Vorgabe das Buch target (sonst das gewählte, sonst das erste). template: Vorbelegung oder NULL. */
static void
open_new (RevolutionContactsView *self,
          EContact               *template,
          RevolutionContactsBook *target)
{
  GListModel *sorted = G_LIST_MODEL (self->book_sorted);
  g_autoptr (GtkStringList) labels = gtk_string_list_new (NULL);
  g_autoptr (GString) log = g_string_new (NULL);
  guint selected = 0;

  g_clear_pointer (&self->edit_books, g_ptr_array_unref);
  self->edit_books = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < g_list_model_get_n_items (sorted); i++)
    {
      g_autoptr (RevolutionContactsBook) book = g_list_model_get_item (sorted, i);
      g_autofree char *label = NULL;

      if (book_readonly (book))
        continue;
      if (book == (target != NULL ? target : self->book))
        selected = self->edit_books->len;
      label = g_strdup_printf ("%s (%s)", e_source_get_display_name (revolution_contacts_book_get_source (book)),
                               revolution_contacts_book_get_group (book));
      gtk_string_list_append (labels, label);
      g_string_append_printf (log, " %s", e_source_get_uid (revolution_contacts_book_get_source (book)));
      g_ptr_array_add (self->edit_books, g_object_ref (book));
    }
  if (self->edit_books->len == 0)
    {
      show_error (self, _("No Writable Address Book"), _("All address books are read-only."));
      return;
    }
  g_debug ("Kontakt-Formular: neu, Bücher%s, Vorgabe %s", log->str,
           e_source_get_uid (revolution_contacts_book_get_source (g_ptr_array_index (self->edit_books, selected))));
  revolution_contacts_editor_edit (self->editor, template, G_LIST_MODEL (labels), selected);
  show_editor (self, TRUE, TRUE);
}

static void
open_edit (RevolutionContactsView *self,
           RevolutionContactsBook *book,
           EContact               *contact)
{
  g_set_object (&self->edit_book, book);
  g_debug ("Kontakt-Formular: bearbeiten %s %s", e_source_get_uid (revolution_contacts_book_get_source (book)),
           (const char *) e_contact_get_const (contact, E_CONTACT_UID));
  revolution_contacts_editor_edit (self->editor, contact, NULL, 0);
  show_editor (self, TRUE, FALSE);
}

static void
on_new (GSimpleAction *action G_GNUC_UNUSED,
        GVariant      *parameter G_GNUC_UNUSED,
        gpointer       user_data)
{
  open_new (user_data, NULL, NULL);
}

static void
on_edit (GSimpleAction *action G_GNUC_UNUSED,
         GVariant      *parameter G_GNUC_UNUSED,
         gpointer       user_data)
{
  RevolutionContactsView *self = user_data;
  RevolutionContactsContact *item = gtk_single_selection_get_selected_item (self->contact_selection);

  if (item != NULL && self->book != NULL)
    open_edit (self, self->book, revolution_contacts_contact_get_contact (item));
}

static void
on_cancel (GSimpleAction *action G_GNUC_UNUSED,
           GVariant      *parameter G_GNUC_UNUSED,
           gpointer       user_data)
{
  g_debug ("Kontakt-Formular: abgebrochen, nichts geschrieben");
  show_editor (user_data, FALSE, FALSE);
}

/* Nach dem Anlegen den neuen Kontakt wählen, sobald die Live-Ansicht ihn meldet (on_objects_added,
 * on_view_complete). */
static void
select_pending (RevolutionContactsView *self)
{
  RevolutionContactsContact *item = self->select_uid != NULL ? g_hash_table_lookup (self->by_uid, self->select_uid) : NULL;
  GListModel *sorted = G_LIST_MODEL (self->contact_sorted);

  if (item == NULL || !self->complete)
    return;
  for (guint i = 0; i < g_list_model_get_n_items (sorted); i++)
    {
      g_autoptr (RevolutionContactsContact) candidate = g_list_model_get_item (sorted, i);

      if (candidate != item)
        continue;
      g_debug ("Kontakte: neuer Kontakt %s gewählt (Zeile %u)", self->select_uid, i);
      g_clear_pointer (&self->select_uid, g_free);
      gtk_single_selection_set_selected (self->contact_selection, i);
      gtk_list_view_scroll_to (self->contact_list, i, GTK_LIST_SCROLL_NONE, NULL);
      return;
    }
}

static void
select_book (RevolutionContactsView *self,
             ESource                *source)
{
  GListModel *sorted = G_LIST_MODEL (self->book_sorted);

  for (guint i = 0; i < g_list_model_get_n_items (sorted); i++)
    {
      g_autoptr (RevolutionContactsBook) book = g_list_model_get_item (sorted, i);

      if (e_source_equal (revolution_contacts_book_get_source (book), source))
        gtk_single_selection_set_selected (self->book_selection, i);
    }
}

static void
on_saved (GObject      *source_object,
          GAsyncResult *result,
          gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionContactsView *self = REVOLUTION_CONTACTS_VIEW (source_object);
  Job *job = g_task_get_task_data (G_TASK (result));
  g_autoptr (GError) error = NULL;

  self->saving = FALSE;
  if (self->contacts == NULL)
    return;
  if (!g_task_propagate_boolean (G_TASK (result), &error))
    {
      /* Formular bleibt mit den Eingaben offen (z. B. ETag-Konflikt, offline). */
      g_debug ("Kontakt: %s %s Fehler: %s", job_names[job->kind], e_source_get_uid (job->source), error->message);
      show_error (self, _("Could Not Save Contact"), error->message);
      update_actions (self);
      return;
    }
  g_debug ("Kontakt: %s %s %s fertig", job_names[job->kind], e_source_get_uid (job->source),
           job->kind == JOB_ADD ? job->uid : (const char *) e_contact_get_const (job->contact, E_CONTACT_UID));
  adw_banner_set_revealed (self->banner, FALSE);
  if (job->kind == JOB_ADD)
    {
      g_free (self->select_uid);
      self->select_uid = g_strdup (job->uid);
      if (self->book == NULL || !e_source_equal (revolution_contacts_book_get_source (self->book), job->source))
        select_book (self, job->source);
      else
        select_pending (self);
    }
  show_editor (self, FALSE, FALSE);
}

static void
on_save (GSimpleAction *action G_GNUC_UNUSED,
         GVariant      *parameter G_GNUC_UNUSED,
         gpointer       user_data)
{
  RevolutionContactsView *self = user_data;
  g_autoptr (EContact) contact = revolution_contacts_editor_build (self->editor);
  gboolean is_new = self->edit_book == NULL;
  RevolutionContactsBook *book = is_new ? g_ptr_array_index (self->edit_books,
                                                            MIN (revolution_contacts_editor_get_book (self->editor),
                                                                 self->edit_books->len - 1))
                                        : self->edit_book;

  if (!revolution_contacts_editor_is_valid (self->editor) || self->saving)
    return;
  self->saving = TRUE;
  update_actions (self);
  run_job (self, job_new (self, is_new ? JOB_ADD : JOB_MODIFY, book, contact), on_saved);
}

/* Löschen wie GNOME Contacts (contacts-main-window.vala delete_contacts, contacts-operation-list.vala
 * execute_with_timeout): Kontakt sofort aus der Liste, Toast mit Rückgängig, gelöscht wird erst nach Ablauf
 * der Toast-Zeit; Rückgängig davor schreibt nichts. Beim Beenden sofort (contacts-app.vala quit_action:
 * operations.flush), hier beim Schließen des Fensters (on_close_request) und beim Zerstören der Ansicht; das Beenden
 * wartet darauf (g_application_hold). */
#define DELETE_SECONDS 5

static void
pending_free (PendingDelete *pending)
{
  g_clear_handle_id (&pending->timeout_id, g_source_remove);
  g_clear_object (&pending->book);
  g_clear_object (&pending->client);
  g_clear_object (&pending->item);
  g_clear_object (&pending->toast);
  g_free (pending);
}

/* Eintrag wieder in die Liste (Rückgängig, Fehler), nur wenn sein Buch noch gewählt ist; sonst zeigt die
 * nächste Ansicht des Buchs ihn ohnehin. */
static void
restore_item (RevolutionContactsView    *self,
              RevolutionContactsBook    *book,
              RevolutionContactsContact *item)
{
  const char *uid = revolution_contacts_contact_get_uid (item);

  if (book != self->book || g_hash_table_contains (self->by_uid, uid))
    return;
  g_hash_table_insert (self->by_uid, g_strdup (uid), item);
  g_list_store_append (self->contacts, item);
  update_state (self);
}

static void
on_removed (GObject      *source_object,
            GAsyncResult *result,
            gpointer      user_data)
{
  Job *job = g_task_get_task_data (G_TASK (result));
  RevolutionContactsView *self = source_object != NULL ? REVOLUTION_CONTACTS_VIEW (source_object) : NULL;
  g_autoptr (RevolutionContactsBook) book = g_object_get_data (G_OBJECT (result), "book") != NULL
                                            ? g_object_ref (g_object_get_data (G_OBJECT (result), "book")) : NULL;
  g_autoptr (GError) error = NULL;
  GApplication *application = user_data;

  if (g_task_propagate_boolean (G_TASK (result), &error))
    g_debug ("Kontakt: löschen %s %s fertig", e_source_get_uid (job->source),
             (const char *) e_contact_get_const (job->contact, E_CONTACT_UID));
  else
    {
      g_debug ("Kontakt: löschen %s Fehler: %s", e_source_get_uid (job->source), error->message);
      if (self != NULL && self->contacts != NULL)
        {
          RevolutionContactsContact *item = g_object_get_data (G_OBJECT (result), "item");

          show_error (self, _("Could Not Delete Contact"), error->message);
          if (item != NULL)
            restore_item (self, book, item);
        }
    }
  if (application != NULL)
    {
      g_application_release (application);
      g_object_unref (application);
    }
}

/* Vorgemerktes Löschen jetzt ausführen (Ablauf, nächstes Löschen, Beenden). detached: die Ansicht wird gerade
 * zerstört, der Auftrag hängt dann nicht an ihr. */
static void
execute_pending (RevolutionContactsView *self,
                 gboolean                detached)
{
  PendingDelete *pending = g_steal_pointer (&self->pending);
  GApplication *application = g_application_get_default ();
  g_autoptr (GTask) task = NULL;
  Job *job;

  if (pending == NULL)
    return;
  g_clear_handle_id (&pending->timeout_id, g_source_remove);
  adw_toast_dismiss (pending->toast);
  job = g_new0 (Job, 1);
  job->kind = JOB_REMOVE;
  job->source = g_object_ref (revolution_contacts_book_get_source (pending->book));
  job->client = g_object_ref (pending->client);
  job->contact = g_object_ref (revolution_contacts_contact_get_contact (pending->item));
  g_debug ("Kontakt: löschen %s %s%s", e_source_get_uid (job->source),
           (const char *) e_contact_get_const (job->contact, E_CONTACT_UID), detached ? " (beim Beenden)" : "");
  if (application != NULL)
    g_application_hold (g_object_ref (application));
  task = g_task_new (detached ? NULL : self, NULL, on_removed, application);
  g_task_set_source_tag (task, execute_pending);
  g_task_set_task_data (task, job, (GDestroyNotify) job_free);
  g_object_set_data_full (G_OBJECT (task), "book", g_object_ref (pending->book), g_object_unref);
  g_object_set_data_full (G_OBJECT (task), "item", g_object_ref (pending->item), g_object_unref);
  g_task_run_in_thread (task, job_thread);
  pending_free (pending);
}

/* Fenster schließen wie Beenden (GNOME Contacts close_request → quit → flush): sofort löschen, die Anwendung
 * hält sich, bis der Auftrag fertig ist (execute_pending). FALSE: Schließen geht weiter. */
static gboolean
on_close_request (RevolutionContactsView *self)
{
  if (self->pending != NULL)
    g_debug ("Kontakt: Fenster geschlossen, vorgemerktes Löschen jetzt");
  execute_pending (self, FALSE);
  return FALSE;
}

static gboolean
on_delete_timeout (gpointer user_data)
{
  RevolutionContactsView *self = user_data;

  self->pending->timeout_id = 0;
  execute_pending (self, FALSE);
  return G_SOURCE_REMOVE;
}

static void
on_undo (RevolutionContactsView *self)
{
  PendingDelete *pending = g_steal_pointer (&self->pending);

  if (pending == NULL)
    return;
  g_debug ("Kontakt: Löschen rückgängig %s, nichts geschrieben", revolution_contacts_contact_get_uid (pending->item));
  restore_item (self, pending->book, pending->item);
  pending_free (pending);
}

static void
on_delete (GSimpleAction *action G_GNUC_UNUSED,
           GVariant      *parameter G_GNUC_UNUSED,
           gpointer       user_data)
{
  RevolutionContactsView *self = user_data;
  RevolutionContactsContact *item = gtk_single_selection_get_selected_item (self->contact_selection);
  EBookClient *client = self->book != NULL ? g_hash_table_lookup (self->clients, book_uid (self)) : NULL;
  GtkWidget *overlay = gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_TOAST_OVERLAY);
  PendingDelete *pending;
  guint position;

  if (item == NULL || client == NULL)
    return;
  execute_pending (self, FALSE);
  pending = g_new0 (PendingDelete, 1);
  pending->book = g_object_ref (self->book);
  pending->client = g_object_ref (client);
  pending->item = g_object_ref (item);
  pending->toast = adw_toast_new (_("Contact deleted"));
  adw_toast_set_button_label (pending->toast, _("_Undo"));
  adw_toast_set_timeout (pending->toast, DELETE_SECONDS);
  g_signal_connect_object (pending->toast, "button-clicked", G_CALLBACK (on_undo), self, G_CONNECT_SWAPPED);
  pending->timeout_id = g_timeout_add_seconds (DELETE_SECONDS, on_delete_timeout, self);
  self->pending = pending;
  g_debug ("Kontakt: Löschen vorgemerkt %s %s „%s“, in %d s", book_uid (self), revolution_contacts_contact_get_uid (item),
           revolution_contacts_contact_get_name (item), DELETE_SECONDS);

  g_hash_table_remove (self->by_uid, revolution_contacts_contact_get_uid (item));
  if (g_list_store_find (self->contacts, item, &position))
    g_list_store_remove (self->contacts, position);
  update_state (self);
  if (overlay != NULL)
    adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (overlay), g_object_ref (pending->toast));
}

/* „Absender als Kontakt übernehmen“ (mail-view.c message.add-sender) wie Evolution mail-add-sender
 * (e-mail-reader.c:163-220 → e_contact_quick_add_email, e-contact-quick-add.c:698-726: „Name <Adresse>“ zerlegen,
 * Anführungszeichen weg, 589-602). Evolution sucht die Adresse im Zielbuch (eab_merging_book_find_contact,
 * e-contact-quick-add.c:334-336) und öffnet bei einem Treffer diesen Kontakt im Editor (ce_have_contact,
 * 259-268); hier ebenso, mit Hinweis, ohne Duplikat-Dialog. Zielbuch: das gewählte, wenn schreibbar, sonst das
 * erste schreibbare (Evolution: Standard-Adressbuch). */
static void
on_sender_found (GObject      *source_object,
                 GAsyncResult *result,
                 gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionContactsView *self = REVOLUTION_CONTACTS_VIEW (source_object);
  Job *job = g_task_get_task_data (G_TASK (result));
  RevolutionContactsBook *book = g_object_get_data (G_OBJECT (result), "book");
  EContact *template = g_object_get_data (G_OBJECT (result), "template");
  g_autoptr (GError) error = NULL;

  if (self->contacts == NULL || self->editing)
    return;
  if (!g_task_propagate_boolean (G_TASK (result), &error))
    g_debug ("Kontakt: Absender suchen %s Fehler: %s", e_source_get_uid (job->source), error->message);
  if (job->contact != NULL)
    {
      GtkWidget *overlay = gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_TOAST_OVERLAY);

      g_debug ("Kontakt: Absender %s schon im Buch %s als %s", job->address, e_source_get_uid (job->source),
               (const char *) e_contact_get_const (job->contact, E_CONTACT_UID));
      if (overlay != NULL)
        adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (overlay), adw_toast_new (_("Contact already exists")));
      open_edit (self, book, job->contact);
      return;
    }
  open_new (self, template, book);
}

void
revolution_contacts_view_add_sender (RevolutionContactsView *self,
                                     const char             *from)
{
  g_autoptr (CamelInternetAddress) cia = camel_internet_address_new ();
  g_autoptr (EContact) template = e_contact_new ();
  g_autoptr (GTask) task = NULL;
  g_autofree char *clean = NULL;
  const char *name = NULL, *address = NULL;
  RevolutionContactsBook *target = NULL;
  GListModel *sorted;
  Job *job;

  g_return_if_fail (REVOLUTION_IS_CONTACTS_VIEW (self));

  if (from == NULL || camel_address_decode (CAMEL_ADDRESS (cia), from) <= 0 ||
      !camel_internet_address_get (cia, 0, &name, &address) || address == NULL || *address == '\0' || self->editing)
    return;
  if (name != NULL)
    {
      gsize len;

      clean = g_strstrip (g_strdup (name));
      len = strlen (clean);
      if (len >= 2 && ((clean[0] == '\'' && clean[len - 1] == '\'') || (clean[0] == '"' && clean[len - 1] == '"')))
        {
          clean[len - 1] = '\0';
          memmove (clean, clean + 1, len - 1);
          g_strstrip (clean);
        }
    }
  if (clean != NULL && *clean != '\0' && g_ascii_strcasecmp (clean, address) != 0)
    e_contact_set (template, E_CONTACT_FULL_NAME, clean);
  e_contact_set (template, E_CONTACT_EMAIL_1, address);

  sorted = G_LIST_MODEL (self->book_sorted);
  if (self->book != NULL && !book_readonly (self->book))
    target = self->book;
  for (guint i = 0; target == NULL && i < g_list_model_get_n_items (sorted); i++)
    {
      g_autoptr (RevolutionContactsBook) book = g_list_model_get_item (sorted, i);

      if (!book_readonly (book))
        target = book;
    }
  g_debug ("Kontakt: Absender übernehmen „%s“ <%s>, Buch %s", clean != NULL ? clean : "", address,
           target != NULL ? e_source_get_uid (revolution_contacts_book_get_source (target)) : "-");
  if (target == NULL)
    {
      open_new (self, template, NULL);
      return;
    }
  job = job_new (self, JOB_FIND, target, NULL);
  job->address = g_strdup (address);
  task = g_task_new (self, NULL, on_sender_found, NULL);
  g_task_set_source_tag (task, revolution_contacts_view_add_sender);
  g_task_set_task_data (task, job, (GDestroyNotify) job_free);
  g_object_set_data_full (G_OBJECT (task), "book", g_object_ref (target), g_object_unref);
  g_object_set_data_full (G_OBJECT (task), "template", g_object_ref (template), g_object_unref);
  g_debug ("Kontakt: suchen %s %s", e_source_get_uid (job->source), address);
  g_task_run_in_thread (task, job_thread);
}

static const GActionEntry contact_actions[] = {
  { "new", on_new, NULL, NULL, NULL, { 0 } },
  { "edit", on_edit, NULL, NULL, NULL, { 0 } },
  { "delete", on_delete, NULL, NULL, NULL, { 0 } },
  { "save", on_save, NULL, NULL, NULL, { 0 } },
  { "cancel", on_cancel, NULL, NULL, NULL, { 0 } },
};

static const GActionEntry contacts_actions[] = {
  { "search", on_search, NULL, NULL, NULL, { 0 } },
  { "toggle-sidebar", on_toggle_sidebar, NULL, NULL, NULL, { 0 } },
  { "jump", on_jump, "s", NULL, NULL, { 0 } },
};

/* Klasse .dark für Regeln nur im dunklen Design (style.css), wie mail-view.c. */
static void
on_dark_changed (AdwStyleManager        *manager,
                 GParamSpec             *pspec G_GNUC_UNUSED,
                 RevolutionContactsView *self)
{
  if (adw_style_manager_get_dark (manager))
    gtk_widget_add_css_class (GTK_WIDGET (self), "dark");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "dark");
}

enum { PROP_0, PROP_STACK, N_PROPS };

static void
revolution_contacts_view_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  RevolutionContactsView *self = REVOLUTION_CONTACTS_VIEW (object);

  if (prop_id == PROP_STACK)
    g_value_set_object (value, self->stack);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
revolution_contacts_view_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  RevolutionContactsView *self = REVOLUTION_CONTACTS_VIEW (object);

  if (prop_id == PROP_STACK)
    g_set_object (&self->stack, g_value_get_object (value));
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
revolution_contacts_view_dispose (GObject *object)
{
  RevolutionContactsView *self = REVOLUTION_CONTACTS_VIEW (object);

  g_cancellable_cancel (self->cancellable);
  /* #370: vorgemerktes Löschen beim Beenden sofort (GNOME Contacts quit_action: flush). */
  execute_pending (self, TRUE);
  drop_view (self);
  g_clear_object (&self->edit_book);
  g_clear_pointer (&self->edit_books, g_ptr_array_unref);
  g_clear_pointer (&self->select_uid, g_free);
  self->contact_actions = NULL;
  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_CONTACTS_VIEW);
  g_clear_object (&self->stack);
  g_clear_object (&self->book);
  g_clear_object (&self->registry);
  g_clear_object (&self->books);
  g_clear_object (&self->contacts);
  g_clear_object (&self->cancellable);
  g_clear_pointer (&self->by_uid, g_hash_table_unref);
  g_clear_pointer (&self->clients, g_hash_table_unref);
  g_clear_pointer (&self->connecting, g_hash_table_unref);

  G_OBJECT_CLASS (revolution_contacts_view_parent_class)->dispose (object);
}

static void
revolution_contacts_view_class_init (RevolutionContactsViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = revolution_contacts_view_get_property;
  object_class->set_property = revolution_contacts_view_set_property;
  object_class->dispose = revolution_contacts_view_dispose;

  g_object_class_install_property (object_class, PROP_STACK,
                                   g_param_spec_object ("stack", NULL, NULL, ADW_TYPE_VIEW_STACK,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_type_ensure (REVOLUTION_TYPE_CONTACTS_BOOK);
  g_type_ensure (REVOLUTION_TYPE_CONTACTS_CONTACT);
  g_type_ensure (REVOLUTION_TYPE_CONTACTS_DETAIL);
  g_type_ensure (REVOLUTION_TYPE_CONTACTS_EDITOR);

  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/contacts-view.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, book_selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, book_sorted);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, contact_selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, contact_sorted);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, contact_list);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, book_list);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, contact_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, title_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, subtitle_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, banner);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, detail);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, header_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, search_entry);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, search_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, split);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, letter_bar);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, pane_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, editor);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, edit_title);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, done_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, edit_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, delete_button);
  gtk_widget_class_bind_template_callback (widget_class, on_search_toggled);
  gtk_widget_class_bind_template_callback (widget_class, on_search_started);
  gtk_widget_class_bind_template_callback (widget_class, on_search_stop);
  gtk_widget_class_bind_template_callback (widget_class, on_search_changed);
  gtk_widget_class_bind_template_callback (widget_class, on_book_selected);
  gtk_widget_class_bind_template_callback (widget_class, on_contact_selected);
}

static void
revolution_contacts_view_init (RevolutionContactsView *self)
{
  g_autoptr (GSimpleActionGroup) contacts_group = g_simple_action_group_new ();
  g_autoptr (GSimpleActionGroup) contact_group = g_simple_action_group_new ();
  g_autoptr (GPropertyAction) sidebar_action = NULL;
  AdwStyleManager *style_manager = adw_style_manager_get_default ();
  GtkEventController *shortcuts = gtk_shortcut_controller_new ();

  gtk_widget_init_template (GTK_WIDGET (self));

  self->books = g_list_store_new (REVOLUTION_TYPE_CONTACTS_BOOK);
  self->contacts = g_list_store_new (REVOLUTION_TYPE_CONTACTS_CONTACT);
  self->by_uid = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->clients = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  self->connecting = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->cancellable = g_cancellable_new ();
  gtk_sort_list_model_set_model (self->book_sorted, G_LIST_MODEL (self->books));
  g_signal_connect (self, "map", G_CALLBACK (on_map), NULL);

  /* #402 Punkt 4: Buchstabenleiste A–Z, je Buchstabe ein Knopf auf contacts.jump; #368: dazu
   * Alt+Buchstabe (Tippen ohne Alt startet die Suche, key-capture-widget in contacts-view.blp). */
  for (char letter = 'A'; letter <= 'Z'; letter++)
    {
      char text[2] = { letter, '\0' };
      g_autofree char *tooltip = g_strdup_printf (_("Jump to “%s”"), text);
      GtkWidget *button = gtk_button_new_with_label (text);
      GtkShortcut *shortcut = gtk_shortcut_new (gtk_keyval_trigger_new (gdk_unicode_to_keyval (g_ascii_tolower (letter)),
                                                                        GDK_ALT_MASK),
                                                gtk_named_action_new ("contacts.jump"));

      gtk_shortcut_set_arguments (shortcut, g_variant_new_string (text));
      gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (shortcuts), shortcut);

      gtk_actionable_set_action_target (GTK_ACTIONABLE (button), "s", text);
      gtk_actionable_set_action_name (GTK_ACTIONABLE (button), "contacts.jump");
      gtk_widget_set_tooltip_text (button, tooltip);
      gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip, -1);
      gtk_widget_add_css_class (button, "flat");
      gtk_box_append (GTK_BOX (self->letter_bar), button);
    }

  g_action_map_add_action_entries (G_ACTION_MAP (contacts_group), contacts_actions, G_N_ELEMENTS (contacts_actions), self);
  /* contacts.sidebar für F9 wie mail.sidebar (mail-view.c). */
  sidebar_action = g_property_action_new ("sidebar", self->split, "show-sidebar");
  g_action_map_add_action (G_ACTION_MAP (contacts_group), G_ACTION (sidebar_action));
  gtk_widget_insert_action_group (GTK_WIDGET (self), "contacts", G_ACTION_GROUP (contacts_group));
  /* #370: contact.new (Strg+N, +), edit, delete (Entf), save, cancel (Escape); Zustand in update_actions. */
  g_action_map_add_action_entries (G_ACTION_MAP (contact_group), contact_actions, G_N_ELEMENTS (contact_actions), self);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "contact", G_ACTION_GROUP (contact_group));
  self->contact_actions = G_ACTION_MAP (contact_group);
  g_signal_connect_object (self->editor, "notify::valid", G_CALLBACK (on_editor_valid), self, G_CONNECT_SWAPPED);

  gtk_widget_add_controller (GTK_WIDGET (self), shortcuts);

  g_signal_connect_object (style_manager, "notify::dark", G_CALLBACK (on_dark_changed), self, G_CONNECT_DEFAULT);
  on_dark_changed (style_manager, NULL, self);
  update_state (self);
  update_actions (self);
}
