/* mail-contact-popover.c – Kontakt-Popover im Nachrichtenkopf (#455)
 *
 * Zu einem Namen im Kopf: Adressbuch-Treffer mit Bild, Firma, E-Mail, Telefon und Adresse, sonst Name und
 * E-Mail mit „Kontakt speichern“. Jede Zeile ist die Aktion (Kommentare 1298, 1300 in #455): Composer, tel:,
 * Karte.
 *
 * Suche über alle aktivierten Adressbücher wie Evolutions Bekannt-Prüfung (e-mail-ui-session.c:1342-1390:
 * e_source_registry_list_enabled, Client aus einem Zwischenspeicher, ein Buch mit Fehler zählt nicht) im
 * Worker-GTask (Architektur 1), nach der genauen Adresse wie e_book_client_contains_email_sync, der erste
 * Treffer gilt. libebook nur lesend wie contact-completion.c. Die Verbindungen bleiben für die Laufzeit des
 * Programms bestehen, sonst läge vor jedem Öffnen ein Verbinden je Buch.
 *
 * Latenz: Der Worker läuft, bevor das Popover erscheint. Liegt die Antwort binnen POPUP_DELAY vor, öffnet es
 * fertig und kann nicht springen. Sonst öffnet es mit Name, E-Mail und Kreisel in der unteren Zeile; Firma,
 * Telefon und Adresse gleiten danach auf, die untere Zeile behält ihre Höhe (mail-contact-popover.blp).
 * Schließen und Wechsel der Mail brechen ab (revolution_mail_contact_popover_cancel, mail-header.c).
 *
 * Karte wie Evolution e_open_map_uri (e-misc-utils.c:4497-4540): maps:q=<Ort>, wenn ein Programm das Schema
 * bedient; sonst geo:0,0?q=<Ort> (RFC 5870 kennt q nicht, Android-Konvention); sonst im Browser wie dort
 * https://www.openstreetmap.org/search?query=<Ort>. Fehlt auch dafür ein Programm: Toast.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>
#include <libebook/libebook.h>

#include "contacts/contacts-contact.h"
#include "mail-contact-popover.h"
#include "mail-view.h"

/* Kürzer als die Autovervollständigung (10 s): die Bücher kommen hier nacheinander, das Popover wartet auf alle. */
#define CONNECT_TIMEOUT 2
#define POPUP_DELAY 150

struct _RevolutionMailContactPopover
{
  GtkPopover                 parent_instance;
  AdwAvatar                 *avatar;
  GtkLabel                  *name_label;
  GtkRevealer               *company_revealer;
  GtkLabel                  *company_label;
  GtkWidget                 *mail_row;
  GtkLabel                  *mail_label;
  GtkRevealer               *phone_revealer;
  GtkWidget                 *phone_row;
  GtkLabel                  *phone_label;
  GtkRevealer               *place_revealer;
  GtkWidget                 *place_row;
  GtkLabel                  *place_label;
  GtkStack                  *bottom;
  GCancellable              *cancellable;
  guint                      popup_id;
  char                      *account_uid;
  char                      *name;
  char                      *address;
  char                      *phone;
  char                      *place;
  char                      *map;
  char                      *book_uid;
  RevolutionContactsContact *contact;
  GBinding                  *photo_binding;
};

G_DEFINE_FINAL_TYPE (RevolutionMailContactPopover, revolution_mail_contact_popover, GTK_TYPE_POPOVER)

typedef struct
{
  GPtrArray *sources;
  char      *address;
  gint64     started;
  char      *book_uid;
  EContact  *contact;
} Lookup;

static void
lookup_free (Lookup *lookup)
{
  g_ptr_array_unref (lookup->sources);
  g_free (lookup->address);
  g_free (lookup->book_uid);
  g_clear_object (&lookup->contact);
  g_free (lookup);
}

static GMutex clients_lock;
static GHashTable *clients;  /* Quell-UID → EBookClient, für die Laufzeit des Programms */

