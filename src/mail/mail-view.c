/* mail-view.c – Mail-Ansicht (Prototyp #336)
 *
 * Vorlage: quelltext/stamp/src/views/mail/stamp-mail-view.c (Template-Klasse
 * mit Eigenschaft „stack“ für den Ansichtsumschalter). Oberfläche in
 * mail-view.blp; hier nur Template-Anbindung und Befüllen mit Beispieldaten.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "composer/composer.h"
#include "mail-sample.h"
#include "mail-view.h"

struct _RevolutionMailView
{
  AdwBreakpointBin     parent_instance;
  AdwViewStack        *stack;
  GtkSingleSelection  *folder_selection;
  GtkSingleSelection  *message_selection;
  GtkStack            *header_stack;
  GtkSearchEntry      *search_entry;
  GtkToggleButton     *search_button;
  AdwOverlaySplitView *split;
  GtkViewport         *folder_viewport;
  GtkWidget           *folder_list;
  GtkWidget           *switcher;
};

G_DEFINE_FINAL_TYPE (RevolutionMailView, revolution_mail_view, ADW_TYPE_BREAKPOINT_BIN)

/* list.compose öffnet den Composer (#337) als eigenes Fenster wie Stamp. */
static void
on_compose (GSimpleAction *action G_GNUC_UNUSED,
            GVariant      *parameter G_GNUC_UNUSED,
            gpointer       user_data)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (user_data));

  gtk_window_present (revolution_composer_new (gtk_window_get_application (GTK_WINDOW (root))));
}

/* Platzhalter ohne Logik, nur damit die Menüeinträge nicht ausgegraut sind.
 * list.filter ist zustandsbehaftet: ohne Handler setzt die Aktivierung nur
 * den Zustand (Radio-Marke im Menü), gefiltert wird nichts. */
static const GActionEntry list_actions[] = {
  { "filter", NULL, "s", "'all'", NULL, { 0 } },
  { "compose", on_compose, NULL, NULL, NULL, { 0 } },
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

/* preferences, shortcuts, about: Platzhalter des Primärmenüs (#399 Punkt 3). */
static const GActionEntry mail_actions[] = {
  { "search", on_search, NULL, NULL, NULL, { 0 } },
  { "preferences", NULL, NULL, NULL, NULL, { 0 } },
  { "shortcuts", NULL, NULL, NULL, NULL, { 0 } },
  { "about", NULL, NULL, NULL, NULL, { 0 } },
};

static const GActionEntry message_actions[] = {
  { "move", NULL, "s", NULL, NULL, { 0 } },
  { "mark-unread", NULL, NULL, NULL, NULL, { 0 } },
  { "print", NULL, NULL, NULL, NULL, { 0 } },
};

/* #400 Punkt 2: der Umschalter schwebt über dem unteren Rand der Ordnerliste.
 * Statt GtkViewport:scroll-to-focus (scrollt nur bis zum Rand des Viewports)
 * scrollt der fokussierte Ordner bis über die Oberkante des Umschalters. */
static void
on_focus_widget (GtkRoot            *root,
                 GParamSpec         *pspec G_GNUC_UNUSED,
                 RevolutionMailView *self)
{
  GtkWidget *focus = gtk_root_get_focus (root);
  GtkWidget *viewport = GTK_WIDGET (self->folder_viewport);
  GtkAdjustment *adjustment;
  graphene_rect_t row, switcher;
  double bottom, value;

  if (focus == NULL || !gtk_widget_is_ancestor (focus, self->folder_list) ||
      !gtk_widget_compute_bounds (focus, viewport, &row))
    return;

  adjustment = gtk_scrollable_get_vadjustment (GTK_SCROLLABLE (viewport));
  value = gtk_adjustment_get_value (adjustment);
  bottom = gtk_widget_get_height (viewport);

  /* Nur wenn der Umschalter über diesem Viewport liegt (Layout „desktop“). */
  if (gtk_widget_get_mapped (self->switcher) &&
      gtk_widget_compute_bounds (self->switcher, viewport, &switcher) &&
      switcher.origin.y > 0 && switcher.origin.y < bottom)
    bottom = switcher.origin.y - gtk_widget_get_margin_top (self->switcher);

  if (row.origin.y < 0)
    gtk_adjustment_set_value (adjustment, value + row.origin.y);
  else if (row.origin.y + row.size.height > bottom)
    gtk_adjustment_set_value (adjustment, value + row.origin.y + row.size.height - bottom);
}

static void
revolution_mail_view_root (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (revolution_mail_view_parent_class)->root (widget);

  g_signal_connect_object (gtk_widget_get_root (widget), "notify::focus-widget",
                           G_CALLBACK (on_focus_widget), widget, G_CONNECT_DEFAULT);
}

static void
revolution_mail_view_unroot (GtkWidget *widget)
{
  g_signal_handlers_disconnect_by_func (gtk_widget_get_root (widget), on_focus_widget, widget);

  GTK_WIDGET_CLASS (revolution_mail_view_parent_class)->unroot (widget);
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

  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_VIEW);
  g_clear_object (&self->stack);

  G_OBJECT_CLASS (revolution_mail_view_parent_class)->dispose (object);
}

