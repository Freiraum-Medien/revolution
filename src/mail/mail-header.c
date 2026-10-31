/* mail-header.c – Nachrichtenkopf für Einzelansicht und Konversation (#455)
 *
 * Ein Kopf für beide Ansichten: mail-reader.c hat einen eigenen, in der Konversation gehört er zum Eintrag
 * (mail-conversation.c), auch zugeklappt, wenn es keinen Lesebereich gibt. Dort gilt set_compact (Bild 32, keine
 * Ränder, ganze Fläche klappt auf und zu) und zugeklappt set_collapsed (Vorschau statt Betreff, keine Empfänger).
 *
 * Namen: je Name ein GtkButton in einer AdwWrapBox statt Links in einem GtkLabel: gtklabel.h kennt kein Rechteck eines
 * Links (Anker fürs Popover), kein a11y-Label je Link, keinen Fokus oder Rechtsklick auf einen einzelnen Link. Das
 * a11y-Label ist „Name, Adresse“, die Adresse steht sonst nur im Popover (Soll 2). Eingeklappt höchstens drei Namen je
 * Zeile, dann „und N weitere“ (Kommentare 1293, 1295).
 *
 * Verhalten wie Evolutions Kontextmenü auf Adressen im Kopf (mail/e-mail-display.c:1938-1951, 2056-2084:
 * „Add to Address Book“, „Send Reply To“, Suchordner; e-util/e-web-view.c:1586-1587, 1620-1633: Adresse
 * kopieren). Suchordner gibt es hier nicht: die Suche aus #365 kennt nur „Nachricht enthält“ über Betreff,
 * Absender, Empfänger und Text (mail-search.c, revolution_mail_search_expression), nichts nur für den Absender.
 *
 * Warnung in „Details“ (Kommentare 1293, 1295 in #455), erst beim ersten Öffnen je Nachricht: multipart/signed prüfen
 * wie Evolution (em-format/e-mail-parser-multipart-signed.c:133-157), Warnung bei CAMEL_CIPHER_VALIDITY_SIGN_BAD
 * (e-mail-part-utils.c:154, in Evolution „Invalid signature“, e-mail-formatter-secure-button.c:58) und bei einem Fehler
 * der Prüfung (kaputter Signaturblock, dort e-mail-parser-multipart-signed.c:160-172). Fehlender Schlüssel, unbekannt
 * oder fremdes Protokoll warnen nicht. Im Worker-GTask aus dem Cache des Stores, der Wechsel der Mail bricht ab.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>

#include "mail-contact-popover.h"
#include "mail-header.h"
#include "mail-view.h"

/* Namen je Zeile bei eingeklapptem „Details“ (Kommentar 1293). */
#define COLLAPSED_NAMES 3

struct _RevolutionMailHeader
{
  GtkWidget             parent_instance;
  GtkWidget            *grid;
  AdwAvatar            *avatar;
  GtkLabel             *sender_label;
  GtkImage             *folder_icon;
  GtkLabel             *folder_label;
  GtkLabel             *date_label;
  GtkLabel             *subject_label;
  GtkLabel             *preview_label;
  GtkWidget            *to_row;
  GtkWidget            *to_caption;
  AdwWrapBox           *to_box;
  GtkWidget            *details_tail;
  GtkToggleButton      *details_button;
  GtkWidget            *cc_row;
  AdwWrapBox           *cc_box;
  GtkRevealer          *details_revealer;
  GtkWidget            *bcc_row;
  AdwWrapBox           *bcc_box;
  AdwWrapBox           *from_box;
  GtkLabel             *date_full;
  GtkWidget            *signature_row;
  CamelSession         *session;
  char                 *store_uid;
  char                 *folder_name;
  char                 *message_uid;
  char                 *account_uid;
  GCancellable         *check;
  gboolean              checked;
  gboolean              pressed;
  GdkEventSequence     *press_sequence;
  double                press_x;
  double                press_y;
  guint32               release_time;
  CamelInternetAddress *from;
  CamelInternetAddress *to;
  CamelInternetAddress *cc;
  CamelInternetAddress *bcc;
  GtkWidget            *popover;
  GtkWidget            *menu_popover;
  GSimpleActionGroup   *actions;
  /* Name mit offenem Popover oder Menü, schwach: der Neuaufbau der Namen kann ihn entfernen. */
  GtkWidget            *active_name;
  char                 *menu_name;
  char                 *menu_address;
  gboolean              compact;
  gboolean              collapsed;
};

G_DEFINE_FINAL_TYPE (RevolutionMailHeader, revolution_mail_header, GTK_TYPE_WIDGET)

