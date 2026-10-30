/* mail-actions.c – Nachrichtenaktionen der Mail-Ansicht: Antworten, Weiterleiten, Öffnen, Absender übernehmen,
 * Gelesen-Timer, Kontextmenü, Wischen, Löschen, Archiv, Spam, Verschieben mit Rückgängig (#359, #360, #370,
 * #354, #357, #353, #363).
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

#include "composer/composer.h"
#include "contacts/contacts-view.h"
#include "eds-compat.h"
#include "mail-favorites.h"
#include "mail-view-private.h"

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

/* #360: Weiterleiten (Strg+Alt+F wie Evolution e-mail-reader.c:3044-3049). */
void
on_forward (GSimpleAction *action G_GNUC_UNUSED,
            GVariant      *parameter G_GNUC_UNUSED,
            gpointer       user_data)
{
  open_reply (user_data, REVOLUTION_COMPOSER_FORWARD);
}

/* #360: Öffnen (Strg+O, Doppelklick, Eingabe) im Entwürfe-Ordner bearbeitet den Entwurf weiter wie Evolution
 * (e-mail-reader.c:3249-3254, e-mail-reader-utils.c:1438-1442); nur im Ordner selbst, nicht in den Favoriten. */
void
on_open (GSimpleAction *action G_GNUC_UNUSED,
         GVariant      *parameter G_GNUC_UNUSED,
         gpointer       user_data)
{
  RevolutionMailView *self = user_data;
  g_autoptr (RevolutionMailAccount) account = NULL;

  if (self->folder == NULL || (revolution_mail_folder_get_flags (self->folder) & CAMEL_FOLDER_TYPE_MASK) != CAMEL_FOLDER_TYPE_DRAFTS)
    return;
  g_object_get (self->folder, "account", &account, NULL);
  if (account != self->favorites)
    open_reply (self, REVOLUTION_COMPOSER_DRAFT);
}

void
on_message_activate (RevolutionMailView *self)
{
  g_action_group_activate_action (G_ACTION_GROUP (self->message_actions), "open", NULL);
}

void
on_reply (GSimpleAction *action G_GNUC_UNUSED,
          GVariant      *parameter G_GNUC_UNUSED,
          gpointer       user_data)
{
  open_reply (user_data, REVOLUTION_COMPOSER_REPLY);
}

void
on_reply_all (GSimpleAction *action G_GNUC_UNUSED,
              GVariant      *parameter G_GNUC_UNUSED,
              gpointer       user_data)
{
  open_reply (user_data, REVOLUTION_COMPOSER_REPLY_ALL);
}

void
on_mark_read (GSimpleAction *action G_GNUC_UNUSED,
              GVariant      *parameter G_GNUC_UNUSED,
              gpointer       user_data)
{
  mark_read (user_data, selected_message (user_data), TRUE, "gelesen");
}

void
on_mark_unread (GSimpleAction *action G_GNUC_UNUSED,
                GVariant      *parameter G_GNUC_UNUSED,
                gpointer       user_data)
{
  mark_read (user_data, selected_message (user_data), FALSE, "ungelesen");
}

void
on_star_on (GSimpleAction *action G_GNUC_UNUSED,
            GVariant      *parameter G_GNUC_UNUSED,
            gpointer       user_data)
{
  mark_starred (user_data, selected_message (user_data), TRUE, "markiert");
}

void
on_star_off (GSimpleAction *action G_GNUC_UNUSED,
             GVariant      *parameter G_GNUC_UNUSED,
             gpointer       user_data)
{
  mark_starred (user_data, selected_message (user_data), FALSE, "nicht markiert");
}

/* message.star (Stern-Knopf der Nachricht, mail-view.blp): Zustand folgt dem Eintrag
 * (update_message_actions), Umschalten setzt das Flag. */
void
on_star_change_state (GSimpleAction *action G_GNUC_UNUSED,
                      GVariant      *state,
                      gpointer       user_data)
{
  gboolean starred = g_variant_get_boolean (state);

  mark_starred (user_data, selected_message (user_data), starred, starred ? "markiert" : "nicht markiert");
}

/* #370: message.add-sender wie Evolution „Absender zum Adressbuch hinzufügen“ (mail-add-sender,
 * e-mail-reader.c:163-220, Menü Nachricht): Absender der gewählten Nachricht, in einer Konversation der
 * neuesten (wie open_reply); wechselt zu den Kontakten mit vorausgefülltem Formular (contacts-view.c). */
static const char *
selected_from (RevolutionMailView *self)
{
  RevolutionMailMessage *message = self->camel_folder != NULL ? selected_message (self) : NULL;
  GListModel *members = message != NULL ? revolution_mail_message_get_members (message) : NULL;
  g_autoptr (RevolutionMailMessage) member = NULL;

  if (members != NULL && g_list_model_get_n_items (members) > 0)
    {
      member = g_list_model_get_item (members, g_list_model_get_n_items (members) - 1);
      return revolution_mail_message_get_from (member);
    }
  return message != NULL ? revolution_mail_message_get_from (message) : NULL;
}