/* Backend beendet: Der Client ist tot, die nächste Suche verbindet neu (wie EClientCache, e-client-cache.c:24-29). */
static void
on_backend_died (EClient *client,
                 gpointer user_data)
{
  const char *uid = user_data;

  g_debug ("Kontakt-Popover: Backend %s beendet", uid);
  g_mutex_lock (&clients_lock);
  if (g_hash_table_lookup (clients, uid) == client)
    g_hash_table_remove (clients, uid);
  g_mutex_unlock (&clients_lock);
}

static void
free_uid (gpointer  data,
          GClosure *closure G_GNUC_UNUSED)
{
  g_free (data);
}

static EBookClient *
ref_client (ESource       *source,
            GCancellable  *cancellable,
            GError       **error)
{
  const char *uid = e_source_get_uid (source);
  EBookClient *client;
  EClient *fresh;

  g_mutex_lock (&clients_lock);
  if (clients == NULL)
    clients = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  client = g_hash_table_lookup (clients, uid);
  if (client != NULL)
    g_object_ref (client);
  g_mutex_unlock (&clients_lock);
  if (client != NULL)
    return client;

  fresh = e_book_client_connect_sync (source, CONNECT_TIMEOUT, cancellable, error);
  if (fresh == NULL)
    return NULL;
  g_signal_connect_data (fresh, "backend-died", G_CALLBACK (on_backend_died), g_strdup (uid), free_uid, 0);
  g_mutex_lock (&clients_lock);
  g_hash_table_replace (clients, g_strdup (uid), g_object_ref (fresh));
  g_mutex_unlock (&clients_lock);
  return E_BOOK_CLIENT (fresh);
}

static void
lookup_thread (GTask        *task,
               gpointer      source_object G_GNUC_UNUSED,
               gpointer      task_data,
               GCancellable *cancellable)
{
  Lookup *lookup = task_data;
  EBookQuery *query = e_book_query_field_test (E_CONTACT_EMAIL, E_BOOK_QUERY_IS, lookup->address);
  g_autofree char *sexp = e_book_query_to_string (query);

  e_book_query_unref (query);
  for (guint i = 0; i < lookup->sources->len && lookup->contact == NULL; i++)
    {
      ESource *source = g_ptr_array_index (lookup->sources, i);
      g_autoptr (GError) error = NULL;
      g_autoptr (EBookClient) client = ref_client (source, cancellable, &error);
      GSList *contacts = NULL;

      if (client != NULL)
        e_book_client_get_contacts_sync (client, sexp, &contacts, cancellable, &error);
      if (error != NULL && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_task_return_error (task, g_steal_pointer (&error));
          return;
        }
      if (error != NULL)
        g_debug ("Kontakt-Popover: %s: %s", e_source_get_uid (source), error->message);
      for (GSList *l = contacts; l != NULL && lookup->contact == NULL; l = l->next)
        if (!GPOINTER_TO_INT (e_contact_get (l->data, E_CONTACT_IS_LIST)))
          {
            lookup->contact = g_object_ref (l->data);
            lookup->book_uid = g_strdup (e_source_get_uid (source));
          }
      g_slist_free_full (contacts, g_object_unref);
    }
  g_task_return_boolean (task, TRUE);
}

