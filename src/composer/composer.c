/* composer.c – Composer-Fenster (Prototyp #337)
 *
 * Vorlage: quelltext/stamp/src/views/mail/composer/stamp-composer.c
 * (AdwApplicationWindow als Template-Klasse, Aktionsgruppe „composer“).
 * Oberfläche in composer.blp; hier nur Template-Anbindung und
 * Platzhalter-Aktionen ohne Logik.
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
};

G_DEFINE_FINAL_TYPE (RevolutionComposer, revolution_composer, ADW_TYPE_APPLICATION_WINDOW)

/* Platzhalter ohne Logik, nur damit Knöpfe und Kürzel nicht ausgegraut sind.
 * Formatierungen sind boolesch zustandsbehaftet: ohne Handler schaltet die
 * Aktivierung nur den Zustand um (ToggleButton zeigt ihn an). */
static const GActionEntry composer_actions[] = {
  { "send", NULL, NULL, NULL, NULL, { 0 } },
  { "attach", NULL, NULL, NULL, NULL, { 0 } },
  { "open-attachment", NULL, "s", NULL, NULL, { 0 } },
  { "remove-attachment", NULL, "s", NULL, NULL, { 0 } },
  { "bold", NULL, NULL, "false", NULL, { 0 } },
  { "italic", NULL, NULL, "false", NULL, { 0 } },
  { "underline", NULL, NULL, "false", NULL, { 0 } },
  { "strikethrough", NULL, NULL, "false", NULL, { 0 } },
  { "bullet-list", NULL, NULL, "false", NULL, { 0 } },
  { "numbered-list", NULL, NULL, "false", NULL, { 0 } },
  { "insert-link", NULL, NULL, NULL, NULL, { 0 } },
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
revolution_composer_class_init (RevolutionComposerClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/revolution/Mail/composer.ui");
}

static void
revolution_composer_init (RevolutionComposer *self)
{
  g_autoptr (GSimpleActionGroup) group = g_simple_action_group_new ();
  AdwStyleManager *style_manager = adw_style_manager_get_default ();

  gtk_widget_init_template (GTK_WIDGET (self));

  g_action_map_add_action_entries (G_ACTION_MAP (group), composer_actions, G_N_ELEMENTS (composer_actions), NULL);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "composer", G_ACTION_GROUP (group));

  g_signal_connect_object (style_manager, "notify::dark", G_CALLBACK (on_dark_changed), self, G_CONNECT_DEFAULT);
  on_dark_changed (style_manager, NULL, self);
}

GtkWindow *
revolution_composer_new (GtkApplication *application)
{
  return g_object_new (REVOLUTION_TYPE_COMPOSER, "application", application, NULL);
}