void
on_add_sender (GSimpleAction *action G_GNUC_UNUSED,
               GVariant      *parameter G_GNUC_UNUSED,
               gpointer       user_data)
{
  RevolutionMailView *self = user_data;
  g_autofree char *from = g_strdup (selected_from (self));
  gpointer contacts = g_object_get_data (G_OBJECT (gtk_widget_get_root (GTK_WIDGET (self))), "contacts-view");

  if (from == NULL || *from == '\0' || contacts == NULL)
    return;
  g_debug ("Mail: Absender als Kontakt %s", from);
  adw_view_stack_set_visible_child_name (self->stack, "contacts");
  revolution_contacts_view_add_sender (contacts, from);
}

/* #455: Aktionen zu einem Namen im Nachrichtenkopf (mail-header.c, mail-contact-popover.c). */
ESourceRegistry *
revolution_mail_view_get_registry (RevolutionMailView *self)
{
  g_return_val_if_fail (REVOLUTION_IS_MAIL_VIEW (self), NULL);

  return self->registry;
}

/* „Name <Adresse>“; ohne eigenen Namen (mail-header.c zeigt dann die Adresse) nur die Adresse. */
static char *
format_recipient (const char *name,
                  const char *address)
{
  return camel_internet_address_format_address (g_ascii_strcasecmp (name, address) != 0 ? name : NULL, address);
}

/* Neue Nachricht wie ein mailto-Link (RFC 6068, composer.c revolution_composer_new_mailto), Absender das Konto der
 * Mail wie Evolution (e-mail-display.c:324-326, em-composer-utils.c:2098). */
void
revolution_mail_view_compose_to (RevolutionMailView *self,
                                 const char         *account_uid,
                                 const char         *name,
                                 const char         *address)
{
  g_autofree char *formatted = format_recipient (name, address);
  g_autofree char *escaped = g_uri_escape_string (formatted, NULL, FALSE);
  g_autofree char *mailto = g_strconcat ("mailto:", escaped, NULL);

  g_return_if_fail (REVOLUTION_IS_MAIL_VIEW (self));

  if (self->registry == NULL || self->session == NULL)
    return;
  g_debug ("Mail: Nachricht an %s", address);
  gtk_window_present (revolution_composer_new_mailto (gtk_window_get_application (GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self)))),
                                                      self->registry, self->session, account_uid, mailto));
}

void
revolution_mail_view_save_contact (RevolutionMailView *self,
                                   const char         *name,
                                   const char         *address)
{
  gpointer contacts = g_object_get_data (G_OBJECT (gtk_widget_get_root (GTK_WIDGET (self))), "contacts-view");
  g_autofree char *from = format_recipient (name, address);

  if (contacts == NULL)
    return;
  g_debug ("Mail: Kontakt speichern %s", address);
  adw_view_stack_set_visible_child_name (self->stack, "contacts");
  revolution_contacts_view_add_sender (contacts, from);
}

void
revolution_mail_view_open_contact (RevolutionMailView *self,
                                   const char         *book_uid,
                                   const char         *contact_uid)
{
  gpointer contacts = g_object_get_data (G_OBJECT (gtk_widget_get_root (GTK_WIDGET (self))), "contacts-view");

  if (contacts == NULL)
    return;
  g_debug ("Mail: Kontakt %s in %s öffnen", contact_uid, book_uid);
  adw_view_stack_set_visible_child_name (self->stack, "contacts");
  revolution_contacts_view_show_contact (contacts, book_uid, contact_uid);
}

/* Nur die passenden Aktionen sind aktiv wie Evolution (Gelesen nur bei ungelesener Auswahl,
 * e-mail-reader.c mail_reader_update_actions); Menüeinträge mit hidden-when blenden die
 * übrigen aus. */
void
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
    { "forward", message != NULL },
    { "open", message != NULL },
    { "delete", message != NULL },
    { "archive", message != NULL },
    { "junk", message != NULL },
    { "move", message != NULL },
    { "move-to", message != NULL },
    { "add-sender", message != NULL && selected_from (self) != NULL && *selected_from (self) != '\0' },
  };
  GAction *star = g_action_map_lookup_action (self->message_actions, "star");

  for (gsize i = 0; i < G_N_ELEMENTS (actions); i++)
    g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (self->message_actions, actions[i].name)),
                                 actions[i].enabled);
  g_simple_action_set_state (G_SIMPLE_ACTION (star), g_variant_new_boolean (starred));
  gtk_widget_set_sensitive (GTK_WIDGET (self->move_button), message != NULL);
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

void
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
void
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

void
clear_member_seen (RevolutionMailView *self)
{
  for (guint i = 0; i < self->member_seen->len; i++)
    g_source_destroy (self->member_seen->pdata[i]);
  g_ptr_array_set_size (self->member_seen, 0);
}

void
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
void
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

/* #353: Kontextmenü der Zeile wie GNOME Dateien (Nautilus): Rechtsklick wählt die Zeile und
 * öffnet ein GtkPopoverMenu ohne Pfeil an der Klickstelle, Kürzel rechts (row_menu in
 * mail-view.blp). Rechtsklick verwirft den Gelesen-Timer wie Evolution (e-mail-reader.c:5733-5735).
 * Tastatur: Menü-Taste bzw. Umschalt+F10 (list.context-menu) an der Zeile mit dem Fokus. */