/* Adresse für Anzeige und Karte, Reihenfolge der Karte wie eab-contact-formatter.c:116-146. */
static char *
postal_text (EContact  *contact,
             char     **map)
{
  static const EContactField fields[] = { E_CONTACT_ADDRESS_WORK, E_CONTACT_ADDRESS_HOME, E_CONTACT_ADDRESS_OTHER };

  for (guint i = 0; i < G_N_ELEMENTS (fields); i++)
    {
      EContactAddress *address = e_contact_get (contact, fields[i]);
      g_autoptr (GStrvBuilder) shown = g_strv_builder_new ();
      g_autoptr (GStrvBuilder) query = g_strv_builder_new ();
      g_auto (GStrv) shown_parts = NULL;
      g_auto (GStrv) query_parts = NULL;

      if (address == NULL)
        continue;
      if (address->street != NULL && *address->street != '\0')
        {
          g_strv_builder_add (shown, address->street);
          g_strv_builder_add (query, address->street);
        }
      if ((address->code != NULL && *address->code != '\0') || (address->locality != NULL && *address->locality != '\0'))
        {
          g_autofree char *city = g_strstrip (g_strdup_printf ("%s %s", address->code != NULL ? address->code : "",
                                                               address->locality != NULL ? address->locality : ""));

          g_strv_builder_add (shown, city);
          if (address->locality != NULL && *address->locality != '\0')
            g_strv_builder_add (query, address->locality);
        }
      if (address->region != NULL && *address->region != '\0')
        {
          g_strv_builder_add (shown, address->region);
          g_strv_builder_add (query, address->region);
        }
      if (address->country != NULL && *address->country != '\0')
        {
          g_strv_builder_add (shown, address->country);
          g_strv_builder_add (query, address->country);
        }
      e_contact_address_free (address);
      shown_parts = g_strv_builder_end (shown);
      query_parts = g_strv_builder_end (query);
      if (shown_parts[0] != NULL)
        {
          *map = g_strjoinv (", ", query_parts);
          return g_strjoinv (", ", shown_parts);
        }
    }
  return NULL;
}

/* Sofort beim Öffnen (animate FALSE) oder aufgleitend, wenn das Popover schon steht. */
static void
reveal (GtkRevealer *revealer,
        gboolean     visible,
        gboolean     animate)
{
  gtk_revealer_set_transition_duration (revealer, animate ? 150 : 0);
  gtk_revealer_set_reveal_child (revealer, visible);
}

static void
show_contact (RevolutionMailContactPopover *self,
              Lookup                       *lookup)
{
  gboolean animate = gtk_widget_get_visible (GTK_WIDGET (self));
  const char *company = NULL;
  const char *name;

  if (lookup->contact != NULL)
    {
      self->contact = revolution_contacts_contact_new (lookup->contact);
      self->book_uid = g_strdup (lookup->book_uid);
      self->photo_binding = g_object_bind_property (self->contact, "photo", self->avatar, "custom-image", G_BINDING_SYNC_CREATE);
      name = e_contact_get_const (lookup->contact, E_CONTACT_FULL_NAME);
      if (name != NULL && *name != '\0')
        {
          adw_avatar_set_text (self->avatar, name);
          gtk_label_set_label (self->name_label, name);
        }
      company = e_contact_get_const (lookup->contact, E_CONTACT_ORG);
      {
        GList *phones = e_contact_get (lookup->contact, E_CONTACT_TEL);

        self->phone = g_strdup (phones != NULL ? phones->data : NULL);
        g_list_free_full (phones, g_free);
      }
      self->place = postal_text (lookup->contact, &self->map);
    }

  gtk_label_set_label (self->company_label, company != NULL ? company : "");
  reveal (self->company_revealer, company != NULL && *company != '\0', animate);
  if (self->phone != NULL)
    {
      g_autofree char *accessible = g_strdup_printf ("%s: %s", _("Call"), self->phone);

      gtk_label_set_label (self->phone_label, self->phone);
      gtk_accessible_update_property (GTK_ACCESSIBLE (self->phone_row), GTK_ACCESSIBLE_PROPERTY_LABEL, accessible, -1);
    }
  reveal (self->phone_revealer, self->phone != NULL, animate);
  if (self->place != NULL)
    {
      g_autofree char *accessible = g_strdup_printf ("%s: %s", _("Navigation"), self->place);

      gtk_label_set_label (self->place_label, self->place);
      gtk_accessible_update_property (GTK_ACCESSIBLE (self->place_row), GTK_ACCESSIBLE_PROPERTY_LABEL, accessible, -1);
    }
  reveal (self->place_revealer, self->place != NULL, animate);
  gtk_stack_set_visible_child_name (self->bottom, lookup->contact != NULL ? "known" : "unknown");
}

