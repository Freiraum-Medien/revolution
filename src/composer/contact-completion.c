/* contact-completion.c – Adressvervollständigung in An/Kopie/Blindkopie (#361)
 *
 * Vorlage: quelltext/stamp/src/views/mail/composer/stamp-contact-completion.c (Popover ohne Fokus mit GtkListView
 * unter dem Feld 566-619, Pfeiltasten und Escape 179-270, Enter 273-292, Abbruch der vorigen Suche bei jeder
 * Eingabe 371-375; Abfrage backend/stamp-account.c:502-509). Stamp zeigt Empfänger als Marken; Revolution behält
 * das Textfeld des Composers (#359) und setzt die Auswahl als „Name <Adresse>, “ ein wie Evolution
 * (completion_match_selected, e-util/e-name-selector-entry.c:1927-1967).
 *
 * Verhalten Evolution 3.56.2 (e-util/e-name-selector-entry.c): Adressbücher sind alle aktivierten mit
 * „Autovervollständigen“ (setup_default_contact_store 2509-2556), gesucht wird ab 3 Zeichen des Abschnitts nach
 * dem letzten Komma (minimum_query_length = 3 in Zeile 3460, GSettings completion-minimum-query-length Vorgabe 3,
 * data/org.gnome.evolution.addressbook.gschema.xml.in:8-12; update_completion_model 1201-1222) mit „contains“ in
 * nickname, email, full_name und file_as (set_completion_query 765-806). Kontaktlisten zählen nicht.
 *
 * libebook nur lesend: Verbinden (e_book_client_connect_sync) und Suchen (e_book_client_get_contacts_sync) je in
 * einem Worker-GTask (Architektur 1); jede Eingabe bricht die laufende Suche ab. Beide Aufrufe sind in EDS 3.56 und
 * 3.62 gleich (Stamp nutzt sie gegen 3.62, stamp-account.c:509, 1303), daher nichts in eds-compat.c.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <string.h>

#include <camel/camel.h>
#include <glib/gi18n.h>
#include <libebook/libebook.h>

#include "contact-completion.h"

#define MINIMUM_QUERY_LENGTH 3
#define MAX_SUGGESTIONS 50
/* Wartezeit auf das Backend wie Stamp (stamp-account.c:1303). */
#define CONNECT_TIMEOUT 10

struct _RevolutionContactCompletion
{
  GObject             parent_instance;

  GPtrArray          *clients;      /* EBookClient, nur im UI-Thread ergänzt */
  GCancellable       *cancellable;  /* Verbinden */
  GCancellable       *query;        /* laufende Suche */
  GtkStringList      *suggestions;  /* „Name <Adresse>“ */
  GtkSingleSelection *selection;
  GtkWidget          *popover;
  GtkWidget          *view;
  GtkEntry           *entry;        /* Feld, an dem das Popover hängt */
  gboolean            inserting;
};

G_DEFINE_FINAL_TYPE (RevolutionContactCompletion, revolution_contact_completion, G_TYPE_OBJECT)

typedef struct
{
  GPtrArray *clients;
  char      *cue;
} Query;

static void
query_free (Query *query)
{
  g_ptr_array_unref (query->clients);
  g_free (query->cue);
  g_free (query);
}

/* Abschnitt nach dem letzten Komma ohne führende Leerzeichen (Evolution get_range_at_position). */
static const char *
cue_start (const char *text)
{
  const char *start = strrchr (text, ',');

  start = start != NULL ? start + 1 : text;
  while (*start == ' ')
    start++;
  return start;
}

static void
connect_thread (GTask        *task,
                gpointer      source_object G_GNUC_UNUSED,
                gpointer      task_data,
                GCancellable *cancellable)
{
  GError *error = NULL;
  EClient *client = e_book_client_connect_sync (task_data, CONNECT_TIMEOUT, cancellable, &error);

  if (client == NULL)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, client, g_object_unref);
}

static void
on_connected (GObject      *source_object,
              GAsyncResult *result,
              gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionContactCompletion *self = REVOLUTION_CONTACT_COMPLETION (source_object);
  ESource *source = g_task_get_task_data (G_TASK (result));
  g_autoptr (GError) error = NULL;
  EClient *client = g_task_propagate_pointer (G_TASK (result), &error);

  if (client == NULL)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Adressvervollständigung: %s nicht verbunden: %s", e_source_get_uid (source), error->message);
      return;
    }
  g_ptr_array_add (self->clients, client);
  g_debug ("Adressvervollständigung: %s verbunden (%u)", e_source_get_uid (source), self->clients->len);
}