void
show_row_menu (RevolutionMailView *self,
               GtkWidget          *widget,
               double              x,
               double              y)
{
  /* #364: dieselbe Art Menü für den Ordnerbaum (folder_menu), nur für Ordner der Konten. */
  gboolean folders = gtk_widget_is_ancestor (widget, self->folder_list);
  GtkWidget *list = folders ? self->folder_list : GTK_WIDGET (self->message_list);
  GtkWidget *popover = folders ? self->folder_popover : self->row_popover;
  graphene_point_t point;

  if ((folders && !g_action_group_get_action_enabled (G_ACTION_GROUP (self->folder_actions), "new")) ||
      !gtk_widget_compute_point (widget, list, &GRAPHENE_POINT_INIT (x, y), &point))
    return;

  if (!folders)
    g_clear_handle_id (&self->seen_id, g_source_remove);
  gtk_popover_set_pointing_to (GTK_POPOVER (popover), &(GdkRectangle) { (int) point.x, (int) point.y, 1, 1 });
  gtk_popover_popup (GTK_POPOVER (popover));
}

RevolutionMailView *
view_for (GtkEventController *controller)
{
  return REVOLUTION_MAIL_VIEW (gtk_widget_get_ancestor (gtk_event_controller_get_widget (controller),
                                                        REVOLUTION_TYPE_MAIL_VIEW));
}

void
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

void
on_context_menu (GSimpleAction *action G_GNUC_UNUSED,
                 GVariant      *parameter G_GNUC_UNUSED,
                 gpointer       user_data)
{
  RevolutionMailView *self = user_data;
  GtkWidget *focus = gtk_root_get_focus (gtk_widget_get_root (GTK_WIDGET (self)));
  graphene_rect_t bounds;

  if (focus == NULL || !gtk_widget_compute_bounds (focus, focus, &bounds) ||
      !(gtk_widget_is_ancestor (focus, self->folder_list) ||
        (selected_message (self) != NULL && gtk_widget_is_ancestor (focus, GTK_WIDGET (self->message_list)))))
    return;

  show_row_menu (self, focus, bounds.size.width / 2, bounds.size.height / 2);
}

/* #353: Wischen über eine Zeile wie Apple Mail: nach rechts Gelesen/Ungelesen, nach links
 * Markieren/Markierung entfernen, für diese Zeile ohne Auswahl. GtkGestureSwipe meldet am Ende
 * die Geschwindigkeit (Touch, Touchpad-Ziehen und Maus); gilt nur waagerecht und ab
 * SWIPE_VELOCITY, damit ein Klick oder Ziehen nach unten nichts auslöst. */
#define SWIPE_VELOCITY 600.0

void
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

/* #363: Löschen, Archivieren, Spam und Verschieben. Evolution markiert beim Löschen nur (DELETED|SEEN,
 * e-mail-reader.c:573-594) und verschiebt erst beim Synchronisieren in einen echten Papierkorb, wenn die
 * ESource use-real-trash-path setzt (camel-imapx-store.c:2241-2260); bei geops@ und Gmail ist das aus, die
 * Mail bliebe am Server im Ordner. Hier wird daher wie Archivieren und Verschieben (mail_transfer_messages,
 * mail-ops.c:1236-1270) sofort verschoben, mit Rückgängig wie Stamp (Hinweis mit „Rückgängig“,
 * stamp-mail-view.c:266-281, stamp-conversation-list.c:3127-3145). Ziele: Ordner des Typs im Ordnerbaum
 * des Kontos (#350; Papierkorb und Spam vom Server oder aus der Ersteinrichtung, mail-folder.c), denn
 * camel_store_get_trash_folder_sync bzw. _junk_folder_sync liefern bei imapx ohne use-real-*-path die
 * virtuellen Ordner (camel-imapx-store.c:2193-2290). Archiv wie Evolution der Archivordner des Kontos
 * (e-mail-reader.c:1331-1437); ohne ihn (Gmail: ESource leer, kein \Archive, imapx verwirft die Typen des Servers
 * wie \All, camel-imapx-store.c:327-331) wie Evolution ein Hinweis statt Archivieren (dort 1420-1428). microsoft365: Papierkorb nur über revolution_eds_store_get_trash_folder_sync,
 * einmal je Store (#416, sie synchronisiert alle offenen Ordner), Verschieben nur über
 * revolution_eds_folder_transfer_messages_to_sync. Spam setzt wie Evolution SEEN|JUNK|JUNK_LEARN
 * (e-mail-reader.c:1030-1042) und verschiebt in den Spam-Ordner, damit Filter am Server lernen; nicht bei
 * microsoft365, das JUNK nicht überträgt (PATCH nur isRead/importance/categories, camel-m365-utils.c:728-747).
 * Rückgängig sucht die Nachrichten im Ziel über die Message-ID (microsoft365 meldet keine neuen UIDs,
 * camel-m365-folder.c:1498-1562) und verschiebt sie zurück. Alles im Worker in der Warteschlange der Flags. */
typedef enum { ACTION_DELETE, ACTION_ARCHIVE, ACTION_JUNK, ACTION_MOVE, ACTION_UNDO } TransferKind;

static const struct { const char *log; const char *toast; guint32 type; } transfer_kinds[] = {
  [ACTION_DELETE]  = { "gelöscht",   N_("Deleted"),          CAMEL_FOLDER_TYPE_TRASH },
  [ACTION_ARCHIVE] = { "archiviert", N_("Archived"),        CAMEL_FOLDER_TYPE_ARCHIVE },
  [ACTION_JUNK]    = { "Spam",       N_("Marked as Junk"), CAMEL_FOLDER_TYPE_JUNK },
  [ACTION_MOVE]    = { "verschoben", N_("Moved"),        0 },
  [ACTION_UNDO]    = { "rückgängig", NULL,                    0 },
};

