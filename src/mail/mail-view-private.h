/* mail-view-private.h – gemeinsamer Zustand der Mail-Ansicht (#444) wie gtk/gtkwindowprivate.h: struct
 * _RevolutionMailView, Typen und Makro aus mail-view.c, Funktionen, die mail-view.c, mail-list.c, mail-queue.c,
 * mail-actions.c und mail-folder-actions.c gegenseitig aufrufen.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "mail-conversation.h"
#include "mail-reader.h"
#include "mail-view.h"

G_BEGIN_DECLS

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
  /* #365: Suche (Ausdruck aus mail-search.c oder NULL), „Alle Ordner“, Suchbereich und Filter (list.filter);
   * die vor der Suche gewählte Nachricht und ob sie nach dem Ende wieder gewählt wird (select_message_from_env). */
  AdwToggleGroup      *search_scope;
  AdwStatusPage       *empty_page;
  GtkWidget           *folder_list;
  char                *search;
  gboolean             search_all;
  char                *filter;
  char                *return_uid;
  gboolean             returning;
  /* #366: Ordner („UID/voller Name“) und Nachricht aus der Benachrichtigung (revolution_mail_view_show_message). */
  char                *show_folder;
  char                *show_uid;
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
  /* #364: Kontextmenü und Aktionen „folder.*“ des Ordnerbaums. */
  GMenuModel          *folder_menu;
  GtkWidget           *folder_popover;
  GActionMap          *folder_actions;
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
  /* #362: Abschnitt „Auf diesem Rechner“ mit dem Postausgang, nur wenn er nicht leer ist
   * (revolution_mail_view_set_outbox); local_section leer oder mit local. */
  RevolutionMailAccount *local;
  GListStore           *local_section;
  /* #357: Konversationen (GSettings „conversations“, beim Wählen des Ordners übernommen), Lesebereich
   * der Konversation, Suchordner aus Ordner und Gesendet zum Laden ihrer Nachrichten (mail-thread.c)
   * und Gelesen-Timer je angezeigter Nachricht (GSource). */
  gboolean             conversations;
  RevolutionMailConversation *conversation_view;
  RevolutionMailFolder *thread_folder;
  GPtrArray           *member_seen;
  /* #363: Ziele für Verschieben (Ordner des Kontos move_account), Aufträge für Rückgängig, Hinweis dazu. */
  GtkMenuButton       *move_button;
  GMenu               *move_menu;
  char                *move_account;
  GPtrArray           *undo;
  AdwToast            *toast;
};

/* #409: echter Ordner und UID eines Auftrags, im Worker beim Start aufgelöst (set_flags_thread). */
typedef struct
{
  CamelFolder *folder;
  char        *uid;
} FlagsLocation;

/* #363: Verschieben läuft in derselben Warteschlange wie die Flags (transfer_thread, on_transferred). */
typedef struct _Transfer Transfer;

typedef struct _SetFlags
{
  /* #363: Verschieben statt Flags, sonst NULL. */
  Transfer    *transfer;
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

enum { FLAGS_SAVED, FLAGS_LOCAL };

#define FLAGS_DEADLINE_SECONDS 10

/* mail-view.c */
void set_empty_page (RevolutionMailView *self, gboolean searching);
RevolutionMailAccount *find_account (RevolutionMailView *self, const char *uid);
GtkTreeListModel *account_tree (RevolutionMailAccount *account);
void select_folder (RevolutionMailView *self, RevolutionMailFolder *target);
GListModel *account_folder_store (RevolutionMailAccount *account);
/* mail-list.c */
gboolean select_shown (RevolutionMailView *self);
void set_list_counts (RevolutionMailView *self);
void start_folder_changes (RevolutionMailView *self);
RevolutionMailFolder *find_folder_type (GListModel *model, guint32 type);
void on_folder_selected (RevolutionMailView *self);
void on_message_selected (RevolutionMailView *self);
/* mail-queue.c */
void set_flags_free (SetFlags *data);
CamelFolder *real_folder (CamelFolder *folder, const char *uid, char **real_uid);
gboolean flags_deadline (gpointer user_data);
void start_flags (RevolutionMailView *self);
void mark_message (RevolutionMailView *self, RevolutionMailMessage *message, guint32 mask, guint32 set, const char *what);
void mark_read (RevolutionMailView *self, RevolutionMailMessage *message, gboolean read, const char *what);
void mark_starred (RevolutionMailView *self, RevolutionMailMessage *message, gboolean starred, const char *what);
RevolutionMailMessage *selected_message (RevolutionMailView *self);
/* mail-actions.c */
void on_forward (GSimpleAction *action, GVariant *parameter, gpointer user_data);
void on_open (GSimpleAction *action, GVariant *parameter, gpointer user_data);
void on_message_activate (RevolutionMailView *self);
void on_reply (GSimpleAction *action, GVariant *parameter, gpointer user_data);
void on_reply_all (GSimpleAction *action, GVariant *parameter, gpointer user_data);
void on_mark_read (GSimpleAction *action, GVariant *parameter, gpointer user_data);
void on_mark_unread (GSimpleAction *action, GVariant *parameter, gpointer user_data);
void on_star_on (GSimpleAction *action, GVariant *parameter, gpointer user_data);
void on_star_off (GSimpleAction *action, GVariant *parameter, gpointer user_data);
void on_star_change_state (GSimpleAction *action, GVariant *state, gpointer user_data);
void on_add_sender (GSimpleAction *action, GVariant *parameter, gpointer user_data);
void update_message_actions (RevolutionMailView *self);
void watch_selected_message (RevolutionMailView *self);
void on_reader_shown (RevolutionMailView *self);
void clear_member_seen (RevolutionMailView *self);
void on_conversation_shown (RevolutionMailView *self, RevolutionMailMessage *member);
void on_conversation_mark (RevolutionMailView *self, RevolutionMailMessage *member, guint mask, guint set);
void show_row_menu (RevolutionMailView *self, GtkWidget *widget, double x, double y);
RevolutionMailView *view_for (GtkEventController *controller);
void on_row_secondary_click (GtkListItem *item, int n_press, double x, double y, GtkGestureClick *gesture);
void on_context_menu (GSimpleAction *action, GVariant *parameter, gpointer user_data);
void on_row_swipe (GtkListItem *item, double velocity_x, double velocity_y, GtkGestureSwipe *gesture);
void transfer_release (Transfer *t);
void transfer_thread (GTask *task, SetFlags *data);
void on_transferred (RevolutionMailView *self, SetFlags *data, GError *error);
void on_move_popup (GtkMenuButton *button, gpointer user_data);
void on_transfer_action (GSimpleAction *action, GVariant *parameter, gpointer user_data);
GdkContentProvider *on_row_drag_prepare (GtkListItem *item, double x, double y, GtkDragSource *source);
gboolean on_folder_drop (GtkListItem *item, const GValue *value, double x, double y, GtkDropTarget *target);
/* mail-folder-actions.c */
void update_folder_actions (RevolutionMailView *self);
void on_folder_edit (GSimpleAction *action, GVariant *parameter, gpointer user_data);
gboolean on_folder_delete_key (GtkWidget *widget, GVariant *args, gpointer user_data);
void on_folder_secondary_click (GtkListItem *item, int n_press, double x, double y, GtkGestureClick *gesture);

G_END_DECLS