static void
query_thread (GTask        *task,
              gpointer      source_object G_GNUC_UNUSED,
              gpointer      task_data,
              GCancellable *cancellable)
{
  Query *query = task_data;
  EBookQuery *tests[] = {
    e_book_query_field_test (E_CONTACT_NICKNAME, E_BOOK_QUERY_CONTAINS, query->cue),
    e_book_query_field_test (E_CONTACT_EMAIL, E_BOOK_QUERY_CONTAINS, query->cue),
    e_book_query_field_test (E_CONTACT_FULL_NAME, E_BOOK_QUERY_CONTAINS, query->cue),
    e_book_query_field_test (E_CONTACT_FILE_AS, E_BOOK_QUERY_CONTAINS, query->cue),
  };
  EBookQuery *any = e_book_query_or (G_N_ELEMENTS (tests), tests, TRUE);
  g_autofree char *sexp = e_book_query_to_string (any);
  g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr (GPtrArray) out = g_ptr_array_new_with_free_func (g_free);

  e_book_query_unref (any);
  for (guint i = 0; i < query->clients->len; i++)
    {
      g_autoptr (GError) error = NULL;
      GSList *contacts = NULL;

      if (!e_book_client_get_contacts_sync (g_ptr_array_index (query->clients, i), sexp, &contacts, cancellable, &error))
        {
          if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            {
              g_task_return_error (task, g_steal_pointer (&error));
              return;
            }
          g_debug ("Adressvervollständigung: Suche in %s: %s",
                   e_source_get_uid (e_client_get_source (g_ptr_array_index (query->clients, i))), error->message);
          continue;
        }
      for (GSList *l = contacts; l != NULL; l = l->next)
        {
          GList *emails;

          if (GPOINTER_TO_INT (e_contact_get (l->data, E_CONTACT_IS_LIST)))
            continue;
          /* Eine Zeile je Adresse, gleiche Adresse aus mehreren Adressbüchern nur einmal. */
          emails = e_contact_get (l->data, E_CONTACT_EMAIL);
          for (GList *e = emails; e != NULL; e = e->next)
            if (g_hash_table_add (seen, g_utf8_casefold (e->data, -1)))
              g_ptr_array_add (out, camel_internet_address_format_address (
                                 e_contact_get_const (l->data, E_CONTACT_FULL_NAME), e->data));
          g_list_free_full (emails, g_free);
        }
      g_slist_free_full (contacts, g_object_unref);
    }
  g_ptr_array_sort_values (out, (GCompareFunc) g_utf8_collate);
  if (out->len > MAX_SUGGESTIONS)
    g_ptr_array_set_size (out, MAX_SUGGESTIONS);
  g_ptr_array_add (out, NULL);
  g_task_return_pointer (task, g_ptr_array_steal (out, NULL), (GDestroyNotify) g_strfreev);
}

static void
on_queried (GObject      *source_object,
            GAsyncResult *result,
            gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionContactCompletion *self = REVOLUTION_CONTACT_COMPLETION (source_object);
  g_auto (GStrv) suggestions = g_task_propagate_pointer (G_TASK (result), NULL);
  guint n;

  if (suggestions == NULL || self->entry == NULL)
    return;
  n = g_strv_length (suggestions);
  gtk_string_list_splice (self->suggestions, 0, g_list_model_get_n_items (G_LIST_MODEL (self->suggestions)),
                          (const char * const *) suggestions);
  g_debug ("Adressvervollständigung: %u Vorschläge aus %u Adressbüchern", n, self->clients->len);
  if (n == 0)
    {
      gtk_popover_popdown (GTK_POPOVER (self->popover));
      return;
    }
  gtk_single_selection_set_selected (self->selection, 0);
  gtk_widget_set_size_request (self->view, gtk_widget_get_width (GTK_WIDGET (self->entry)), -1);
  gtk_popover_popup (GTK_POPOVER (self->popover));
}

static void
popdown (RevolutionContactCompletion *self)
{
  g_cancellable_cancel (self->query);
  g_clear_object (&self->query);
  gtk_popover_popdown (GTK_POPOVER (self->popover));
}