#define JUNK_MASK (CAMEL_MESSAGE_SEEN | CAMEL_MESSAGE_JUNK | CAMEL_MESSAGE_NOTJUNK | CAMEL_MESSAGE_JUNK_LEARN)

struct _Transfer
{
  TransferKind kind;
  CamelFolder *list_folder;
  GPtrArray   *uids;
  char        *target;
  Transfer    *undo_of;
  /* Im Worker gesetzt: echte Quelle und Ziel, Message-IDs und Flags vorher, UIDs im Ziel. */
  CamelFolder *source;
  CamelFolder *dest;
  GArray      *ids;
  GArray      *flags;
  GPtrArray   *moved;
};

static void
transfer_clear (Transfer *t)
{
  g_clear_object (&t->list_folder);
  g_clear_pointer (&t->uids, g_ptr_array_unref);
  g_free (t->target);
  g_clear_pointer (&t->undo_of, transfer_release);
  g_clear_object (&t->source);
  g_clear_object (&t->dest);
  g_clear_pointer (&t->ids, g_array_unref);
  g_clear_pointer (&t->flags, g_array_unref);
  g_clear_pointer (&t->moved, g_ptr_array_unref);
}

void
transfer_release (Transfer *t)
{
  g_atomic_rc_box_release_full (t, (GDestroyNotify) transfer_clear);
}

static gboolean
is_m365 (CamelFolder *folder)
{
  CamelProvider *provider = camel_service_get_provider (CAMEL_SERVICE (camel_folder_get_parent_store (folder)));

  return provider != NULL && g_strcmp0 (provider->protocol, "microsoft365") == 0;
}

/* microsoft365: Papierkorb einmal je Store holen (#416), danach aus dem Speicher. Im Worker. */
static CamelFolder *
cached_trash (CamelStore *store,
              GError    **error)
{
  static GMutex lock;
  CamelFolder *trash;

  g_mutex_lock (&lock);
  trash = g_object_get_data (G_OBJECT (store), "revolution-trash");
  if (trash != NULL)
    g_object_ref (trash);
  else if ((trash = revolution_eds_store_get_trash_folder_sync (store, NULL, error)) != NULL)
    g_object_set_data_full (G_OBJECT (store), "revolution-trash", g_object_ref (trash), g_object_unref);
  g_mutex_unlock (&lock);

  return trash;
}

/* UIDs im Ziel zu den Message-IDs; zuerst unter denen, die vorher nicht dort waren. Im Worker. */
static GPtrArray *
find_moved (CamelFolder *dest,
            GArray      *ids,
            GHashTable  *before)
{
  GPtrArray *uids = camel_folder_get_uids (dest);
  GPtrArray *found = g_ptr_array_new_with_free_func (g_free);
  g_autofree gboolean *done = g_new0 (gboolean, MAX (ids->len, 1));

  for (int pass = 0; pass < 2 && found->len < ids->len; pass++)
    for (guint i = 0; i < uids->len; i++)
      {
        g_autoptr (CamelMessageInfo) info = NULL;
        guint64 id;

        if (g_hash_table_contains (before, uids->pdata[i]) != (pass == 1) ||
            (info = camel_folder_get_message_info (dest, uids->pdata[i])) == NULL)
          continue;
        id = camel_message_info_get_message_id (info);
        for (guint j = 0; j < ids->len; j++)
          if (!done[j] && id != 0 && g_array_index (ids, guint64, j) == id)
            {
              done[j] = TRUE;
              g_ptr_array_add (found, g_strdup (uids->pdata[i]));
              break;
            }
      }
  camel_folder_free_uids (dest, uids);

  return found;
}

