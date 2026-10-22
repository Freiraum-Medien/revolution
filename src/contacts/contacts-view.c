/* contacts-view.c – Kontakte-Ansicht (Prototyp #338)
 *
 * Vorlage: quelltext/stamp/src/views/contact/stamp-contact-view.c (Template-
 * Klasse mit Eigenschaft „stack“ für den Ansichtsumschalter), Muster wie
 * mail-view.c. Oberfläche in contacts-view.blp; hier nur Template-Anbindung
 * und Befüllen mit Beispieldaten.
 * #402: Suche, Seitenleiste, Umschalter-Tooltip, Fokus-Scrollen und Klasse .dark
 * übernommen aus mail-view.c (#399–#401).
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>

#include "contacts-sample.h"
#include "contacts-view.h"

struct _RevolutionContactsView
{
  AdwBreakpointBin     parent_instance;
  AdwViewStack        *stack;
  GtkSingleSelection  *book_selection;
  GtkSingleSelection  *contact_selection;
  GtkStack            *header_stack;
  GtkSearchEntry      *search_entry;
  GtkToggleButton     *search_button;
  AdwOverlaySplitView *split;
  GtkViewport         *book_viewport;
  GtkWidget           *book_list;
  GtkWidget           *switcher;
  GtkWidget           *link_notice;
  GtkWidget           *letter_bar;
  AdwViewStackPage    *firm_page;
  GtkLabel            *firm_label;
};

G_DEFINE_FINAL_TYPE (RevolutionContactsView, revolution_contacts_view, ADW_TYPE_BREAKPOINT_BIN)

/* Platzhalter ohne Logik, nur damit die Knöpfe nicht ausgegraut sind.
 * #402: call, invite, link, share-vcard, show-qr (Schnellaktionen, Teilen-Menü). */
static const GActionEntry contact_actions[] = {
  { "new", NULL, NULL, NULL, NULL, { 0 } },
  { "edit", NULL, NULL, NULL, NULL, { 0 } },
  { "write-mail", NULL, NULL, NULL, NULL, { 0 } },
  { "call", NULL, NULL, NULL, NULL, { 0 } },
  { "invite", NULL, NULL, NULL, NULL, { 0 } },
  { "link", NULL, NULL, NULL, NULL, { 0 } },
  { "share-vcard", NULL, NULL, NULL, NULL, { 0 } },
  { "show-qr", NULL, NULL, NULL, NULL, { 0 } },
};

/* Wählt in der Kontaktliste den Eintrag mit diesem Namen (Firma-Reiter, Start). */
static void
select_contact (RevolutionContactsView *self,
                const char             *title)
{
  GListModel *model = gtk_single_selection_get_model (self->contact_selection);
  guint n = g_list_model_get_n_items (model);

  for (guint i = 0; i < n; i++)
    {
      g_autoptr (GObject) entry = g_list_model_get_item (model, i);
      g_autofree char *entry_title = NULL;

      g_object_get (entry, "title", &entry_title, NULL);
      if (g_strcmp0 (entry_title, title) == 0)
        {
          gtk_single_selection_set_selected (self->contact_selection, i);
          return;
        }
    }
}

/* #402 Punkt 2: Klick auf einen Kollegen wählt ihn in der Liste, kein Dialog.
 * Der Zeilenname (contacts-view.blp) ist der Kontaktname. */
static void
on_colleague_activated (RevolutionContactsView *self,
                        GtkListBoxRow          *row)
{
  select_contact (self, gtk_widget_get_name (GTK_WIDGET (row)));
}

