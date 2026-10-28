/* mail-conversation.c – Lesebereich einer Konversation (#357)
 *
 * Die Auswahl einer Konversation in der Liste (mail-view.c) zeigt hier alle Nachrichten des Threads
 * untereinander, neueste oben. Je Nachricht ein Kopf (Bild, Absender, Ort, Datum, Vorschau, Knöpfe
 * für Markieren und Gelesen); ein Klick oder Eingabe/Leertaste klappt die Nachricht auf bzw. zu.
 * Aufgeklappt zeigt sie ein eigener Lesebereich (RevolutionMailReader, mail-reader.c, ohne eigenen
 * Kopf und so hoch wie der Inhalt), mit Anhängen (#356) und Sperre entfernter Inhalte (#355) je
 * Nachricht. Zugeklappt gibt es keinen Lesebereich und keine WebView (Speicher): beim Zuklappen wird er
 * entfernt. Beim Öffnen sind die neueste und alle ungelesenen Nachrichten offen, die übrigen zu, wie
 * Stamp (stamp-message-list.c:465-470, 551-553: ungelesene aufgeklappt) und Apple Mail (neueste offen,
 * Dokument #10 „#357“).
 *
 * Vorlage Stamp views/mail/message-list (stamp-message-list.c:461-620 Liste der Nachrichten eines
 * Threads, stamp-message-list-item.c:350-368 Auf-/Zuklappen). Kommen Nachrichten hinzu (Live-Änderung
 * der Liste, mail-message.c update_members), bleiben die bestehenden Einträge mit ihrem Zustand.
 * Signale an mail-view.c: „shown“ (Inhalt einer Nachricht angezeigt: Gelesen-Timer, #353) und „mark“
 * (Markieren/Gelesen einer einzelnen Nachricht).
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>

#include "mail-conversation.h"
#include "mail-reader.h"
#include "mail-view.h"

struct _RevolutionMailConversation
{
  AdwBin                 parent_instance;
  GtkScrolledWindow     *scroller;
  GtkLabel              *subject_label;
  GtkLabel              *count_label;
  GtkBox                *entries;
  CamelSession          *session;
  RevolutionMailFolder  *folder;
  RevolutionMailMessage *conversation;
  gulong                 items_handler;
  /* Nachricht → Eintrag (GtkBox mit Entry als Daten). */
  GHashTable            *shown;
};

G_DEFINE_FINAL_TYPE (RevolutionMailConversation, revolution_mail_conversation, ADW_TYPE_BIN)

enum { SIGNAL_SHOWN, SIGNAL_MARK, N_SIGNALS };
static guint signals[N_SIGNALS];

typedef struct
{
  RevolutionMailConversation *self;
  RevolutionMailMessage      *member;
  GtkWidget                  *box;
  GtkWidget                  *header;
  GtkWidget                  *dot;
  GtkWidget                  *sender;
  GtkWidget                  *date;
  GtkWidget                  *preview;
  GtkWidget                  *star;
  GtkWidget                  *read;
  GtkWidget                  *reader;
  gboolean                    expanded;
  gulong                      notify_handler;
} Entry;

static void
entry_free (Entry *entry)
{
  g_clear_signal_handler (&entry->notify_handler, entry->member);
  g_object_unref (entry->member);
  g_free (entry);
}

static Entry *
entry_of (GtkWidget *box)
{
  return g_object_get_data (G_OBJECT (box), "revolution-entry");
}

