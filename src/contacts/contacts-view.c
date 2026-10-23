/* contacts-view.c – Kontakte-Ansicht (Prototyp #338)
 *
 * Vorlage: quelltext/stamp/src/views/contact/stamp-contact-view.c (Template-
 * Klasse mit Eigenschaft „stack“ für den Ansichtsumschalter), Muster wie
 * mail-view.c. Oberfläche in contacts-view.blp; hier nur Template-Anbindung.
 * Die Modelle sind leer bis M6; Aktionen ohne Funktion sind nicht registriert,
 * ihre Knöpfe und Menüeinträge sind daher ausgegraut (#342).
 * #402: Suche, Seitenleiste und Klasse .dark
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
  GtkWidget           *letter_bar;
};

G_DEFINE_FINAL_TYPE (RevolutionContactsView, revolution_contacts_view, ADW_TYPE_BREAKPOINT_BIN)

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

  g_object_class_install_property (object_class, PROP_STACK,
                                   g_param_spec_object ("stack", NULL, NULL, ADW_TYPE_VIEW_STACK,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_type_ensure (REVOLUTION_TYPE_CONTACTS_SAMPLE_ENTRY);

  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/contacts-view.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, book_selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, contact_selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, header_stack);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, search_entry);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, search_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, split);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, letter_bar);
  gtk_widget_class_bind_template_callback (widget_class, on_search_toggled);
  gtk_widget_class_bind_template_callback (widget_class, on_search_started);
  gtk_widget_class_bind_template_callback (widget_class, on_search_stop);
}

static void
revolution_contacts_view_init (RevolutionContactsView *self)
{
  g_autoptr (GListStore) books = g_list_store_new (REVOLUTION_TYPE_CONTACTS_SAMPLE_ENTRY);
  g_autoptr (GListStore) contacts = g_list_store_new (REVOLUTION_TYPE_CONTACTS_SAMPLE_ENTRY);
  g_autoptr (GSimpleActionGroup) contacts_group = g_simple_action_group_new ();
  g_autoptr (GPropertyAction) sidebar_action = NULL;
  AdwStyleManager *style_manager = adw_style_manager_get_default ();

  gtk_widget_init_template (GTK_WIDGET (self));

  gtk_single_selection_set_model (self->book_selection, G_LIST_MODEL (books));
  gtk_single_selection_set_model (self->contact_selection, G_LIST_MODEL (contacts));

  /* #402 Punkt 4: Buchstabenleiste A–Z, je Buchstabe ein Knopf auf contacts.jump
   * (Sprung folgt in M6, bis dahin ausgegraut). */
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

  g_action_map_add_action_entries (G_ACTION_MAP (contacts_group), contacts_actions, G_N_ELEMENTS (contacts_actions), self);
  /* contacts.sidebar für F9 wie mail.sidebar (mail-view.c). */
  sidebar_action = g_property_action_new ("sidebar", self->split, "show-sidebar");
  g_action_map_add_action (G_ACTION_MAP (contacts_group), G_ACTION (sidebar_action));
  gtk_widget_insert_action_group (GTK_WIDGET (self), "contacts", G_ACTION_GROUP (contacts_group));

  g_signal_connect_object (style_manager, "notify::dark", G_CALLBACK (on_dark_changed), self, G_CONNECT_DEFAULT);
  on_dark_changed (style_manager, NULL, self);
}