void
transfer_thread (GTask    *task,
                 SetFlags *data)
{
  Transfer *t = data->transfer;
  Transfer *of = t->undo_of;
  g_autoptr (GPtrArray) uids = g_ptr_array_new_with_free_func (g_free);
  g_autoptr (GHashTable) before = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GPtrArray *dest_uids;
  GError *error = NULL;

  if (of != NULL)
    {
      t->source = of->dest != NULL ? g_object_ref (of->dest) : NULL;
      t->dest = of->source != NULL ? g_object_ref (of->source) : NULL;
      for (guint i = 0; of->moved != NULL && i < of->moved->len; i++)
        g_ptr_array_add (uids, g_strdup (of->moved->pdata[i]));
    }
  else
    {
      for (guint i = 0; i < t->uids->len; i++)
        {
          char *real_uid = NULL;
          g_autoptr (CamelFolder) real = real_folder (t->list_folder, t->uids->pdata[i], &real_uid);

          if (real != NULL && t->source == NULL)
            t->source = g_object_ref (real);
          if (real != NULL && real == t->source)
            g_ptr_array_add (uids, real_uid);
          else
            g_free (real_uid);
        }
      if (t->source != NULL)
        t->dest = t->kind == ACTION_DELETE && is_m365 (t->source)
                    ? cached_trash (camel_folder_get_parent_store (t->source), &error)
                    : camel_store_get_folder_sync (camel_folder_get_parent_store (t->source), t->target, 0, NULL, &error);
    }
  if (error == NULL && (t->source == NULL || t->dest == NULL || t->dest == t->source || uids->len == 0))
    g_set_error (&error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID_UID, _("Message %s not found"), data->uid);
  if (error != NULL)
    {
      g_task_return_error (task, error);
      return;
    }

  t->ids = g_array_new (FALSE, TRUE, sizeof (guint64));
  t->flags = g_array_new (FALSE, TRUE, sizeof (guint32));
  for (guint i = 0; i < uids->len; i++)
    {
      g_autoptr (CamelMessageInfo) info = camel_folder_get_message_info (t->source, uids->pdata[i]);
      guint64 id = info != NULL ? camel_message_info_get_message_id (info) : 0;
      guint32 flags = info != NULL ? camel_message_info_get_flags (info) : 0;

      g_array_append_val (t->ids, id);
      g_array_append_val (t->flags, flags);
    }

  /* Spam: Flags wie Evolution unter der Sperre je Ordner (#409) und zum Server, bevor verschoben wird. */
  if (t->kind == ACTION_JUNK && !is_m365 (t->source))
    {
      gboolean ok;

      revolution_eds_folder_lock (t->source);
      for (guint i = 0; i < uids->len; i++)
        camel_folder_set_message_flags (t->source, uids->pdata[i], JUNK_MASK,
                                        CAMEL_MESSAGE_SEEN | CAMEL_MESSAGE_JUNK | CAMEL_MESSAGE_JUNK_LEARN);
      ok = camel_folder_synchronize_sync (t->source, FALSE, NULL, &error);
      revolution_eds_folder_unlock (t->source);
      if (!ok)
        {
          g_task_return_error (task, error);
          return;
        }
    }

  dest_uids = camel_folder_get_uids (t->dest);
  for (guint i = 0; i < dest_uids->len; i++)
    g_hash_table_add (before, g_strdup (dest_uids->pdata[i]));
  camel_folder_free_uids (t->dest, dest_uids);

  if (!revolution_eds_folder_transfer_messages_to_sync (t->source, uids, t->dest, TRUE, NULL, NULL, &error))
    {
      g_task_return_error (task, error);
      return;
    }
  t->moved = find_moved (t->dest, t->ids, before);
  if (t->moved->len < uids->len && revolution_eds_folder_refresh_info_sync (t->dest, NULL, NULL))
    {
      g_ptr_array_unref (t->moved);
      t->moved = find_moved (t->dest, t->ids, before);
    }

  /* Rückgängig nach Spam: Flags von vorher. */
  if (of != NULL && of->kind == ACTION_JUNK && !is_m365 (t->dest))
    {
      revolution_eds_folder_lock (t->dest);
      for (guint i = 0; i < t->moved->len; i++)
        {
          g_autoptr (CamelMessageInfo) info = camel_folder_get_message_info (t->dest, t->moved->pdata[i]);

          for (guint j = 0; info != NULL && j < of->ids->len; j++)
            if (g_array_index (of->ids, guint64, j) == camel_message_info_get_message_id (info))
              camel_folder_set_message_flags (t->dest, t->moved->pdata[i], JUNK_MASK,
                                              g_array_index (of->flags, guint32, j) & JUNK_MASK);
        }
      if (!camel_folder_synchronize_sync (t->dest, FALSE, NULL, &error))
        {
          revolution_eds_folder_unlock (t->dest);
          g_task_return_error (task, error);
          return;
        }
      revolution_eds_folder_unlock (t->dest);
    }

  g_task_return_int (task, FLAGS_SAVED);
}

/* Im UI-Thread aus on_flags_set. Ein Fehler zeigt die Zeilen wieder (Liste neu aus der Summary). */
void
on_transferred (RevolutionMailView *self,
                SetFlags           *data,
                GError             *error)
{
  Transfer *t = data->transfer;
  g_autoptr (GString) moved = g_string_new (NULL);
  g_autofree char *detail = NULL;

  if (error == NULL)
    {
      for (guint i = 0; i < t->moved->len; i++)
        g_string_append_printf (moved, " %s", (char *) t->moved->pdata[i]);
      g_debug ("Aktion: %s %s %s → %s: %u von %u verschoben, im Ziel%s, nach %.0f ms", data->loading, data->what,
               camel_folder_get_full_name (t->source), camel_folder_get_full_name (t->dest), t->moved->len,
               t->ids->len, moved->str, (g_get_monotonic_time () - data->started) / 1000.0);
      return;
    }

  g_warning ("Aktion %s %s: %s", data->loading, data->what, error->message);
  if (self->cancellable == NULL || g_cancellable_is_cancelled (self->cancellable))
    return;
  detail = g_strdup_printf ("%s: %s", gtk_label_get_label (self->list_title), error->message);
  revolution_mail_view_show_error (self, _("Not Moved"), detail, FALSE);
  for (guint i = 0; t->list_folder == self->camel_folder && i < t->uids->len; i++)
    g_hash_table_add (self->pending_changed, g_strdup (t->uids->pdata[i]));
}