enum { SIGNAL_ACTIVATED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* Anzeigename, sonst die Adresse. Ein Name in spitzen Klammern gilt als kein Name (microsoft365,
 * camel-m365-folder.c:787-800, wie mail-message.c). */
static char *
shown_name (const char *name,
            const char *email)
{
  g_auto (GStrv) words = g_strsplit (name != NULL ? name : "", " ", -1);
  g_autoptr (GString) bare = g_string_new (NULL);

  /* Doppelte Leerzeichen aus dem Exchange-Adressbuch nicht mitnehmen („Martha  Tscheliesnig“). */
  for (guint i = 0; words[i] != NULL; i++)
    if (*words[i] != '\0')
      g_string_append_printf (bare, "%s%s", bare->len > 0 ? " " : "", words[i]);
  if (bare->str[0] == '<' || (email != NULL && g_ascii_strcasecmp (bare->str, email) == 0))
    g_string_truncate (bare, 0);
  return g_strdup (bare->len > 0 ? bare->str : (email != NULL ? email : ""));
}

/* Popover und Menü hängen am Kopf (bleibt stehen, Namen nicht), der Pfeil zeigt auf den Namen. */
static void
point_at (RevolutionMailHeader *self,
          GtkPopover           *popover,
          GtkWidget            *name)
{
  graphene_rect_t bounds;

  if (gtk_widget_compute_bounds (name, GTK_WIDGET (self), &bounds))
    gtk_popover_set_pointing_to (popover, &(GdkRectangle) { (int) bounds.origin.x, (int) bounds.origin.y,
                                                            (int) bounds.size.width, (int) bounds.size.height });
}

static void
set_active_name (RevolutionMailHeader *self,
                 GtkWidget            *name)
{
  if (self->active_name != NULL)
    gtk_widget_remove_css_class (self->active_name, "open");
  g_set_weak_pointer (&self->active_name, name);
  if (name != NULL)
    gtk_widget_add_css_class (name, "open");
}

static gboolean
focus_name (gpointer user_data)
{
  if (gtk_widget_get_mapped (user_data))
    gtk_widget_grab_focus (user_data);
  return G_SOURCE_REMOVE;
}

/* Fokus zurück auf den Namen erst nach „closed“: GTK gibt ihn danach dem Elternwidget des Popovers (gemessen, c9:
 * direkt nach dem Grab auf dem Namen, in der nächsten Runde der Hauptschleife auf dem Kopf). In der Konversation ist
 * der Kopf fokussierbar, die nächste Leertaste klappte die Nachricht zu. */
static void
on_popover_closed (RevolutionMailHeader *self)
{
  GtkWidget *name = self->active_name;

  set_active_name (self, NULL);
  if (name != NULL)
    g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, focus_name, g_object_ref (name), g_object_unref);
}

static void
on_name_clicked (GtkButton            *button,
                 RevolutionMailHeader *self)
{
  GtkWidget *view = gtk_widget_get_ancestor (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_VIEW);

  gtk_popover_popdown (GTK_POPOVER (self->popover));
  set_active_name (self, GTK_WIDGET (button));
  point_at (self, GTK_POPOVER (self->popover), GTK_WIDGET (button));
  revolution_mail_contact_popover_open (REVOLUTION_MAIL_CONTACT_POPOVER (self->popover),
                                        view != NULL ? revolution_mail_view_get_registry (REVOLUTION_MAIL_VIEW (view)) : NULL,
                                        self->account_uid,
                                        g_object_get_data (G_OBJECT (button), "name"),
                                        g_object_get_data (G_OBJECT (button), "address"));
}

static void
show_menu (RevolutionMailHeader *self,
           GtkWidget            *button)
{
  g_autoptr (GMenu) menu = g_menu_new ();
  g_autoptr (GString) shown = NULL;
  g_autofree char *compose = NULL;

  g_free (self->menu_name);
  self->menu_name = g_strdup (g_object_get_data (G_OBJECT (button), "name"));
  g_free (self->menu_address);
  self->menu_address = g_strdup (g_object_get_data (G_OBJECT (button), "address"));
  /* Menütexte werten „_“ als Tastenkürzel, Adressen enthalten es als Zeichen. */
  shown = g_string_new (self->menu_name);
  g_string_replace (shown, "_", "__", 0);
  /* Translators: context menu of a name in the message header, %s is the name. */
  compose = g_strdup_printf (_("New Message to %s"), shown->str);
  g_menu_append (menu, compose, "header.compose");
  g_menu_append (menu, _("Save Contact"), "header.save-contact");
  g_menu_append (menu, _("Copy Address"), "header.copy-address");
  gtk_popover_menu_set_menu_model (GTK_POPOVER_MENU (self->menu_popover), G_MENU_MODEL (menu));
  set_active_name (self, button);
  point_at (self, GTK_POPOVER (self->menu_popover), button);
  gtk_popover_popup (GTK_POPOVER (self->menu_popover));
}