/* Der Name kann in den 150 ms verschwunden sein (Ansicht gewechselt): nicht über einer anderen Ansicht öffnen. */
static void
popup_if_shown (RevolutionMailContactPopover *self)
{
  if (gtk_widget_get_mapped (gtk_widget_get_parent (GTK_WIDGET (self))))
    gtk_popover_popup (GTK_POPOVER (self));
  else
    g_signal_emit_by_name (self, "closed");
}

static void
on_found (GObject      *source_object,
          GAsyncResult *result,
          gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionMailContactPopover *self = REVOLUTION_MAIL_CONTACT_POPOVER (source_object);
  Lookup *lookup = g_task_get_task_data (G_TASK (result));
  g_autoptr (GError) error = NULL;

  if (!g_task_propagate_boolean (G_TASK (result), &error))
    return;
  g_clear_object (&self->cancellable);
  g_debug ("Kontakt-Popover: %s %s nach %.0f ms, %s", lookup->address, lookup->contact != NULL ? "gefunden" : "unbekannt",
           (g_get_monotonic_time () - lookup->started) / 1000.0, lookup->book_uid != NULL ? lookup->book_uid : "-");
  show_contact (self, lookup);
  if (self->popup_id != 0)
    {
      g_clear_handle_id (&self->popup_id, g_source_remove);
      popup_if_shown (self);
    }
}

static void
popup_timeout (gpointer user_data)
{
  RevolutionMailContactPopover *self = user_data;

  self->popup_id = 0;
  gtk_stack_set_visible_child_name (self->bottom, "search");
  g_debug ("Kontakt-Popover: Antwort nach %d ms noch offen, Kreisel", POPUP_DELAY);
  popup_if_shown (self);
}

void
revolution_mail_contact_popover_cancel (RevolutionMailContactPopover *self)
{
  g_return_if_fail (REVOLUTION_IS_MAIL_CONTACT_POPOVER (self));

  g_clear_handle_id (&self->popup_id, g_source_remove);
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
}

/* Name und E-Mail stehen sofort, der Rest kommt aus dem Worker (Kopf dieser Datei). registry darf NULL sein:
 * vor dem Laden der Konten gibt es keine Bücher, das Popover zeigt dann den Fall ohne Treffer. */
void
revolution_mail_contact_popover_open (RevolutionMailContactPopover *self,
                                      ESourceRegistry              *registry,
                                      const char                   *account_uid,
                                      const char                   *name,
                                      const char                   *address)
{
  g_autoptr (GTask) task = NULL;
  Lookup *lookup;

  g_return_if_fail (REVOLUTION_IS_MAIL_CONTACT_POPOVER (self));
  g_return_if_fail (address != NULL);

  revolution_mail_contact_popover_cancel (self);
  g_clear_pointer (&self->photo_binding, g_binding_unbind);
  adw_avatar_set_custom_image (self->avatar, NULL);
  g_clear_object (&self->contact);
  g_clear_pointer (&self->phone, g_free);
  g_clear_pointer (&self->place, g_free);
  g_clear_pointer (&self->map, g_free);
  g_clear_pointer (&self->book_uid, g_free);
  g_set_str (&self->account_uid, account_uid);
  g_free (self->name);
  self->name = g_strdup (name);
  g_free (self->address);
  self->address = g_strdup (address);

  adw_avatar_set_text (self->avatar, name);
  gtk_label_set_label (self->name_label, name);
  gtk_label_set_label (self->mail_label, address);
  {
    g_autofree char *accessible = g_strdup_printf ("%s: %s", _("Write Mail"), address);

    gtk_accessible_update_property (GTK_ACCESSIBLE (self->mail_row), GTK_ACCESSIBLE_PROPERTY_LABEL, accessible, -1);
  }
  reveal (self->company_revealer, FALSE, FALSE);
  reveal (self->phone_revealer, FALSE, FALSE);
  reveal (self->place_revealer, FALSE, FALSE);

  lookup = g_new0 (Lookup, 1);
  lookup->sources = g_ptr_array_new_with_free_func (g_object_unref);
  lookup->address = g_strdup (address);
  lookup->started = g_get_monotonic_time ();
  if (registry != NULL)
    {
      GList *sources = e_source_registry_list_enabled (registry, E_SOURCE_EXTENSION_ADDRESS_BOOK);

      for (GList *l = sources; l != NULL; l = l->next)
        g_ptr_array_add (lookup->sources, l->data);
      g_list_free (sources);
    }

  self->cancellable = g_cancellable_new ();
  self->popup_id = g_timeout_add_once (POPUP_DELAY, popup_timeout, self);
  task = g_task_new (self, self->cancellable, on_found, NULL);
  g_task_set_source_tag (task, revolution_mail_contact_popover_open);
  g_task_set_task_data (task, lookup, (GDestroyNotify) lookup_free);
  g_task_run_in_thread (task, lookup_thread);
}