static void
revolution_mail_view_class_init (RevolutionMailViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = revolution_mail_view_get_property;
  object_class->set_property = revolution_mail_view_set_property;
  object_class->dispose = revolution_mail_view_dispose;
  widget_class->root = revolution_mail_view_root;
  widget_class->unroot = revolution_mail_view_unroot;

  g_object_class_install_property (object_class, PROP_STACK,
                                   g_param_spec_object ("stack", NULL, NULL, ADW_TYPE_VIEW_STACK,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_type_ensure (REVOLUTION_TYPE_SAMPLE_FOLDER);
  g_type_ensure (REVOLUTION_TYPE_SAMPLE_MESSAGE);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/revolution/Mail/mail-view.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, folder_selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, message_selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, header_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, search_entry);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, search_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, split);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, folder_viewport);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, folder_list);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, switcher);
  gtk_widget_class_bind_template_callback (widget_class, on_search_toggled);
  gtk_widget_class_bind_template_callback (widget_class, on_search_started);
  gtk_widget_class_bind_template_callback (widget_class, on_search_stop);
}

static void
revolution_mail_view_init (RevolutionMailView *self)
{
  g_autoptr (GListModel) folders = revolution_mail_sample_folders ();
  g_autoptr (GListModel) messages = revolution_mail_sample_messages ();
  g_autoptr (GSimpleActionGroup) list_group = g_simple_action_group_new ();
  g_autoptr (GSimpleActionGroup) message_group = g_simple_action_group_new ();
  g_autoptr (GSimpleActionGroup) mail_group = g_simple_action_group_new ();
  g_autoptr (GPropertyAction) sidebar_action = NULL;

  gtk_widget_init_template (GTK_WIDGET (self));

  gtk_single_selection_set_model (self->folder_selection, folders);
  gtk_single_selection_set_model (self->message_selection, messages);
  /* #400 Punkt 7: angezeigt wird die HTML-Mail der Stadtwerke (3. Zeile, mail-sample.c). */
  gtk_single_selection_set_selected (self->message_selection, 2);

  g_action_map_add_action_entries (G_ACTION_MAP (list_group), list_actions, G_N_ELEMENTS (list_actions), self);
  g_action_map_add_action_entries (G_ACTION_MAP (message_group), message_actions, G_N_ELEMENTS (message_actions), NULL);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "list", G_ACTION_GROUP (list_group));
  gtk_widget_insert_action_group (GTK_WIDGET (self), "message", G_ACTION_GROUP (message_group));
  g_action_map_add_action_entries (G_ACTION_MAP (mail_group), mail_actions, G_N_ELEMENTS (mail_actions), self);
  /* mail.sidebar für F9 wie calendar.sidebar (calendar-view.c:426-431). */
  sidebar_action = g_property_action_new ("sidebar", self->split, "show-sidebar");
  g_action_map_add_action (G_ACTION_MAP (mail_group), G_ACTION (sidebar_action));
  gtk_widget_insert_action_group (GTK_WIDGET (self), "mail", G_ACTION_GROUP (mail_group));
}