static void
on_changed (GtkEditable                 *editable,
            RevolutionContactCompletion *self)
{
  const char *cue = cue_start (gtk_editable_get_text (editable));
  g_autoptr (GTask) task = NULL;
  Query *query;

  if (self->inserting)
    return;
  popdown (self);
  /* Nur Eingaben des Benutzers: Füllen beim Antworten (Feld ohne Fokus) sucht nicht. */
  if ((gtk_widget_get_state_flags (GTK_WIDGET (editable)) & GTK_STATE_FLAG_FOCUS_WITHIN) == 0 ||
      g_utf8_strlen (cue, -1) < MINIMUM_QUERY_LENGTH || self->clients->len == 0)
    return;
  if (self->entry != GTK_ENTRY (editable))
    {
      if (self->entry != NULL)
        gtk_widget_unparent (self->popover);
      gtk_widget_set_parent (self->popover, GTK_WIDGET (editable));
      self->entry = GTK_ENTRY (editable);
    }

  self->query = g_cancellable_new ();
  query = g_new0 (Query, 1);
  query->clients = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < self->clients->len; i++)
    g_ptr_array_add (query->clients, g_object_ref (g_ptr_array_index (self->clients, i)));
  query->cue = g_strdup (cue);
  task = g_task_new (self, self->query, on_queried, NULL);
  g_task_set_source_tag (task, on_changed);
  g_task_set_task_data (task, query, (GDestroyNotify) query_free);
  g_task_run_in_thread (task, query_thread);
}

static void
insert (RevolutionContactCompletion *self,
        guint                        position)
{
  const char *text = gtk_editable_get_text (GTK_EDITABLE (self->entry));
  const char *address = gtk_string_list_get_string (self->suggestions, position);
  g_autofree char *new_text = NULL;

  if (address == NULL)
    return;
  new_text = g_strdup_printf ("%.*s%s, ", (int) (cue_start (text) - text), text, address);
  self->inserting = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (self->entry), new_text);
  gtk_editable_set_position (GTK_EDITABLE (self->entry), -1);
  self->inserting = FALSE;
  popdown (self);
}

static void
on_activate (GtkListView                 *view G_GNUC_UNUSED,
             guint                        position,
             RevolutionContactCompletion *self)
{
  insert (self, position);
}

static gboolean
on_key_pressed (GtkEventControllerKey       *controller,
                guint                        keyval,
                guint                        keycode G_GNUC_UNUSED,
                GdkModifierType              state,
                RevolutionContactCompletion *self)
{
  guint n = g_list_model_get_n_items (G_LIST_MODEL (self->suggestions));
  guint selected = gtk_single_selection_get_selected (self->selection);

  if (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller)) != GTK_WIDGET (self->entry) ||
      !gtk_widget_get_visible (self->popover) || n == 0 || (state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) != 0)
    return FALSE;
  if (selected >= n)
    selected = 0;

  switch (keyval)
    {
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
      selected = keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down ? (selected + 1) % n : (selected + n - 1) % n;
      gtk_single_selection_set_selected (self->selection, selected);
      gtk_list_view_scroll_to (GTK_LIST_VIEW (self->view), selected, GTK_LIST_SCROLL_NONE, NULL);
      return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter:
      insert (self, selected);
      return TRUE;
    case GDK_KEY_Escape:
      popdown (self);
      return TRUE;
    default:
      return FALSE;
    }
}

static void
on_focus_leave (GtkEventControllerFocus     *controller,
                RevolutionContactCompletion *self)
{
  if (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller)) == GTK_WIDGET (self->entry))
    popdown (self);
}

static void
on_setup (GtkSignalListItemFactory *factory G_GNUC_UNUSED,
          GtkListItem              *item)
{
  GtkWidget *label = gtk_label_new (NULL);

  gtk_label_set_xalign (GTK_LABEL (label), 0);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_list_item_set_child (item, label);
}

static void
on_bind (GtkSignalListItemFactory *factory G_GNUC_UNUSED,
         GtkListItem              *item)
{
  const char *address = gtk_string_object_get_string (gtk_list_item_get_item (item));

  gtk_label_set_text (GTK_LABEL (gtk_list_item_get_child (item)), address);
  gtk_list_item_set_accessible_label (item, address);
}

static void
revolution_contact_completion_dispose (GObject *object)
{
  RevolutionContactCompletion *self = REVOLUTION_CONTACT_COMPLETION (object);

  g_cancellable_cancel (self->cancellable);
  g_cancellable_cancel (self->query);
  g_clear_object (&self->query);
  if (self->entry != NULL)
    gtk_widget_unparent (self->popover);
  self->entry = NULL;
  g_clear_object (&self->popover);

  G_OBJECT_CLASS (revolution_contact_completion_parent_class)->dispose (object);
}