static void
queue_transfer (RevolutionMailView *self,
                Transfer           *t)
{
  SetFlags *data = g_new0 (SetFlags, 1);

  data->transfer = t;
  data->folder = g_object_ref (t->list_folder);
  data->uid = g_strdup (t->uids->pdata[0]);
  data->what = transfer_kinds[t->kind].log;
  data->loading = g_strdup (self->loading);
  data->started = g_get_monotonic_time ();
  g_debug ("Aktion: %s %s %u Nachrichten%s%s", data->loading, data->what, t->uids->len, t->target ? " nach " : "",
           t->target ? t->target : "");
  g_queue_push_tail (&self->flag_jobs, data);
  start_flags (self);
}

static void
on_undo (RevolutionMailView *self)
{
  g_autoptr (GPtrArray) undo = g_steal_pointer (&self->undo);

  for (guint i = 0; undo != NULL && i < undo->len; i++)
    {
      Transfer *of = undo->pdata[i];
      Transfer *t = g_atomic_rc_box_new0 (Transfer);

      t->kind = ACTION_UNDO;
      t->undo_of = g_atomic_rc_box_acquire (of);
      t->list_folder = g_object_ref (of->list_folder);
      t->uids = g_ptr_array_ref (of->uids);
      queue_transfer (self, t);
    }
  if (self->toast != NULL)
    adw_toast_dismiss (self->toast);
}

/* Strg+Z wirkt auch nach dem Hinweis bis zur nächsten Aktion, wie „Widerrufen“ in Apple Mail. */
static void
on_toast_dismissed (RevolutionMailView *self,
                    AdwToast           *toast)
{
  if (toast == self->toast)
    g_clear_object (&self->toast);
}

/* Kurzer Hinweis ohne Knopf, statt still nichts zu tun (Ziel = Quelle, anderes Konto). */
static void
show_hint (RevolutionMailView *self,
           const char         *title)
{
  AdwToastOverlay *overlay = ADW_TOAST_OVERLAY (gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_TOAST_OVERLAY));

  g_debug ("Hinweis: %s", title);
  if (overlay != NULL)
    adw_toast_overlay_add_toast (overlay, adw_toast_new (title));
}

/* Nachricht bzw. Konversation verschieben (target: voller Name im Konto target_account, nur bei
 * ACTION_MOVE). Konversation: ihre Nachrichten aus dem Ordner der Liste, nicht die aus Gesendet, wie ein
 * zugeklappter Thread in Evolution (e_mail_reader_get_selected_uids_with_collapsed_threads,
 * e-mail-reader.c:4357-4363; Evolution mischt keine Ordner) und Stamp (collect_uids_for_folder,
 * stamp-conversation-list.c:2399-2417). Je Konto und Quellordner ein Auftrag. Die Zeile verschwindet
 * sofort, gewählt wird die nächste, sonst die vorige wie Evolution (e_mail_reader_select_next_message,
 * e-mail-reader-utils.c:3210-3229). TRUE, wenn etwas verschoben wird. */