static void
on_closed (RevolutionMailContactPopover *self)
{
  revolution_mail_contact_popover_cancel (self);
}

static RevolutionMailView *
mail_view (RevolutionMailContactPopover *self)
{
  GtkWidget *view = gtk_widget_get_ancestor (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_VIEW);

  return view != NULL ? REVOLUTION_MAIL_VIEW (view) : NULL;
}

static void
on_uri_launched (GObject      *source_object,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  g_autoptr (GtkWidget) parent = user_data;
  g_autoptr (GError) error = NULL;

  if (gtk_uri_launcher_launch_finish (GTK_URI_LAUNCHER (source_object), result, &error) ||
      g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
    return;
  g_debug ("Kontakt-Popover: Start fehlgeschlagen: %s", error->message);
  if (gtk_widget_get_root (parent) != NULL)
    {
      GtkWidget *overlay = gtk_widget_get_ancestor (parent, ADW_TYPE_TOAST_OVERLAY);

      if (overlay != NULL)
        adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (overlay), adw_toast_new (_("Could Not Open the App")));
    }
}

/* Über GtkUriLauncher (auf dem Desktop das Portal OpenURI). Ohne Programm für das Schema gleich der Toast: das Portal
 * zeigte sonst einen Auswahldialog ohne Eintrag. */
static void
open_uri (RevolutionMailContactPopover *self,
          const char                   *scheme,
          const char                   *uri,
          const char                   *missing)
{
  g_autoptr (GAppInfo) app = g_app_info_get_default_for_uri_scheme (scheme);
  GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (self));
  GtkRoot *root = gtk_widget_get_root (parent);

  gtk_popover_popdown (GTK_POPOVER (self));
  g_debug ("Kontakt-Popover: öffnen %s (Programm %s)", uri, app != NULL ? g_app_info_get_id (app) : "-");
  if (app == NULL)
    {
      GtkWidget *overlay = gtk_widget_get_ancestor (parent, ADW_TYPE_TOAST_OVERLAY);

      if (overlay != NULL)
        adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (overlay), adw_toast_new (missing));
      return;
    }
  {
    g_autoptr (GtkUriLauncher) launcher = gtk_uri_launcher_new (uri);

    gtk_uri_launcher_launch (launcher, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL, NULL, on_uri_launched,
                             g_object_ref (parent));
  }
}

static void
on_mail_clicked (RevolutionMailContactPopover *self)
{
  RevolutionMailView *view = mail_view (self);

  gtk_popover_popdown (GTK_POPOVER (self));
  if (view != NULL)
    revolution_mail_view_compose_to (view, self->account_uid, self->name, self->address);
}