static void
on_name_pressed (GtkGestureClick *gesture,
                 int              n_press G_GNUC_UNUSED,
                 double           x G_GNUC_UNUSED,
                 double           y G_GNUC_UNUSED,
                 gpointer         user_data)
{
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  show_menu (user_data, gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture)));
}

static gboolean
on_name_menu_key (GtkWidget *widget,
                  GVariant  *args G_GNUC_UNUSED,
                  gpointer   user_data)
{
  show_menu (user_data, widget);
  return TRUE;
}

static void
on_compose (GSimpleAction *action G_GNUC_UNUSED,
            GVariant      *parameter G_GNUC_UNUSED,
            gpointer       user_data)
{
  RevolutionMailHeader *self = user_data;
  GtkWidget *view = gtk_widget_get_ancestor (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_VIEW);

  if (view != NULL)
    revolution_mail_view_compose_to (REVOLUTION_MAIL_VIEW (view), self->account_uid, self->menu_name, self->menu_address);
}

static void
on_save_contact (GSimpleAction *action G_GNUC_UNUSED,
                 GVariant      *parameter G_GNUC_UNUSED,
                 gpointer       user_data)
{
  RevolutionMailHeader *self = user_data;
  GtkWidget *view = gtk_widget_get_ancestor (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_VIEW);

  if (view != NULL)
    revolution_mail_view_save_contact (REVOLUTION_MAIL_VIEW (view), self->menu_name, self->menu_address);
}

static void
on_copy_address (GSimpleAction *action G_GNUC_UNUSED,
                 GVariant      *parameter G_GNUC_UNUSED,
                 gpointer       user_data)
{
  RevolutionMailHeader *self = user_data;

  gdk_clipboard_set_text (gtk_widget_get_clipboard (GTK_WIDGET (self)), self->menu_address);
}

static const GActionEntry header_actions[] = {
  { "compose", on_compose, NULL, NULL, NULL, { 0 } },
  { "save-contact", on_save_contact, NULL, NULL, NULL, { 0 } },
  { "copy-address", on_copy_address, NULL, NULL, NULL, { 0 } },
};

/* Name als Textlink, dahinter das Komma außerhalb des Knopfs (Unterstreichung nur unter dem Namen). */
static GtkWidget *
name_new (RevolutionMailHeader *self,
          const char           *name,
          const char           *email,
          gboolean              comma)
{
  GtkWidget *item = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *button = gtk_button_new ();
  GtkWidget *label = gtk_label_new (name);
  GtkGesture *menu_click = gtk_gesture_click_new ();
  GtkEventController *keys = gtk_shortcut_controller_new ();
  g_autofree char *accessible = g_ascii_strcasecmp (name, email) != 0 ? g_strdup_printf ("%s, %s", name, email)
                                                                      : g_strdup (email);

  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
  gtk_button_set_child (GTK_BUTTON (button), label);
  gtk_widget_add_css_class (button, "mail-name");
  gtk_widget_set_cursor_from_name (button, "pointer");
  gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, accessible, -1);
  g_object_set_data_full (G_OBJECT (button), "name", g_strdup (name), g_free);
  g_object_set_data_full (G_OBJECT (button), "address", g_strdup (email), g_free);
  g_signal_connect (button, "clicked", G_CALLBACK (on_name_clicked), self);

  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (menu_click), GDK_BUTTON_SECONDARY);
  g_signal_connect (menu_click, "pressed", G_CALLBACK (on_name_pressed), self);
  gtk_widget_add_controller (button, GTK_EVENT_CONTROLLER (menu_click));
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (keys),
                                        gtk_shortcut_new (gtk_shortcut_trigger_parse_string ("<Shift>F10|Menu"),
                                                          gtk_callback_action_new (on_name_menu_key, self, NULL)));
  gtk_widget_add_controller (button, keys);

  gtk_box_append (GTK_BOX (item), button);
  if (comma)
    {
      GtkWidget *separator = gtk_label_new (",");

      gtk_widget_add_css_class (separator, "dimmed");
      gtk_accessible_update_state (GTK_ACCESSIBLE (separator), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
      gtk_box_append (GTK_BOX (item), separator);
    }
  return item;
}