/* Zustand der Nachricht (ungelesen, markiert) an Kopf und Knöpfen, zugänglicher Name (Architektur 8). */
static void
entry_update (Entry *entry)
{
  gboolean unread = revolution_mail_message_get_unread (entry->member);
  gboolean starred = revolution_mail_message_get_starred (entry->member);
  g_autofree char *sender = NULL, *date = NULL, *label = NULL;

  g_object_get (entry->member, "sender", &sender, "date", &date, NULL);
  gtk_widget_set_visible (entry->dot, unread);
  if (unread)
    gtk_widget_add_css_class (entry->sender, "heading");
  else
    gtk_widget_remove_css_class (entry->sender, "heading");

  gtk_button_set_icon_name (GTK_BUTTON (entry->star), starred ? "starred-symbolic" : "non-starred-symbolic");
  gtk_widget_set_tooltip_text (entry->star, starred ? _("Remove Flag") : _("Flag"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (entry->star), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  starred ? _("Remove Flag") : _("Flag"), -1);
  if (starred)
    gtk_widget_add_css_class (entry->star, "accent");
  else
    gtk_widget_remove_css_class (entry->star, "accent");

  gtk_button_set_icon_name (GTK_BUTTON (entry->read), unread ? "mail-unread-symbolic" : "mail-read-symbolic");
  gtk_widget_set_tooltip_text (entry->read, unread ? _("Mark as Read") : _("Mark as Unread"));
  gtk_accessible_update_property (GTK_ACCESSIBLE (entry->read), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                  unread ? _("Mark as Read") : _("Mark as Unread"), -1);

  /* Translators: zugänglicher Name des Kopfs einer Nachricht in der Konversation: Absender, Datum. */
  label = g_strdup_printf (_("Message from %s, %s%s%s"), sender, date, unread ? _(", unread") : "",
                           starred ? _(", flagged") : "");
  gtk_accessible_update_property (GTK_ACCESSIBLE (entry->header), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}

static void
on_reader_shown (RevolutionMailReader *reader G_GNUC_UNUSED,
                 Entry                *entry)
{
  g_signal_emit (entry->self, signals[SIGNAL_SHOWN], 0, entry->member);
}

/* Aufklappen legt den Lesebereich an und lädt die Nachricht (mail-reader.c, im Worker), Zuklappen
 * entfernt ihn samt WebView. Geladen wird aus dem Ordner der Liste (folder: Ordner, Favorit oder
 * Suchordner der Konversation), der Kopf nennt den echten Ort (revolution_mail_view_find_place). */
static void
entry_set_expanded (Entry    *entry,
                    gboolean  expanded)
{
  RevolutionMailConversation *self = entry->self;

  entry->expanded = expanded;
  gtk_widget_set_visible (entry->preview, !expanded);
  gtk_accessible_update_state (GTK_ACCESSIBLE (entry->header), GTK_ACCESSIBLE_STATE_EXPANDED, expanded, -1);

  if (expanded && entry->reader == NULL && self->session != NULL && self->folder != NULL)
    {
      GtkWidget *view = gtk_widget_get_ancestor (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_VIEW);
      g_autoptr (RevolutionMailFolder) place = view != NULL ? revolution_mail_view_find_place (REVOLUTION_MAIL_VIEW (view), entry->member)
                                                            : NULL;

      entry->reader = g_object_new (REVOLUTION_TYPE_MAIL_READER, NULL);
      revolution_mail_reader_set_compact (REVOLUTION_MAIL_READER (entry->reader), TRUE);
      g_signal_connect (entry->reader, "shown", G_CALLBACK (on_reader_shown), entry);
      gtk_box_append (GTK_BOX (entry->box), entry->reader);
      revolution_mail_reader_show (REVOLUTION_MAIL_READER (entry->reader), self->session, self->folder,
                                   place != NULL ? place : self->folder, entry->member);
      g_debug ("Konversation: %s aufgeklappt", revolution_mail_message_get_uid (entry->member));
    }
  else if (!expanded && entry->reader != NULL)
    {
      gtk_box_remove (GTK_BOX (entry->box), entry->reader);
      entry->reader = NULL;
      g_debug ("Konversation: %s zugeklappt", revolution_mail_message_get_uid (entry->member));
    }
}

static void
on_header_clicked (GtkButton *button G_GNUC_UNUSED,
                   Entry     *entry)
{
  entry_set_expanded (entry, !entry->expanded);
}

static void
on_star_clicked (GtkButton *button G_GNUC_UNUSED,
                 Entry     *entry)
{
  gboolean starred = revolution_mail_message_get_starred (entry->member);

  g_signal_emit (entry->self, signals[SIGNAL_MARK], 0, entry->member, (guint) CAMEL_MESSAGE_FLAGGED,
                 starred ? 0u : (guint) CAMEL_MESSAGE_FLAGGED);
}

/* Ungelesen hebt auch „gelöscht“ auf wie die Liste (mail-view.c mark_read, Evolution e-mail-reader.c:1183). */
static void
on_read_clicked (GtkButton *button G_GNUC_UNUSED,
                 Entry     *entry)
{
  gboolean unread = revolution_mail_message_get_unread (entry->member);

  g_signal_emit (entry->self, signals[SIGNAL_MARK], 0, entry->member,
                 (guint) (unread ? CAMEL_MESSAGE_SEEN : CAMEL_MESSAGE_SEEN | CAMEL_MESSAGE_DELETED),
                 unread ? (guint) CAMEL_MESSAGE_SEEN : 0u);
}

static GtkWidget *
entry_new (RevolutionMailConversation *self,
           RevolutionMailMessage      *member)
{
  Entry *entry = g_new0 (Entry, 1);
  GtkWidget *row, *content, *top, *avatar, *place_label, *dot_box;
  g_autoptr (RevolutionMailFolder) place = NULL;
  GtkWidget *view = gtk_widget_get_ancestor (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_VIEW);
  g_autofree char *sender = NULL, *avatar_text = NULL, *date = NULL, *preview = NULL, *place_name = NULL;

  g_object_get (member, "sender", &sender, "avatar", &avatar_text, "date", &date, "preview", &preview, NULL);
  entry->self = self;
  entry->member = g_object_ref (member);

  entry->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (entry->box, "conversation-entry");
  g_object_set_data_full (G_OBJECT (entry->box), "revolution-entry", entry, (GDestroyNotify) entry_free);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append (GTK_BOX (entry->box), row);

  /* Kopf als flacher Knopf: Tastatur (Tab, Eingabe, Leertaste) und Zeiger. */
  entry->header = gtk_button_new ();
  gtk_widget_set_hexpand (entry->header, TRUE);
  gtk_widget_add_css_class (entry->header, "flat");
  gtk_widget_add_css_class (entry->header, "conversation-header");
  gtk_widget_set_tooltip_text (entry->header, _("Expand or Collapse Message"));
  g_signal_connect (entry->header, "clicked", G_CALLBACK (on_header_clicked), entry);
  gtk_box_append (GTK_BOX (row), entry->header);

  content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_button_set_child (GTK_BUTTON (entry->header), content);

  dot_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request (dot_box, 10, -1);
  gtk_widget_set_valign (dot_box, GTK_ALIGN_START);
  gtk_widget_set_margin_top (dot_box, 4);
  entry->dot = gtk_image_new_from_icon_name ("media-record-symbolic");
  gtk_image_set_pixel_size (GTK_IMAGE (entry->dot), 10);
  gtk_widget_add_css_class (entry->dot, "accent");
  gtk_accessible_update_property (GTK_ACCESSIBLE (entry->dot), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Unread"), -1);
  gtk_box_append (GTK_BOX (dot_box), entry->dot);
  gtk_box_append (GTK_BOX (content), dot_box);

  avatar = adw_avatar_new (32, avatar_text != NULL ? avatar_text : sender, TRUE);
  gtk_widget_set_valign (avatar, GTK_ALIGN_START);
  gtk_widget_set_margin_end (avatar, 6);
  gtk_box_append (GTK_BOX (content), avatar);

  top = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand (top, TRUE);
  gtk_box_append (GTK_BOX (content), top);

  {
    GtkWidget *line = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);

    entry->sender = gtk_label_new (sender);
    gtk_label_set_xalign (GTK_LABEL (entry->sender), 0);
    gtk_label_set_ellipsize (GTK_LABEL (entry->sender), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (entry->sender, TRUE);
    gtk_box_append (GTK_BOX (line), entry->sender);

    /* Ort (Ordner — Konto) wie der Kopf des Lesebereichs (mail-reader.c), z. B. „Gesendet — geops@…“. */
    place = view != NULL ? revolution_mail_view_find_place (REVOLUTION_MAIL_VIEW (view), member) : NULL;
    if (place != NULL)
      {
        g_autoptr (RevolutionMailAccount) account = NULL;
        g_autofree char *folder_name = NULL, *account_name = NULL;

        g_object_get (place, "account", &account, "display-name", &folder_name, NULL);
        g_object_get (account, "display-name", &account_name, NULL);
        /* Translators: Ordner und Konto im Nachrichtenkopf, z. B. „Eingang — Privat“. */
        place_name = g_strdup_printf (_("%s — %s"), folder_name, account_name);
      }
    place_label = gtk_label_new (place_name);
    gtk_label_set_ellipsize (GTK_LABEL (place_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (place_label), 28);
    gtk_widget_set_visible (place_label, place_name != NULL);
    gtk_widget_add_css_class (place_label, "caption");
    gtk_widget_add_css_class (place_label, "dimmed");
    gtk_box_append (GTK_BOX (line), place_label);

    entry->date = gtk_label_new (date);
    gtk_widget_add_css_class (entry->date, "caption");
    gtk_widget_add_css_class (entry->date, "numeric");
    gtk_widget_add_css_class (entry->date, "dimmed");
    gtk_box_append (GTK_BOX (line), entry->date);
    gtk_box_append (GTK_BOX (top), line);
  }

  entry->preview = gtk_label_new (preview);
  gtk_label_set_xalign (GTK_LABEL (entry->preview), 0);
  gtk_label_set_ellipsize (GTK_LABEL (entry->preview), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (entry->preview), TRUE);
  gtk_widget_add_css_class (entry->preview, "dimmed");
  gtk_box_append (GTK_BOX (top), entry->preview);

  /* Knöpfe je Nachricht neben dem Kopf (nicht darin: keine Knöpfe in Knöpfen). */
  entry->read = gtk_button_new ();
  gtk_widget_set_valign (entry->read, GTK_ALIGN_START);
  gtk_widget_add_css_class (entry->read, "flat");
  gtk_widget_add_css_class (entry->read, "circular");
  g_signal_connect (entry->read, "clicked", G_CALLBACK (on_read_clicked), entry);
  gtk_box_append (GTK_BOX (row), entry->read);

  entry->star = gtk_button_new ();
  gtk_widget_set_valign (entry->star, GTK_ALIGN_START);
  gtk_widget_add_css_class (entry->star, "flat");
  gtk_widget_add_css_class (entry->star, "circular");
  g_signal_connect (entry->star, "clicked", G_CALLBACK (on_star_clicked), entry);
  gtk_box_append (GTK_BOX (row), entry->star);

  entry->notify_handler = g_signal_connect_swapped (member, "notify", G_CALLBACK (entry_update), entry);
  entry_update (entry);
  entry_set_expanded (entry, FALSE);

  return entry->box;
}

static void
show_count (RevolutionMailConversation *self)
{
  GListModel *members = revolution_mail_message_get_members (self->conversation);
  guint n = members != NULL ? g_list_model_get_n_items (members) : 1;
  g_autofree char *count = g_strdup_printf (ngettext ("%u message", "%u messages", n), n);
  g_autofree char *subject = NULL;

  g_object_get (self->conversation, "subject", &subject, NULL);
  gtk_label_set_label (self->subject_label, subject);
  gtk_label_set_label (self->count_label, count);
}

/* Einträge nach den Nachrichten der Konversation, neueste oben. Bestehende bleiben (Zustand, geladene
 * Lesebereiche), neue kommen dazu: zu Beginn die neueste und ungelesene aufgeklappt, später eintreffende
 * ungelesene ebenso. */
static void
sync_entries (RevolutionMailConversation *self,
              gboolean                    initial)
{
  GListModel *members = revolution_mail_message_get_members (self->conversation);
  g_autoptr (GHashTable) keep = g_hash_table_new (NULL, NULL);
  GtkWidget *previous = NULL;
  guint n = members != NULL ? g_list_model_get_n_items (members) : 0;
  GHashTableIter iter;
  gpointer key, value;

  for (guint i = n; i > 0; i--)
    {
      g_autoptr (RevolutionMailMessage) member = g_list_model_get_item (members, i - 1);
      GtkWidget *box = g_hash_table_lookup (self->shown, member);

      if (box == NULL)
        {
          const char *source_uid = NULL, *source_folder = NULL;

          /* Beleg der Gruppierung (tests/konversationen.sh): UID, Datum und echter Ordner je Nachricht. */
          if (!revolution_mail_message_get_source (member, &source_uid, &source_folder) && self->folder != NULL)
            source_folder = revolution_mail_folder_get_full_name (self->folder);
          g_debug ("Konversation: Nachricht %s %" G_GINT64_FORMAT " aus %s/%s", revolution_mail_message_get_uid (member),
                   revolution_mail_message_get_timestamp (member), source_uid != NULL ? source_uid : "-",
                   source_folder != NULL ? source_folder : "-");
          box = entry_new (self, member);
          g_hash_table_insert (self->shown, g_object_ref (member), box);
          gtk_box_insert_child_after (self->entries, box, previous);
          if ((initial && i == n) || revolution_mail_message_get_unread (member))
            entry_set_expanded (entry_of (box), TRUE);
        }
      else
        gtk_box_reorder_child_after (self->entries, box, previous);

      g_hash_table_add (keep, member);
      previous = box;
    }

  g_hash_table_iter_init (&iter, self->shown);
  while (g_hash_table_iter_next (&iter, &key, &value))
    if (!g_hash_table_contains (keep, key))
      {
        gtk_box_remove (self->entries, value);
        g_hash_table_iter_remove (&iter);
      }

  show_count (self);
}

static void
on_members_changed (RevolutionMailConversation *self)
{
  sync_entries (self, FALSE);
}

/* Auswahl einer Konversation in der Liste (mail-view.c, on_message_selected). */
void
revolution_mail_conversation_show (RevolutionMailConversation *self,
                                   CamelSession               *session,
                                   RevolutionMailFolder       *folder,
                                   RevolutionMailMessage      *conversation)
{
  GListModel *members;

  g_return_if_fail (REVOLUTION_IS_MAIL_CONVERSATION (self));
  g_return_if_fail (CAMEL_IS_SESSION (session));
  g_return_if_fail (REVOLUTION_IS_MAIL_FOLDER (folder));
  g_return_if_fail (REVOLUTION_IS_MAIL_MESSAGE (conversation));

  revolution_mail_conversation_clear (self);
  g_set_object (&self->session, session);
  g_set_object (&self->folder, folder);
  g_set_object (&self->conversation, conversation);

  members = revolution_mail_message_get_members (conversation);
  if (members != NULL)
    self->items_handler = g_signal_connect_object (members, "items-changed", G_CALLBACK (on_members_changed), self,
                                                   G_CONNECT_SWAPPED);
  sync_entries (self, TRUE);
  gtk_adjustment_set_value (gtk_scrolled_window_get_vadjustment (self->scroller), 0);
  g_debug ("Konversation: %s mit %u Nachrichten angezeigt", revolution_mail_message_get_uid (conversation),
           members != NULL ? g_list_model_get_n_items (members) : 0);
}

/* Keine Konversation gewählt: Einträge und Lesebereiche (WebViews) entfernen. */
void
revolution_mail_conversation_clear (RevolutionMailConversation *self)
{
  GtkWidget *child;

  g_return_if_fail (REVOLUTION_IS_MAIL_CONVERSATION (self));

  if (self->conversation != NULL)
    {
      GListModel *members = revolution_mail_message_get_members (self->conversation);

      if (members != NULL)
        g_clear_signal_handler (&self->items_handler, members);
    }
  self->items_handler = 0;
  g_hash_table_remove_all (self->shown);
  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self->entries))) != NULL)
    gtk_box_remove (self->entries, child);
  g_clear_object (&self->conversation);
  g_clear_object (&self->folder);
}

