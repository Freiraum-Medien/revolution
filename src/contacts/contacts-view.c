/* contacts-view.c – Kontakte-Ansicht (Prototyp #338)
 *
 * Vorlage: quelltext/stamp/src/views/contact/stamp-contact-view.c (Template-
 * Klasse mit Eigenschaft „stack“ für den Ansichtsumschalter), Muster wie
 * mail-view.c. Oberfläche in contacts-view.blp; hier nur Template-Anbindung
 * und Befüllen mit Beispieldaten.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "contacts-sample.h"
#include "contacts-view.h"

struct _RevolutionContactsView
{
  AdwBreakpointBin    parent_instance;
  AdwViewStack       *stack;
  GtkSingleSelection *book_selection;
  GtkSingleSelection *contact_selection;
};

G_DEFINE_FINAL_TYPE (RevolutionContactsView, revolution_contacts_view, ADW_TYPE_BREAKPOINT_BIN)

/* Platzhalter ohne Logik, nur damit die Knöpfe nicht ausgegraut sind. */
static const GActionEntry contact_actions[] = {
  { "new", NULL, NULL, NULL, NULL, { 0 } },
  { "edit", NULL, NULL, NULL, NULL, { 0 } },
  { "write-mail", NULL, NULL, NULL, NULL, { 0 } },
};

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

  g_type_ensure (REVOLUTION_TYPE_SAMPLE_ENTRY);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/revolution/Mail/contacts-view.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, book_selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionContactsView, contact_selection);
}

static void
revolution_contacts_view_init (RevolutionContactsView *self)
{
  g_autoptr (GListModel) books = revolution_contacts_sample_books ();
  g_autoptr (GListModel) contacts = revolution_contacts_sample_contacts ();
  g_autoptr (GSimpleActionGroup) contact_group = g_simple_action_group_new ();

  gtk_widget_init_template (GTK_WIDGET (self));

  gtk_single_selection_set_model (self->book_selection, books);
  gtk_single_selection_set_model (self->contact_selection, contacts);

  g_action_map_add_action_entries (G_ACTION_MAP (contact_group), contact_actions, G_N_ELEMENTS (contact_actions), NULL);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "contact", G_ACTION_GROUP (contact_group));
}