/* Namen vor tail (bleibt stehen) neu aufbauen: höchstens limit, dann „und N weitere“. */
static void
fill_names (RevolutionMailHeader *self,
            AdwWrapBox           *box,
            CamelInternetAddress *addresses,
            GtkWidget            *tail,
            guint                 limit)
{
  guint total = addresses != NULL ? camel_address_length (CAMEL_ADDRESS (addresses)) : 0;
  guint shown = MIN (total, limit);
  GtkWidget *previous = NULL;
  GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (box));

  while (child != NULL)
    {
      GtkWidget *next = gtk_widget_get_next_sibling (child);

      if (child != tail)
        adw_wrap_box_remove (box, child);
      child = next;
    }

  for (guint i = 0; i < shown; i++)
    {
      const char *name = NULL, *email = NULL;
      g_autofree char *text = NULL;
      GtkWidget *item;

      if (!camel_internet_address_get (addresses, i, &name, &email) || email == NULL)
        continue;
      text = shown_name (name, email);
      item = name_new (self, text, email, i + 1 < shown);
      adw_wrap_box_insert_child_after (box, item, previous);
      previous = item;
    }

  if (total > shown)
    {
      guint rest = total - shown;
      /* Translators: end of the recipients line in the message header, after the last name, e.g. “and 5 more”. */
      g_autofree char *more = g_strdup_printf (ngettext ("and %u more", "and %u more", rest), rest);
      GtkWidget *label = gtk_label_new (more);

      gtk_widget_add_css_class (label, "dimmed");
      adw_wrap_box_insert_child_after (box, label, previous);
    }
}

static gboolean
has_addresses (CamelInternetAddress *addresses)
{
  return addresses != NULL && camel_address_length (CAMEL_ADDRESS (addresses)) > 0;
}

/* Zeilen nach den Adressen und „Details“. Offen nimmt An und Kopie vollständig in ihre Zeilen, Blindkopie
 * und Von nur offen, sonst bliebe je Empfänger ein unsichtbarer Knopf liegen. */
static void
update_names (RevolutionMailHeader *self)
{
  gboolean open = gtk_toggle_button_get_active (self->details_button);
  guint limit = open ? G_MAXUINT : COLLAPSED_NAMES;

  fill_names (self, self->to_box, self->to, self->details_tail, limit);
  fill_names (self, self->cc_box, self->cc, NULL, limit);
  fill_names (self, self->bcc_box, open ? self->bcc : NULL, NULL, limit);
  fill_names (self, self->from_box, open ? self->from : NULL, NULL, limit);
  gtk_widget_set_visible (self->to_caption, has_addresses (self->to));
  gtk_widget_set_visible (self->cc_row, !self->collapsed && has_addresses (self->cc));
  gtk_widget_set_visible (self->bcc_row, has_addresses (self->bcc));
}

typedef struct
{
  CamelSession *session;
  char         *store_uid;
  char         *folder_name;
  char         *message_uid;
} Check;

static void
check_free (Check *check)
{
  g_object_unref (check->session);
  g_free (check->store_uid);
  g_free (check->folder_name);
  g_free (check->message_uid);
  g_free (check);
}

/* Kontext je Protokoll wie e-mail-parser-multipart-signed.c:133-146, NULL bei fremdem Protokoll. */
static CamelCipherContext *
cipher_for (CamelSession     *session,
            CamelDataWrapper *signed_part)
{
  CamelContentType *type = camel_data_wrapper_get_mime_type_field (signed_part);
  const char *protocol = type != NULL ? camel_content_type_param (type, "protocol") : NULL;

  if (protocol == NULL)
    return NULL;
  if (g_ascii_strcasecmp (protocol, "application/pkcs7-signature") == 0 ||
      g_ascii_strcasecmp (protocol, "application/xpkcs7signature") == 0 ||
      g_ascii_strcasecmp (protocol, "application/xpkcs7-signature") == 0 ||
      g_ascii_strcasecmp (protocol, "application/x-pkcs7-signature") == 0)
    return camel_smime_context_new (session);
  if (g_ascii_strcasecmp (protocol, "application/pgp-signature") == 0)
    return camel_gpg_context_new (session);
  return NULL;
}

/* Alle Teile bis auf angehängte Nachrichten; TRUE, sobald eine Signatur als ungültig gilt. */
static gboolean
has_bad_signature (CamelSession  *session,
                   CamelMimePart *part,
                   GCancellable  *cancellable)
{
  CamelDataWrapper *content = camel_medium_get_content (CAMEL_MEDIUM (part));

  if (CAMEL_IS_MULTIPART_SIGNED (content))
    {
      g_autoptr (CamelCipherContext) cipher = cipher_for (session, content);
      g_autoptr (GError) error = NULL;
      CamelCipherValidity *validity = cipher != NULL ? camel_cipher_context_verify_sync (cipher, part, cancellable, &error) : NULL;

      if (validity != NULL)
        {
          gboolean bad = validity->sign.status == CAMEL_CIPHER_VALIDITY_SIGN_BAD;

          g_debug ("Kopf: Signatur Status %d", validity->sign.status);
          camel_cipher_validity_free (validity);
          if (bad)
            return TRUE;
        }
      else if (cipher == NULL)
        g_debug ("Kopf: Signatur nicht geprüft: Protokoll unbekannt");
      else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          /* Kaputter Signaturblock: Evolution zeigt „Error verifying signature“, nicht unsigniert. */
          g_debug ("Kopf: Signatur Fehler: %s", error != NULL ? error->message : "ohne Meldung");
          return TRUE;
        }
    }
  if (CAMEL_IS_MULTIPART (content))
    for (guint i = 0; i < camel_multipart_get_number (CAMEL_MULTIPART (content)); i++)
      if (has_bad_signature (session, camel_multipart_get_part (CAMEL_MULTIPART (content), i), cancellable))
        return TRUE;
  return FALSE;
}

