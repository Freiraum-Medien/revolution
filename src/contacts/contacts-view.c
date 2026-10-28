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
 * UI-Thread der Signale bei start_thread. Bücher sind nur lesend angebunden (#370 schreibt).
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>
#include <libebook/libebook.h>

#include "contacts-book.h"
#include "contacts-contact.h"
#include "contacts-view.h"

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
  AdwStatusPage          *contact_status;
  GtkStack               *header_stack;
  GtkSearchEntry         *search_entry;
  GtkToggleButton        *search_button;
  AdwOverlaySplitView    *split;
  GtkWidget              *letter_bar;

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
};

G_DEFINE_FINAL_TYPE (RevolutionContactsView, revolution_contacts_view, ADW_TYPE_BREAKPOINT_BIN)

static void open_view (RevolutionContactsView *self);

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
}

static void
on_objects_modified (EBookClientView        *view,
                     const GSList           *contacts,
                     RevolutionContactsView *self)
{
  guint added = 0, modified = 0;

  if (view != self->view)
    return;
  add_contacts (self, contacts, &added, &modified);
  for (const GSList *l = contacts; l != NULL; l = l->next)
    g_debug ("Kontakte: %s geändert %s „%s“", book_uid (self), (const char *) e_contact_get_const (l->data, E_CONTACT_UID),
             (const char *) e_contact_get_const (l->data, E_CONTACT_FULL_NAME));
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
  g_hash_table_remove_all (self->by_uid);
  gtk_sort_list_model_set_model (self->contact_sorted, NULL);
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
  g_debug ("Kontakte: %s verbunden, nur lesend %s, do-initial-query %s", uid,
           e_client_is_readonly (client) ? "ja" : "nein",
           e_client_check_capability (client, "do-initial-query") ? "ja" : "nein");
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
  open_view (self);
  if (self->connect_all)
    connect_book (self, book);
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

  for (guint i = 0; i < g_list_model_get_n_items (sorted); i++)
    {
      g_autoptr (RevolutionContactsBook) book = g_list_model_get_item (sorted, i);
      ESource *source = revolution_contacts_book_get_source (book);

      g_debug ("Adressbuch: %u %s %s / %s", i, e_source_get_uid (source), revolution_contacts_book_get_group (book),
               e_source_get_display_name (source));
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
  for (guint i = 0; i < g_list_model_get_n_items (books); i++)
    {
      g_autoptr (RevolutionContactsBook) book = g_list_model_get_item (books, i);

      connect_book (self, book);
    }
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
    g_signal_connect_object (registry, signals[i], G_CALLBACK (sync_books), self, G_CONNECT_SWAPPED);
  sync_books (self);
  if (gtk_widget_get_mapped (GTK_WIDGET (self)))
    on_map (self, NULL);
}

/* ------------------------------------------------------------------ Liste, Suche */

static void
on_contact_selected (RevolutionContactsView *self)
{
  RevolutionContactsContact *item = gtk_single_selection_get_selected_item (self->contact_selection);

  /* Detailansicht folgt in #369; bis dahin Name im Leerzustand. */
  adw_status_page_set_title (self->contact_status, item != NULL ? revolution_contacts_contact_get_name (item)
                                                                 : _("No Contact Selected"));
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
  drop_view (self);
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
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, contact_status);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, header_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, search_entry);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, search_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, split);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, letter_bar);
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

  gtk_widget_add_controller (GTK_WIDGET (self), shortcuts);

  g_signal_connect_object (style_manager, "notify::dark", G_CALLBACK (on_dark_changed), self, G_CONNECT_DEFAULT);
  on_dark_changed (style_manager, NULL, self);
  update_state (self);
}
