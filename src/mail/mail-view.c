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

#include "mail-sample.h"
#include "mail-view.h"

struct _RevolutionMailView
{
  AdwBreakpointBin    parent_instance;
  AdwViewStack       *stack;
  GtkSingleSelection *folder_selection;
  GtkSingleSelection *message_selection;
};

G_DEFINE_FINAL_TYPE (RevolutionMailView, revolution_mail_view, ADW_TYPE_BREAKPOINT_BIN)

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

  g_object_class_install_property (object_class, PROP_STACK,
                                   g_param_spec_object ("stack", NULL, NULL, ADW_TYPE_VIEW_STACK,
                                                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_type_ensure (REVOLUTION_TYPE_SAMPLE_FOLDER);
  g_type_ensure (REVOLUTION_TYPE_SAMPLE_MESSAGE);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/revolution/Mail/mail-view.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, folder_selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailView, message_selection);
}

static void
revolution_mail_view_init (RevolutionMailView *self)
{
  g_autoptr (GListModel) folders = revolution_mail_sample_folders ();
  g_autoptr (GListModel) messages = revolution_mail_sample_messages ();

  gtk_widget_init_template (GTK_WIDGET (self));

  gtk_single_selection_set_model (self->folder_selection, folders);
  gtk_single_selection_set_model (self->message_selection, messages);
}