static void
revolution_mail_conversation_dispose (GObject *object)
{
  RevolutionMailConversation *self = REVOLUTION_MAIL_CONVERSATION (object);

  if (self->shown != NULL)
    revolution_mail_conversation_clear (self);
  g_clear_pointer (&self->shown, g_hash_table_unref);
  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_CONVERSATION);
  g_clear_object (&self->session);

  G_OBJECT_CLASS (revolution_mail_conversation_parent_class)->dispose (object);
}

static void
revolution_mail_conversation_class_init (RevolutionMailConversationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = revolution_mail_conversation_dispose;

  signals[SIGNAL_SHOWN] = g_signal_new ("shown", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                        G_TYPE_NONE, 1, REVOLUTION_TYPE_MAIL_MESSAGE);
  signals[SIGNAL_MARK] = g_signal_new ("mark", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                       G_TYPE_NONE, 3, REVOLUTION_TYPE_MAIL_MESSAGE, G_TYPE_UINT, G_TYPE_UINT);

  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/mail-conversation.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailConversation, scroller);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailConversation, subject_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailConversation, count_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailConversation, entries);
}

static void
revolution_mail_conversation_init (RevolutionMailConversation *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));
  self->shown = g_hash_table_new_full (NULL, NULL, g_object_unref, NULL);
}