static void
check_thread (GTask        *task,
              gpointer      source_object G_GNUC_UNUSED,
              gpointer      task_data,
              GCancellable *cancellable)
{
  Check *check = task_data;
  g_autoptr (CamelService) service = camel_session_ref_service (check->session, check->store_uid);
  g_autoptr (CamelFolder) folder = NULL;
  g_autoptr (CamelMimeMessage) message = NULL;
  GError *error = NULL;

  /* Ordner immer über camel_store_get_folder_sync (docs/ENTWICKLUNG.md „Camel-Ordner“). */
  if (CAMEL_IS_STORE (service))
    folder = camel_store_get_folder_sync (CAMEL_STORE (service), check->folder_name, 0, cancellable, &error);
  if (folder != NULL)
    message = camel_folder_get_message_sync (folder, check->message_uid, cancellable, &error);
  if (message == NULL)
    {
      g_task_return_error (task, error != NULL ? error : g_error_new_literal (CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE,
                                                                              check->store_uid));
      return;
    }
  g_task_return_boolean (task, has_bad_signature (check->session, CAMEL_MIME_PART (message), cancellable));
}

static void
on_checked (GObject      *source_object,
            GAsyncResult *result,
            gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionMailHeader *self = REVOLUTION_MAIL_HEADER (source_object);
  g_autoptr (GError) error = NULL;
  gboolean bad = g_task_propagate_boolean (G_TASK (result), &error);

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;
  if (error != NULL)
    g_debug ("Kopf: Signatur nicht geprüft: %s", error->message);
  g_clear_object (&self->check);
  gtk_widget_set_visible (self->signature_row, bad);
}

/* Einmal je Nachricht, sobald „Details“ offen ist und die Adressen da sind. */
static void
check_warnings (RevolutionMailHeader *self)
{
  g_autoptr (GTask) task = NULL;
  Check *check;

  if (self->checked || !gtk_toggle_button_get_active (self->details_button) || self->from == NULL || self->session == NULL)
    return;
  self->checked = TRUE;

  check = g_new0 (Check, 1);
  check->session = g_object_ref (self->session);
  check->store_uid = g_strdup (self->store_uid);
  check->folder_name = g_strdup (self->folder_name);
  check->message_uid = g_strdup (self->message_uid);
  self->check = g_cancellable_new ();
  task = g_task_new (self, self->check, on_checked, NULL);
  g_task_set_source_tag (task, check_warnings);
  g_task_set_task_data (task, check, (GDestroyNotify) check_free);
  g_task_run_in_thread (task, check_thread);
}

static void
on_details_toggled (RevolutionMailHeader *self)
{
  update_names (self);
  gtk_revealer_set_reveal_child (self->details_revealer, gtk_toggle_button_get_active (self->details_button));
  check_warnings (self);
}

/* Datum im Kopf wie der Prototyp, „Heute, 13:46 Uhr“ (Kommentar 1296); die Einheit nur über die Übersetzung. */
static char *
header_date (GDateTime *local)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autoptr (GDate) day = g_date_new_dmy (g_date_time_get_day_of_month (local), g_date_time_get_month (local),
                                          g_date_time_get_year (local));
  g_autoptr (GDate) today = g_date_new_dmy (g_date_time_get_day_of_month (now), g_date_time_get_month (now),
                                            g_date_time_get_year (now));
  int days = g_date_days_between (day, today);
  g_autofree char *time = g_date_time_format (local, "%H:%M");
  /* Translators: time of day in the message header, %s is e.g. “13:46”; German “%s Uhr”. */
  g_autofree char *clock = g_strdup_printf (C_("message header", "%s"), time);
  g_autofree char *date = NULL;

  if (days == 0)
    /* Translators: date in the message header, %s is the time. */
    return g_strdup_printf (_("Today, %s"), clock);
  if (days == 1)
    /* Translators: date in the message header, %s is the time. */
    return g_strdup_printf (_("Yesterday, %s"), clock);

  /* Translators: date in the message header (g_date_time_format), e.g. “10/17/2026”. */
  date = g_date_time_format (local, _("%m/%d/%Y"));
  return g_strdup_printf ("%s, %s", date, clock);
}

