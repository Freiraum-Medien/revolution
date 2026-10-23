/* composer.c – Composer-Fenster (Prototyp #337)
 *
 * Vorlage: quelltext/stamp/src/views/mail/composer/stamp-composer.c
 * (AdwApplicationWindow als Template-Klasse, Aktionsgruppe „composer“).
 * Oberfläche in composer.blp; hier nur Template-Anbindung. Aktionen ohne
 * Funktion (Senden, Anhang, Formatierung) sind nicht registriert, ihre Knöpfe
 * sind daher ausgegraut (#342); sie folgen mit #359 und #360.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "composer.h"

struct _RevolutionComposer
{
  AdwApplicationWindow parent_instance;

  GtkWidget *format_button;
  GtkWidget *cc_entry;
  GtkWidget *bcc_entry;
  GtkWidget *body_view;
  GtkWidget *heading_dropdown;
};

G_DEFINE_FINAL_TYPE (RevolutionComposer, revolution_composer, ADW_TYPE_APPLICATION_WINDOW)

/* #402: Text-Mail blendet die Formatierungsleiste aus (Knopf „Formatierung anzeigen“
 * aus und gesperrt), HTML-Mail zeigt sie wieder. */
static void
change_mail_format (GSimpleAction *action,
                    GVariant      *value,
                    gpointer       user_data)
{
  RevolutionComposer *self = user_data;
  gboolean html = g_strcmp0 (g_variant_get_string (value, NULL), "html") == 0;

  g_simple_action_set_state (action, value);
  gtk_widget_set_sensitive (self->format_button, html);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->format_button), html);
}

/* #402: Kopie/Blindkopie getrennt ein-/ausblenden (Hauptmenü, Haken = sichtbar);
 * Beschriftung und Trennlinie folgen per Bindung (composer.blp). */
static void
change_show_cc (GSimpleAction *action,
                GVariant      *value,
                gpointer       user_data)
{
  RevolutionComposer *self = user_data;

  g_simple_action_set_state (action, value);
  gtk_widget_set_visible (self->cc_entry, g_variant_get_boolean (value));
}

static void
change_show_bcc (GSimpleAction *action,
                 GVariant      *value,
                 gpointer       user_data)
{
  RevolutionComposer *self = user_data;

  g_simple_action_set_state (action, value);
  gtk_widget_set_visible (self->bcc_entry, g_variant_get_boolean (value));
}

/* #402: Emoji-Auswahl (GtkEmojiChooser) am Textfeld, wie Strg+. im Textfeld. */
static void
insert_emoji (GSimpleAction *action G_GNUC_UNUSED,
              GVariant      *parameter G_GNUC_UNUSED,
              gpointer       user_data)
{
  RevolutionComposer *self = user_data;

  gtk_widget_grab_focus (self->body_view);
  g_signal_emit_by_name (self->body_view, "insert-emoji");
}

static const GActionEntry composer_actions[] = {
  { "show-cc", NULL, NULL, "true", change_show_cc, { 0 } },
  { "show-bcc", NULL, NULL, "true", change_show_bcc, { 0 } },
  { "mail-format", NULL, "s", "'html'", change_mail_format, { 0 } },
  { "insert-emoji", insert_emoji, NULL, NULL, NULL, { 0 } },
};

/* #402: Klasse .dark für Regeln nur im dunklen Design (style.css, Abschnitt
 * „Composer (#402)“), wie mail-view.c on_dark_changed (#401 Punkt 1). */
static void
on_dark_changed (AdwStyleManager    *manager,
                 GParamSpec         *pspec G_GNUC_UNUSED,
                 RevolutionComposer *self)
{
  if (adw_style_manager_get_dark (manager))
    gtk_widget_add_css_class (GTK_WIDGET (self), "dark");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "dark");
}

static void
revolution_composer_dispose (GObject *object)
{
  gtk_widget_dispose_template (GTK_WIDGET (object), REVOLUTION_TYPE_COMPOSER);

  G_OBJECT_CLASS (revolution_composer_parent_class)->dispose (object);
}

static void
revolution_composer_class_init (RevolutionComposerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = revolution_composer_dispose;
  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/composer.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionComposer, format_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionComposer, cc_entry);
  gtk_widget_class_bind_template_child (widget_class, RevolutionComposer, bcc_entry);
  gtk_widget_class_bind_template_child (widget_class, RevolutionComposer, body_view);
  gtk_widget_class_bind_template_child (widget_class, RevolutionComposer, heading_dropdown);
}

static void
revolution_composer_init (RevolutionComposer *self)
{
  g_autoptr (GSimpleActionGroup) group = g_simple_action_group_new ();
  g_autoptr (GPropertyAction) heading = NULL;
  AdwStyleManager *style_manager = adw_style_manager_get_default ();

  gtk_widget_init_template (GTK_WIDGET (self));

  g_action_map_add_action_entries (G_ACTION_MAP (group), composer_actions, G_N_ELEMENTS (composer_actions), self);

  /* #402: Platzhalter-Aktion Absatzformat, spiegelt die Auswahl der Liste. */
  heading = g_property_action_new ("heading", self->heading_dropdown, "selected");
  g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (heading));
  gtk_widget_insert_action_group (GTK_WIDGET (self), "composer", G_ACTION_GROUP (group));

  g_signal_connect_object (style_manager, "notify::dark", G_CALLBACK (on_dark_changed), self, G_CONNECT_DEFAULT);
  on_dark_changed (style_manager, NULL, self);
}

GtkWindow *
revolution_composer_new (GtkApplication *application)
{
  return g_object_new (REVOLUTION_TYPE_COMPOSER, "application", application, NULL);
}