static gboolean
transfer_message (RevolutionMailView    *self,
                  RevolutionMailMessage *message,
                  TransferKind           kind,
                  const char            *target,
                  const char            *target_account)
{
  GListModel *members = message != NULL ? revolution_mail_message_get_members (message) : NULL;
  g_autoptr (GHashTable) groups = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_ptr_array_unref);
  g_autoptr (GPtrArray) undo = g_ptr_array_new_with_free_func ((GDestroyNotify) transfer_release);
  g_autoptr (RevolutionMailAccount) current = NULL;
  g_autofree char *title = NULL;
  const char *hint = NULL;
  AdwToastOverlay *overlay;
  GHashTableIter iter;
  gpointer key, value;
  guint position;

  if (message == NULL || self->camel_folder == NULL || self->folder == NULL)
    return FALSE;
  g_object_get (self->folder, "account", &current, NULL);

  for (guint i = 0; i < (members != NULL ? g_list_model_get_n_items (members) : 1); i++)
    {
      g_autoptr (RevolutionMailMessage) item = members != NULL ? g_list_model_get_item (members, i) : g_object_ref (message);
      const char *uid = current != NULL ? revolution_mail_account_get_uid (current) : "";
      const char *folder = revolution_mail_folder_get_full_name (self->folder);
      const char *source_uid, *source_folder;
      char *group;

      if (revolution_mail_message_get_source (item, &source_uid, &source_folder))
        {
          if (self->thread_folder != NULL && g_strcmp0 (source_folder, folder) != 0)
            continue;
          uid = source_uid;
          folder = source_folder;
        }
      group = g_strconcat (uid, "\n", folder, NULL);
      if (!g_hash_table_contains (groups, group))
        g_hash_table_insert (groups, g_strdup (group), g_ptr_array_new_with_free_func (g_free));
      g_ptr_array_add (g_hash_table_lookup (groups, group), g_strdup (revolution_mail_message_get_uid (item)));
      g_free (group);
    }

  g_hash_table_iter_init (&iter, groups);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      g_autofree char *uid = g_strndup (key, strchr (key, '\n') - (char *) key);
      const char *source = strchr (key, '\n') + 1;
      g_autoptr (RevolutionMailAccount) account = find_account (self, uid);
      GListModel *tree = account != NULL ? gtk_tree_list_model_get_model (account_tree (account)) : NULL;
      g_autoptr (RevolutionMailFolder) folder = NULL;
      const char *name = target;
      Transfer *t;

      if (kind != ACTION_MOVE)
        {
          folder = find_folder_type (tree, transfer_kinds[kind].type);
          name = folder != NULL ? revolution_mail_folder_get_full_name (folder) : NULL;
        }
      else if (g_strcmp0 (uid, target_account) != 0)
        {
          hint = _("Messages cannot be moved between accounts");
          continue;
        }
      if (name == NULL || g_strcmp0 (name, source) == 0)
        {
          hint = name != NULL ? _("The message is already in this folder") : NULL;
          g_debug ("Aktion: %s %s nicht möglich, Ziel %s", uid, transfer_kinds[kind].log, name ? "ist die Quelle" : "fehlt");
          if (name == NULL)
            {
              g_autofree char *detail = g_strdup_printf ("%s: %s", uid, _(transfer_kinds[kind].toast));

              revolution_mail_view_show_error (self, _("No Target Folder"), detail, FALSE);
            }
          continue;
        }

      t = g_atomic_rc_box_new0 (Transfer);
      t->kind = kind;
      t->list_folder = g_object_ref (self->camel_folder);
      t->uids = g_ptr_array_ref (value);
      t->target = g_strdup (name);
      g_ptr_array_add (undo, g_atomic_rc_box_acquire (t));
      queue_transfer (self, t);
    }
  if (undo->len == 0)
    {
      if (hint != NULL)
        show_hint (self, hint);
      return FALSE;
    }

  if (g_list_store_find (self->messages, message, &position))
    {
      gboolean selected = message == selected_message (self);
      guint n_items;

      g_list_store_remove (self->messages, position);
      n_items = g_list_model_get_n_items (G_LIST_MODEL (self->messages));
      set_list_counts (self);
      if (selected && n_items > 0)
        gtk_single_selection_set_selected (self->message_selection, MIN (position, n_items - 1));
      g_debug ("Aktion: Zeile %u weg, gewählt %u von %u", position, gtk_single_selection_get_selected (self->message_selection),
               n_items);
    }

  /* Hinweis mit Rückgängig (Strg+Z), ein neuer ersetzt den alten. */
  if (self->toast != NULL)
    adw_toast_dismiss (self->toast);
  g_clear_object (&self->toast);
  g_clear_pointer (&self->undo, g_ptr_array_unref);
  overlay = ADW_TOAST_OVERLAY (gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_TOAST_OVERLAY));
  if (overlay == NULL)
    return TRUE;
  title = kind == ACTION_MOVE ? g_strdup_printf (_("Moved to “%s”"), strrchr (target, '/') ? strrchr (target, '/') + 1 : target)
                              : g_strdup (_(transfer_kinds[kind].toast));
  self->toast = g_object_ref (adw_toast_new (title));
  adw_toast_set_button_label (self->toast, _("_Undo"));
  g_signal_connect_object (self->toast, "button-clicked", G_CALLBACK (on_undo), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->toast, "dismissed", G_CALLBACK (on_toast_dismissed), self, G_CONNECT_SWAPPED);
  self->undo = g_steal_pointer (&undo);
  adw_toast_overlay_add_toast (overlay, self->toast);

  return TRUE;
}

/* Ordner des Kontos der gewählten Nachricht als Ziele (Kopfleiste und Kontextmenü), ohne den eigenen. */
static void
append_move_items (GMenu      *menu,
                   GListModel *folders,
                   const char *prefix,
                   const char *exclude)
{
  for (guint i = 0; folders != NULL && i < g_list_model_get_n_items (folders); i++)
    {
      g_autoptr (RevolutionMailFolder) folder = g_list_model_get_item (folders, i);
      const char *name = revolution_mail_folder_get_full_name (folder);
      g_autofree char *display = NULL;
      g_autofree char *label = NULL;

      g_object_get (folder, "display-name", &display, NULL);
      label = prefix != NULL ? g_strdup_printf ("%s/%s", prefix, display) : g_strdup (display);
      if ((revolution_mail_folder_get_flags (folder) & (CAMEL_FOLDER_NOSELECT | CAMEL_FOLDER_VIRTUAL)) == 0 &&
          g_strcmp0 (name, exclude) != 0)
        {
          g_autoptr (GMenuItem) item = g_menu_item_new (label, NULL);

          g_menu_item_set_action_and_target (item, "message.move-to", "s", name);
          g_debug ("Aktion: Ziel %s", name);
          g_menu_append_item (menu, item);
        }
      append_move_items (menu, revolution_mail_folder_get_children (folder), label, exclude);
    }
}

static void
fill_move_menu (RevolutionMailView *self)
{
  RevolutionMailMessage *message = selected_message (self);
  GListModel *members = message != NULL ? revolution_mail_message_get_members (message) : NULL;
  g_autoptr (RevolutionMailMessage) first = members != NULL ? g_list_model_get_item (members, 0)
                                                            : message != NULL ? g_object_ref (message) : NULL;
  g_autoptr (RevolutionMailAccount) account = NULL;
  const char *uid, *folder;

  g_menu_remove_all (self->move_menu);
  g_clear_pointer (&self->move_account, g_free);
  if (first == NULL || self->folder == NULL)
    return;
  if (revolution_mail_message_get_source (first, &uid, &folder))
    account = find_account (self, uid);
  else
    {
      g_object_get (self->folder, "account", &account, NULL);
      folder = revolution_mail_folder_get_full_name (self->folder);
    }
  if (account == NULL || account == self->favorites)
    return;
  self->move_account = g_strdup (revolution_mail_account_get_uid (account));
  append_move_items (self->move_menu, gtk_tree_list_model_get_model (account_tree (account)), NULL, folder);
}