/* Liste der Nachricht: Name, Bild, Betreff, Ort. */
void
revolution_mail_header_set_summary (RevolutionMailHeader *self,
                                    const char           *sender,
                                    const char           *avatar,
                                    const char           *subject,
                                    const char           *icon,
                                    const char           *place)
{
  g_return_if_fail (REVOLUTION_IS_MAIL_HEADER (self));

  adw_avatar_set_text (self->avatar, avatar != NULL ? avatar : sender);
  gtk_label_set_label (self->sender_label, sender);
  gtk_label_set_label (self->subject_label, subject);
  gtk_image_set_from_icon_name (self->folder_icon, icon);
  gtk_widget_set_visible (GTK_WIDGET (self->folder_icon), icon != NULL);
  gtk_label_set_label (self->folder_label, place);
  gtk_widget_set_visible (GTK_WIDGET (self->folder_label), place != NULL);
}

const char *
revolution_mail_header_get_subject (RevolutionMailHeader *self)
{
  g_return_val_if_fail (REVOLUTION_IS_MAIL_HEADER (self), NULL);

  return gtk_label_get_label (self->subject_label);
}

/* date: Unix-Zeit, 0 ohne Datum. */
void
revolution_mail_header_set_date (RevolutionMailHeader *self,
                                 gint64                date)
{
  g_autoptr (GDateTime) local = NULL;
  g_autofree char *shown = NULL;
  g_autofree char *full = NULL;

  g_return_if_fail (REVOLUTION_IS_MAIL_HEADER (self));

  local = date > 0 ? g_date_time_new_from_unix_local (date) : NULL;
  shown = local != NULL ? header_date (local) : g_strdup ("");
  /* Translators: full date in the details (g_date_time_format). */
  full = local != NULL ? g_date_time_format (local, _("%A, %B %-d, %Y, %H:%M:%S")) : g_strdup ("");
  gtk_label_set_label (self->date_label, shown);
  gtk_label_set_label (self->date_full, full);
}

/* Woher die Nachricht stammt: store_uid, folder_name und message_uid wie geladen (auch Suchordner), account_uid das
 * Konto des echten Ordners (Absender neuer Mails). */
void
revolution_mail_header_set_source (RevolutionMailHeader *self,
                                   CamelSession         *session,
                                   const char           *store_uid,
                                   const char           *folder_name,
                                   const char           *message_uid,
                                   const char           *account_uid)
{
  g_return_if_fail (REVOLUTION_IS_MAIL_HEADER (self));

  g_set_object (&self->session, session);
  g_set_str (&self->store_uid, store_uid);
  g_set_str (&self->folder_name, folder_name);
  g_set_str (&self->message_uid, message_uid);
  g_set_str (&self->account_uid, account_uid);
}

/* Empfänger aus der geladenen Nachricht (im UI-Thread aufrufen; Adressen nur lesen). */
void
revolution_mail_header_set_addresses (RevolutionMailHeader *self,
                                      CamelInternetAddress *from,
                                      CamelInternetAddress *to,
                                      CamelInternetAddress *cc,
                                      CamelInternetAddress *bcc)
{
  g_return_if_fail (REVOLUTION_IS_MAIL_HEADER (self));

  g_set_object (&self->from, from);
  g_set_object (&self->to, to);
  g_set_object (&self->cc, cc);
  g_set_object (&self->bcc, bcc);
  update_names (self);
  check_warnings (self);
}

/* Wechsel der Mail: Suche abbrechen, Popover und Menü schließen, Empfänger leeren. Die Zeile „Kopie:“ bleibt bis zur
 * neuen Nachricht stehen: So springt der Inhalt nicht, solange die Nachricht lädt. */
void
revolution_mail_header_clear (RevolutionMailHeader *self)
{
  gboolean cc;

  g_return_if_fail (REVOLUTION_IS_MAIL_HEADER (self));

  cc = gtk_widget_get_visible (self->cc_row);
  revolution_mail_contact_popover_cancel (REVOLUTION_MAIL_CONTACT_POPOVER (self->popover));
  gtk_popover_popdown (GTK_POPOVER (self->popover));
  gtk_popover_popdown (GTK_POPOVER (self->menu_popover));
  set_active_name (self, NULL);
  g_cancellable_cancel (self->check);
  g_clear_object (&self->check);
  self->checked = FALSE;
  gtk_widget_set_visible (self->signature_row, FALSE);
  g_clear_object (&self->from);
  g_clear_object (&self->to);
  g_clear_object (&self->cc);
  g_clear_object (&self->bcc);
  update_names (self);
  gtk_widget_set_visible (self->cc_row, cc);
  gtk_widget_set_visible (self->to_caption, TRUE);
  revolution_mail_header_set_date (self, 0);
}