/* tel: wie eab-contact-formatter.c:321, ohne Leerzeichen (RFC 3966 kennt nur - . ( ) als Trenner). */
static void
on_phone_clicked (RevolutionMailContactPopover *self)
{
  g_autoptr (GString) uri = g_string_new ("tel:");

  for (const char *p = self->phone; *p != '\0'; p++)
    if (!g_ascii_isspace (*p) && *p != '/')
      g_string_append_c (uri, *p);
  open_uri (self, "tel", uri->str, _("No App for Phone Calls"));
}

static void
on_place_clicked (RevolutionMailContactPopover *self)
{
  g_autofree char *escaped = g_uri_escape_string (self->map, NULL, FALSE);
  g_autoptr (GAppInfo) maps = g_app_info_get_default_for_uri_scheme ("maps");
  g_autoptr (GAppInfo) geo = maps == NULL ? g_app_info_get_default_for_uri_scheme ("geo") : NULL;
  const char *scheme = maps != NULL ? "maps" : geo != NULL ? "geo" : "https";
  const char *prefix = maps != NULL ? "maps:q=" : geo != NULL ? "geo:0,0?q=" : "https://www.openstreetmap.org/search?query=";
  g_autofree char *uri = g_strconcat (prefix, escaped, NULL);

  open_uri (self, scheme, uri, _("No App for Maps"));
}

static void
on_open_clicked (RevolutionMailContactPopover *self)
{
  RevolutionMailView *view = mail_view (self);
  g_autofree char *book = g_strdup (self->book_uid);
  g_autofree char *uid = self->contact != NULL ? g_strdup (revolution_contacts_contact_get_uid (self->contact)) : NULL;

  gtk_popover_popdown (GTK_POPOVER (self));
  if (view != NULL && book != NULL && uid != NULL)
    revolution_mail_view_open_contact (view, book, uid);
}

static void
on_save_clicked (RevolutionMailContactPopover *self)
{
  RevolutionMailView *view = mail_view (self);

  gtk_popover_popdown (GTK_POPOVER (self));
  if (view != NULL)
    revolution_mail_view_save_contact (view, self->name, self->address);
}

static void
revolution_mail_contact_popover_dispose (GObject *object)
{
  RevolutionMailContactPopover *self = REVOLUTION_MAIL_CONTACT_POPOVER (object);

  revolution_mail_contact_popover_cancel (self);
  g_clear_pointer (&self->photo_binding, g_binding_unbind);
  g_clear_object (&self->contact);
  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_CONTACT_POPOVER);

  G_OBJECT_CLASS (revolution_mail_contact_popover_parent_class)->dispose (object);
}

static void
revolution_mail_contact_popover_finalize (GObject *object)
{
  RevolutionMailContactPopover *self = REVOLUTION_MAIL_CONTACT_POPOVER (object);

  g_free (self->account_uid);
  g_free (self->name);
  g_free (self->address);
  g_free (self->phone);
  g_free (self->place);
  g_free (self->map);
  g_free (self->book_uid);

  G_OBJECT_CLASS (revolution_mail_contact_popover_parent_class)->finalize (object);
}

static void
revolution_mail_contact_popover_class_init (RevolutionMailContactPopoverClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = revolution_mail_contact_popover_dispose;
  object_class->finalize = revolution_mail_contact_popover_finalize;

  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/mail-contact-popover.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailContactPopover, avatar);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailContactPopover, name_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailContactPopover, company_revealer);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailContactPopover, company_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailContactPopover, mail_row);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailContactPopover, mail_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailContactPopover, phone_revealer);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailContactPopover, phone_row);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailContactPopover, phone_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailContactPopover, place_revealer);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailContactPopover, place_row);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailContactPopover, place_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailContactPopover, bottom);
  gtk_widget_class_bind_template_callback (widget_class, on_closed);
  gtk_widget_class_bind_template_callback (widget_class, on_mail_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_phone_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_place_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_open_clicked);
  gtk_widget_class_bind_template_callback (widget_class, on_save_clicked);
}

static void
revolution_mail_contact_popover_init (RevolutionMailContactPopover *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
}