static void
revolution_contact_completion_finalize (GObject *object)
{
  RevolutionContactCompletion *self = REVOLUTION_CONTACT_COMPLETION (object);

  g_clear_object (&self->cancellable);
  g_clear_object (&self->suggestions);
  g_clear_object (&self->selection);
  g_clear_pointer (&self->clients, g_ptr_array_unref);

  G_OBJECT_CLASS (revolution_contact_completion_parent_class)->finalize (object);
}

static void
revolution_contact_completion_class_init (RevolutionContactCompletionClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = revolution_contact_completion_dispose;
  G_OBJECT_CLASS (klass)->finalize = revolution_contact_completion_finalize;
}

static void
revolution_contact_completion_init (RevolutionContactCompletion *self)
{
  g_autoptr (GtkListItemFactory) factory = gtk_signal_list_item_factory_new ();
  GtkWidget *scrolled = gtk_scrolled_window_new ();

  self->clients = g_ptr_array_new_with_free_func (g_object_unref);
  self->cancellable = g_cancellable_new ();
  self->suggestions = gtk_string_list_new (NULL);
  self->selection = gtk_single_selection_new (g_object_ref (G_LIST_MODEL (self->suggestions)));
  g_signal_connect (factory, "setup", G_CALLBACK (on_setup), NULL);
  g_signal_connect (factory, "bind", G_CALLBACK (on_bind), NULL);
  self->view = gtk_list_view_new (g_object_ref (GTK_SELECTION_MODEL (self->selection)), g_object_ref (factory));
  gtk_list_view_set_single_click_activate (GTK_LIST_VIEW (self->view), TRUE);
  /* Ausgewählte Zeile hervorgehoben wie in Seitenleisten (libadwaita-Stilklasse). */
  gtk_widget_add_css_class (self->view, "navigation-sidebar");
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->view), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Address Suggestions"), -1);
  g_signal_connect_object (self->view, "activate", G_CALLBACK (on_activate), self, G_CONNECT_DEFAULT);

  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scrolled), 300);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scrolled), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), self->view);

  /* Ohne Fokus und ohne Autohide wie Stamp (stamp-contact-completion.c:582-587): Tippen bleibt im Feld. */
  self->popover = g_object_ref_sink (gtk_popover_new ());
  gtk_popover_set_position (GTK_POPOVER (self->popover), GTK_POS_BOTTOM);
  gtk_popover_set_has_arrow (GTK_POPOVER (self->popover), FALSE);
  gtk_popover_set_autohide (GTK_POPOVER (self->popover), FALSE);
  gtk_widget_set_can_focus (self->popover, FALSE);
  gtk_widget_set_halign (self->popover, GTK_ALIGN_START);
  gtk_popover_set_child (GTK_POPOVER (self->popover), scrolled);
}

RevolutionContactCompletion *
revolution_contact_completion_new (ESourceRegistry *registry)
{
  RevolutionContactCompletion *self = g_object_new (REVOLUTION_TYPE_CONTACT_COMPLETION, NULL);
  g_autolist (ESource) sources = e_source_registry_list_enabled (registry, E_SOURCE_EXTENSION_ADDRESS_BOOK);

  for (GList *l = sources; l != NULL; l = l->next)
    {
      g_autoptr (GTask) task = NULL;

      if (!e_source_autocomplete_get_include_me (e_source_get_extension (l->data, E_SOURCE_EXTENSION_AUTOCOMPLETE)))
        continue;
      task = g_task_new (self, self->cancellable, on_connected, NULL);
      g_task_set_source_tag (task, revolution_contact_completion_new);
      g_task_set_task_data (task, g_object_ref (l->data), g_object_unref);
      g_task_run_in_thread (task, connect_thread);
    }
  return self;
}

void
revolution_contact_completion_add_entry (RevolutionContactCompletion *self,
                                         GtkEntry                    *entry)
{
  GtkEventController *keys = gtk_event_controller_key_new ();
  GtkEventController *focus = gtk_event_controller_focus_new ();

  /* Vor dem Textfeld (Capture): Pfeile, Enter und Escape gehören bei offener Liste der Auswahl. */
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect_object (keys, "key-pressed", G_CALLBACK (on_key_pressed), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (focus, "leave", G_CALLBACK (on_focus_leave), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller (GTK_WIDGET (entry), keys);
  gtk_widget_add_controller (GTK_WIDGET (entry), focus);
  g_signal_connect_object (entry, "changed", G_CALLBACK (on_changed), self, G_CONNECT_DEFAULT);
  gtk_accessible_update_property (GTK_ACCESSIBLE (entry), GTK_ACCESSIBLE_PROPERTY_AUTOCOMPLETE,
                                  GTK_ACCESSIBLE_AUTOCOMPLETE_LIST, -1);
}