void
on_move_popup (GtkMenuButton *button G_GNUC_UNUSED,
               gpointer       user_data)
{
  fill_move_menu (user_data);
}

void
on_transfer_action (GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       user_data)
{
  RevolutionMailView *self = user_data;
  const char *name = g_action_get_name (G_ACTION (action));

  if (g_str_equal (name, "move"))
    gtk_menu_button_popup (self->move_button);
  else if (g_str_equal (name, "undo"))
    on_undo (self);
  else if (g_str_equal (name, "move-to"))
    {
      transfer_message (self, selected_message (self), ACTION_MOVE, g_variant_get_string (parameter, NULL), self->move_account);
      /* Das Menü ist zu, der Fokus läge sonst außerhalb der Ansicht (Kürzel wie Strg+Z wirkungslos, gemessen). */
      gtk_widget_grab_focus (GTK_WIDGET (self->message_list));
    }
  else
    transfer_message (self, selected_message (self),
                      g_str_equal (name, "delete") ? ACTION_DELETE : g_str_equal (name, "archive") ? ACTION_ARCHIVE : ACTION_JUNK,
                      NULL, NULL);
}

/* Ziehen einer Zeile (GtkDragSource, mail-view.blp) wie Stamp (stamp-conversation-list.c:1619-1645), als Inhalt
 * aber Text „revolution-message:<UID>“ statt des Objekts: unter X11 trägt XDND nur MIME-Typen, ein reiner
 * GType kommt nie an (gemessen mit GDK_DEBUG=dnd: „DND formats:“ leer, kein Ablegen). Schnelles waagerechtes
 * Ziehen ist Wischen (#353, on_row_swipe): solange die Geschwindigkeit dafür spricht oder noch unbekannt ist,
 * kein Ziehen (GTK fragt bei jeder Bewegung neu). */
#define DRAG_PREFIX "revolution-message:"

GdkContentProvider *
on_row_drag_prepare (GtkListItem   *item,
                     double         x G_GNUC_UNUSED,
                     double         y G_GNUC_UNUSED,
                     GtkDragSource *source)
{
  g_autoptr (GListModel) controllers = gtk_widget_observe_controllers (gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (source)));
  RevolutionMailMessage *message = gtk_list_item_get_item (item);
  g_autofree char *text = NULL;
  double vx = 0, vy = 0;

  for (guint i = 0; i < g_list_model_get_n_items (controllers); i++)
    {
      g_autoptr (GObject) controller = g_list_model_get_item (controllers, i);

      if (GTK_IS_GESTURE_SWIPE (controller) && !gtk_gesture_swipe_get_velocity (GTK_GESTURE_SWIPE (controller), &vx, &vy))
        return NULL;
    }
  if (message == NULL || (ABS (vx) >= SWIPE_VELOCITY && ABS (vx) >= 2 * ABS (vy)))
    return NULL;

  text = g_strconcat (DRAG_PREFIX, revolution_mail_message_get_uid (message), NULL);

  return gdk_content_provider_new_typed (G_TYPE_STRING, text);
}

/* Ablegen auf einem Ordner der Seitenleiste (GtkDropTarget, mail-view.blp), nur im selben Konto. */
gboolean
on_folder_drop (GtkListItem   *item,
                const GValue  *value,
                double         x G_GNUC_UNUSED,
                double         y G_GNUC_UNUSED,
                GtkDropTarget *target)
{
  RevolutionMailView *self = view_for (GTK_EVENT_CONTROLLER (target));
  GtkTreeListRow *row = gtk_list_item_get_item (item);
  g_autoptr (RevolutionMailFolder) folder = row != NULL ? gtk_tree_list_row_get_item (row) : NULL;
  g_autoptr (RevolutionMailAccount) account = NULL;
  g_autoptr (RevolutionMailMessage) message = NULL;
  const char *text = g_value_get_string (value);

  for (guint i = 0; text != NULL && g_str_has_prefix (text, DRAG_PREFIX) && message == NULL &&
                    i < g_list_model_get_n_items (G_LIST_MODEL (self->messages)); i++)
    {
      message = g_list_model_get_item (G_LIST_MODEL (self->messages), i);
      if (g_strcmp0 (revolution_mail_message_get_uid (message), text + strlen (DRAG_PREFIX)) != 0)
        g_clear_object (&message);
    }
  if (message == NULL || folder == NULL ||
      (revolution_mail_folder_get_flags (folder) & (CAMEL_FOLDER_NOSELECT | CAMEL_FOLDER_VIRTUAL)) != 0)
    return FALSE;
  g_object_get (folder, "account", &account, NULL);
  g_debug ("Aktion: Ziehen auf %s", revolution_mail_folder_get_full_name (folder));
  if (account == NULL || account == self->favorites)
    {
      show_hint (self, _("Cannot move here"));
      return FALSE;
    }

  return transfer_message (self, message, ACTION_MOVE, revolution_mail_folder_get_full_name (folder),
                           revolution_mail_account_get_uid (account));
}