/* Darstellung im Eintrag der Konversation (mail-conversation.c): Bild 32 statt 48, Ränder am Eintrag, die Fläche ist
 * zugleich der Knopf zum Auf- und Zuklappen (Tastatur: Eingabe, Leertaste). */
void
revolution_mail_header_set_compact (RevolutionMailHeader *self,
                                    gboolean              compact)
{
  g_return_if_fail (REVOLUTION_IS_MAIL_HEADER (self));

  self->compact = compact;
  adw_avatar_set_size (self->avatar, compact ? 32 : 48);
  gtk_widget_set_margin_start (self->grid, compact ? 0 : 24);
  gtk_widget_set_margin_end (self->grid, compact ? 0 : 18);
  gtk_widget_set_margin_top (self->grid, compact ? 0 : 14);
  gtk_widget_set_margin_bottom (self->grid, compact ? 0 : 14);
  gtk_widget_set_focusable (GTK_WIDGET (self), compact);
  if (compact)
    gtk_widget_add_css_class (GTK_WIDGET (self), "conversation-header");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "conversation-header");
}

/* Zugeklappt in der Konversation: Vorschau statt Betreff, keine Empfänger, nichts auswählbar (Klick klappt auf). */
void
revolution_mail_header_set_collapsed (RevolutionMailHeader *self,
                                      gboolean              collapsed,
                                      const char           *preview)
{
  g_return_if_fail (REVOLUTION_IS_MAIL_HEADER (self));

  self->collapsed = collapsed;
  gtk_label_set_label (self->preview_label, preview != NULL ? preview : "");
  gtk_widget_set_visible (GTK_WIDGET (self->preview_label), collapsed);
  gtk_widget_set_visible (GTK_WIDGET (self->subject_label), !collapsed);
  gtk_widget_set_visible (self->to_row, !collapsed);
  gtk_widget_set_visible (self->cc_row, !collapsed && has_addresses (self->cc));
  gtk_widget_set_visible (GTK_WIDGET (self->details_revealer), !collapsed);
  gtk_label_set_selectable (self->sender_label, !collapsed);
  gtk_label_set_selectable (self->subject_label, !collapsed);
}

/* Zeiger über einem Knopf (Name, Details)? Die klappen nicht. */
static gboolean
over_button (RevolutionMailHeader *self,
             double                x,
             double                y)
{
  GtkNative *native = gtk_widget_get_native (GTK_WIDGET (self));
  graphene_point_t point;
  GtkWidget *target;
  double dx, dy;

  gtk_native_get_surface_transform (native, &dx, &dy);
  if (!gtk_widget_compute_point (GTK_WIDGET (native), GTK_WIDGET (self), &GRAPHENE_POINT_INIT ((float) (x - dx), (float) (y - dy)), &point))
    return TRUE;
  target = gtk_widget_pick (GTK_WIDGET (self), point.x, point.y, GTK_PICK_DEFAULT);
  return target == NULL || gtk_widget_get_ancestor (target, GTK_TYPE_BUTTON) != NULL;
}

/* Klick auf die Fläche klappt in der Konversation auf und zu, auch auf Text: Auswählbare Labels beanspruchen ihre Klicks
 * für sich, kein GtkGestureClick oberhalb sähe sie, daher rohe Ereignisse in der Capture-Phase. Ziehen wählt Text aus
 * und klappt nicht, das Popover (Kind, eigene Oberfläche) zählt nicht. */
static gboolean
on_event (GtkEventControllerLegacy *controller G_GNUC_UNUSED,
          GdkEvent                 *event,
          RevolutionMailHeader     *self)
{
  GdkEventType type = gdk_event_get_event_type (event);
  GdkEventSequence *sequence = gdk_event_get_event_sequence (event);
  gboolean button = type == GDK_BUTTON_PRESS || type == GDK_BUTTON_RELEASE;
  gboolean touch = type == GDK_TOUCH_BEGIN || type == GDK_TOUCH_END;
  double x, y;

  if (type == GDK_TOUCH_CANCEL && sequence == self->press_sequence)
    self->pressed = FALSE;
  if (!self->compact || (!button && !touch) || (button && gdk_button_event_get_button (event) != GDK_BUTTON_PRIMARY) ||
      !gdk_event_get_position (event, &x, &y) ||
      gdk_event_get_surface (event) != gtk_native_get_surface (gtk_widget_get_native (GTK_WIDGET (self))))
    return GDK_EVENT_PROPAGATE;

  if (type == GDK_BUTTON_PRESS || type == GDK_TOUCH_BEGIN)
    {
      self->pressed = !over_button (self, x, y);
      self->press_sequence = sequence;
      self->press_x = x;
      self->press_y = y;
    }
  else if (self->pressed && sequence == self->press_sequence)
    {
      guint32 time = gdk_event_get_time (event);
      gboolean second = time - self->release_time < 400;

      self->pressed = FALSE;
      self->release_time = time;
      if (!second && !gtk_drag_check_threshold (GTK_WIDGET (self), (int) self->press_x, (int) self->press_y, (int) x, (int) y))
        {
          gtk_widget_grab_focus (GTK_WIDGET (self));
          g_signal_emit (self, signals[SIGNAL_ACTIVATED], 0);
        }
    }
  return GDK_EVENT_PROPAGATE;
}