/* #402 Punkt 3: Schließen-Knopf des Verknüpfungsvorschlags. */
static void
on_link_dismiss (RevolutionContactsView *self)
{
  gtk_widget_set_visible (self->link_notice, FALSE);
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

/* preferences, shortcuts, about: Platzhalter des Primärmenüs wie mail-view.c. */
static const GActionEntry contacts_actions[] = {
  { "search", on_search, NULL, NULL, NULL, { 0 } },
  /* #402 Punkt 4: Buchstabenleiste, Platzhalter ohne Sprung. */
  { "jump", NULL, "s", NULL, NULL, { 0 } },
  { "toggle-sidebar", on_toggle_sidebar, NULL, NULL, NULL, { 0 } },
  { "preferences", NULL, NULL, NULL, NULL, { 0 } },
  { "shortcuts", NULL, NULL, NULL, NULL, { 0 } },
  { "about", NULL, NULL, NULL, NULL, { 0 } },
};

/* Wie mail-view.c (#400 Punkt 2): der fokussierte Eintrag scrollt bis über die
 * Oberkante des schwebenden Umschalters statt nur bis zum Rand des Viewports. */
static void
on_focus_widget (GtkRoot                *root,
                 GParamSpec             *pspec G_GNUC_UNUSED,
                 RevolutionContactsView *self)
{
  GtkWidget *focus = gtk_root_get_focus (root);
  GtkWidget *viewport = GTK_WIDGET (self->book_viewport);
  GtkAdjustment *adjustment;
  graphene_rect_t row, switcher;
  double bottom, value;

  if (focus == NULL || !gtk_widget_is_ancestor (focus, self->book_list) ||
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
revolution_contacts_view_root (GtkWidget *widget)
{
  GTK_WIDGET_CLASS (revolution_contacts_view_parent_class)->root (widget);

  g_signal_connect_object (gtk_widget_get_root (widget), "notify::focus-widget",
                           G_CALLBACK (on_focus_widget), widget, G_CONNECT_DEFAULT);
}

static void
revolution_contacts_view_unroot (GtkWidget *widget)
{
  g_signal_handlers_disconnect_by_func (gtk_widget_get_root (widget), on_focus_widget, widget);

  GTK_WIDGET_CLASS (revolution_contacts_view_parent_class)->unroot (widget);
}

/* Umschalter nur mit Symbolen: Seitentitel als Tooltip (wie mail-view.c, #401). */
static gboolean
on_switcher_query_tooltip (RevolutionContactsView *self,
                           int                     x,
                           int                     y,
                           gboolean                keyboard,
                           GtkTooltip             *tooltip,
                           GtkWidget              *switcher)
{
  g_autoptr (GtkSelectionModel) pages = NULL;
  g_autoptr (AdwViewStackPage) page = NULL;
  GtkWidget *button;
  GtkWidget *child;
  guint index = 0;

  if (keyboard)
    button = gtk_root_get_focus (gtk_widget_get_root (switcher));
  else
    button = gtk_widget_pick (switcher, x, y, GTK_PICK_DEFAULT);

  while (button != NULL && button != switcher && !GTK_IS_TOGGLE_BUTTON (button))
    button = gtk_widget_get_parent (button);

  if (button == NULL || button == switcher || self->stack == NULL)
    return FALSE;

  for (child = gtk_widget_get_first_child (switcher); child != NULL && child != button;
       child = gtk_widget_get_next_sibling (child))
    if (GTK_IS_TOGGLE_BUTTON (child))
      index++;

  if (child == NULL)
    return FALSE;

  pages = adw_view_stack_get_pages (self->stack);
  page = g_list_model_get_item (G_LIST_MODEL (pages), index);
  if (page == NULL)
    return FALSE;

  gtk_tooltip_set_text (tooltip, adw_view_stack_page_get_title (page));
  return TRUE;
}

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

  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_CONTACTS_VIEW);
  g_clear_object (&self->stack);

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
  widget_class->root = revolution_contacts_view_root;
  widget_class->unroot = revolution_contacts_view_unroot;

  g_object_class_install_property (object_class, PROP_STACK,
                                   g_param_spec_object ("stack", NULL, NULL, ADW_TYPE_VIEW_STACK,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_type_ensure (REVOLUTION_TYPE_CONTACTS_SAMPLE_ENTRY);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/revolution/Mail/contacts-view.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, book_selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, contact_selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, header_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, search_entry);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, search_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, split);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, book_viewport);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, book_list);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, switcher);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, link_notice);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, letter_bar);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, firm_page);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, firm_label);
  gtk_widget_class_bind_template_callback (widget_class, on_search_toggled);
  gtk_widget_class_bind_template_callback (widget_class, on_switcher_query_tooltip);
  gtk_widget_class_bind_template_callback (widget_class, on_search_started);
  gtk_widget_class_bind_template_callback (widget_class, on_search_stop);
  gtk_widget_class_bind_template_callback (widget_class, on_colleague_activated);
  gtk_widget_class_bind_template_callback (widget_class, on_link_dismiss);
}

static void
revolution_contacts_view_init (RevolutionContactsView *self)
{
  g_autoptr (GListModel) books = revolution_contacts_sample_books ();
  g_autoptr (GListModel) contacts = revolution_contacts_sample_contacts ();
  g_autoptr (GSimpleActionGroup) contact_group = g_simple_action_group_new ();
  g_autoptr (GSimpleActionGroup) contacts_group = g_simple_action_group_new ();
  g_autoptr (GPropertyAction) sidebar_action = NULL;
  AdwStyleManager *style_manager = adw_style_manager_get_default ();

  gtk_widget_init_template (GTK_WIDGET (self));

  gtk_single_selection_set_model (self->book_selection, books);
  gtk_single_selection_set_model (self->contact_selection, contacts);
  /* #402: angezeigt wird Jana Albrecht (Mail- und Kalender-Beispiele). */
  select_contact (self, _("Jana Albrecht"));

  /* #402 Punkt 2: Reiter „Firma“ nur mit eingetragener Firma. */
  adw_view_stack_page_set_visible (self->firm_page, *gtk_label_get_text (self->firm_label) != '\0');

  /* #402 Punkt 4: Buchstabenleiste A–Z, je Buchstabe ein Knopf auf contacts.jump. */
  for (char letter = 'A'; letter <= 'Z'; letter++)
    {
      char text[2] = { letter, '\0' };
      g_autofree char *tooltip = g_strdup_printf (_("Zu „%s“ springen"), text);
      GtkWidget *button = gtk_button_new_with_label (text);

      gtk_actionable_set_action_target (GTK_ACTIONABLE (button), "s", text);
      gtk_actionable_set_action_name (GTK_ACTIONABLE (button), "contacts.jump");
      gtk_widget_set_tooltip_text (button, tooltip);
      gtk_accessible_update_property (GTK_ACCESSIBLE (button), GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip, -1);
      gtk_widget_add_css_class (button, "flat");
      gtk_box_append (GTK_BOX (self->letter_bar), button);
    }

  g_action_map_add_action_entries (G_ACTION_MAP (contact_group), contact_actions, G_N_ELEMENTS (contact_actions), NULL);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "contact", G_ACTION_GROUP (contact_group));
  g_action_map_add_action_entries (G_ACTION_MAP (contacts_group), contacts_actions, G_N_ELEMENTS (contacts_actions), self);
  /* contacts.sidebar für F9 wie mail.sidebar (mail-view.c). */
  sidebar_action = g_property_action_new ("sidebar", self->split, "show-sidebar");
  g_action_map_add_action (G_ACTION_MAP (contacts_group), G_ACTION (sidebar_action));
  gtk_widget_insert_action_group (GTK_WIDGET (self), "contacts", G_ACTION_GROUP (contacts_group));

  g_signal_connect_object (style_manager, "notify::dark", G_CALLBACK (on_dark_changed), self, G_CONNECT_DEFAULT);
  on_dark_changed (style_manager, NULL, self);
}