static gboolean
on_activate_key (GtkWidget *widget,
                 GVariant  *args G_GNUC_UNUSED,
                 gpointer   user_data G_GNUC_UNUSED)
{
  RevolutionMailHeader *self = REVOLUTION_MAIL_HEADER (widget);

  if (!self->compact || !gtk_widget_is_focus (widget))
    return FALSE;
  g_signal_emit (self, signals[SIGNAL_ACTIVATED], 0);
  return TRUE;
}

static void
revolution_mail_header_dispose (GObject *object)
{
  RevolutionMailHeader *self = REVOLUTION_MAIL_HEADER (object);

  g_clear_weak_pointer (&self->active_name);
  g_clear_pointer (&self->popover, gtk_widget_unparent);
  g_clear_pointer (&self->menu_popover, gtk_widget_unparent);
  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_HEADER);
  g_cancellable_cancel (self->check);
  g_clear_object (&self->check);
  g_clear_object (&self->session);
  g_clear_object (&self->actions);
  g_clear_object (&self->from);
  g_clear_object (&self->to);
  g_clear_object (&self->cc);
  g_clear_object (&self->bcc);

  G_OBJECT_CLASS (revolution_mail_header_parent_class)->dispose (object);
}

static void
revolution_mail_header_finalize (GObject *object)
{
  RevolutionMailHeader *self = REVOLUTION_MAIL_HEADER (object);

  g_free (self->menu_name);
  g_free (self->menu_address);
  g_free (self->store_uid);
  g_free (self->folder_name);
  g_free (self->message_uid);
  g_free (self->account_uid);

  G_OBJECT_CLASS (revolution_mail_header_parent_class)->finalize (object);
}

static void
revolution_mail_header_class_init (RevolutionMailHeaderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = revolution_mail_header_dispose;
  object_class->finalize = revolution_mail_header_finalize;

  g_type_ensure (REVOLUTION_TYPE_MAIL_CONTACT_POPOVER);
  signals[SIGNAL_ACTIVATED] = g_signal_new ("activated", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 0);

  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/mail-header.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, grid);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, avatar);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, sender_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, folder_icon);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, folder_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, date_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, subject_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, preview_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, to_row);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, to_caption);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, to_box);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, details_tail);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, details_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, cc_row);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, cc_box);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, details_revealer);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, bcc_row);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, bcc_box);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, from_box);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, date_full);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailHeader, signature_row);
  gtk_widget_class_bind_template_callback (widget_class, on_details_toggled);
}

static void
revolution_mail_header_init (RevolutionMailHeader *self)
{
  GtkEventController *events = gtk_event_controller_legacy_new ();
  GtkEventController *keys = gtk_shortcut_controller_new ();

  gtk_widget_init_template (GTK_WIDGET (self));

  self->actions = g_simple_action_group_new ();
  g_action_map_add_action_entries (G_ACTION_MAP (self->actions), header_actions, G_N_ELEMENTS (header_actions), self);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "header", G_ACTION_GROUP (self->actions));

  self->popover = g_object_new (REVOLUTION_TYPE_MAIL_CONTACT_POPOVER, NULL);
  gtk_widget_set_parent (self->popover, GTK_WIDGET (self));
  g_signal_connect_object (self->popover, "closed", G_CALLBACK (on_popover_closed), self, G_CONNECT_SWAPPED);
  self->menu_popover = gtk_popover_menu_new_from_model (NULL);
  gtk_popover_set_has_arrow (GTK_POPOVER (self->menu_popover), FALSE);
  gtk_widget_set_parent (self->menu_popover, GTK_WIDGET (self));
  g_signal_connect_object (self->menu_popover, "closed", G_CALLBACK (on_popover_closed), self, G_CONNECT_SWAPPED);

  gtk_event_controller_set_propagation_phase (events, GTK_PHASE_CAPTURE);
  g_signal_connect (events, "event", G_CALLBACK (on_event), self);
  gtk_widget_add_controller (GTK_WIDGET (self), events);
  gtk_shortcut_controller_add_shortcut (GTK_SHORTCUT_CONTROLLER (keys),
                                        gtk_shortcut_new (gtk_shortcut_trigger_parse_string ("Return|KP_Enter|space"),
                                                          gtk_callback_action_new (on_activate_key, NULL, NULL)));
  gtk_widget_add_controller (GTK_WIDGET (self), keys);
}
