/* main.c
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>
#include <adwaita.h>

#include "contacts/contacts-view.h"
#include "calendar/calendar-view.h"
#include "mail/mail-view.h"

/* Drei Themes (Arnd 22.10.2026, #340): stateful Aktion app.theme (Typ „s“) für den
 * Abschnitt „Darstellung“ im Hauptmenü (mail-, calendar-, contacts-view.blp).
 *   hell         force-light
 *   dunkel-hell  force-dark, Mail-Inhalt und Schreibfläche bleiben hell
 *   dunkel       force-dark und Klasse .content-dark an allen Fenstern (style.css)
 * Startwert: dem System folgen (color-scheme default). Bis zur ersten Wahl zeigt die
 * Radio-Marke das Aussehen des Systems (hell bzw. dunkel-hell, wie bisher).
 * Zum Testen überschreibt die Umgebungsvariable REVOLUTION_THEME den Startwert. */
static void
apply_content_class (GtkWindow  *window,
                     const char *theme)
{
  if (g_strcmp0 (theme, "dunkel") == 0)
    gtk_widget_add_css_class (GTK_WIDGET (window), "content-dark");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (window), "content-dark");
}

static void
change_theme (GSimpleAction *action,
              GVariant      *state,
              gpointer       user_data)
{
  GtkApplication *app = GTK_APPLICATION (user_data);
  const char *theme = g_variant_get_string (state, NULL);
  AdwColorScheme scheme;

  if (g_strcmp0 (theme, "hell") == 0)
    scheme = ADW_COLOR_SCHEME_FORCE_LIGHT;
  else if (g_strcmp0 (theme, "dunkel-hell") == 0 || g_strcmp0 (theme, "dunkel") == 0)
    scheme = ADW_COLOR_SCHEME_FORCE_DARK;
  else
    return;

  /* Erst das Schema, dann der Zustand: on_system_dark_changed greift dann nicht mehr. */
  adw_style_manager_set_color_scheme (adw_style_manager_get_default (), scheme);
  g_simple_action_set_state (action, state);

  for (GList *l = gtk_application_get_windows (app); l != NULL; l = l->next)
    apply_content_class (GTK_WINDOW (l->data), theme);
}

static void
on_window_added (GtkApplication *app,
                 GtkWindow      *window)
{
  g_autoptr (GVariant) state = g_action_group_get_action_state (G_ACTION_GROUP (app), "theme");

  apply_content_class (window, g_variant_get_string (state, NULL));
}

static void
on_system_dark_changed (AdwStyleManager *manager,
                        GParamSpec      *pspec G_GNUC_UNUSED,
                        GSimpleAction   *action)
{
  if (adw_style_manager_get_color_scheme (manager) != ADW_COLOR_SCHEME_DEFAULT)
    return;

  g_simple_action_set_state (action,
                             g_variant_new_string (adw_style_manager_get_dark (manager) ? "dunkel-hell" : "hell"));
}

static void
on_startup (GtkApplication *app)
{
  const GActionEntry app_actions[] = {
    { "theme", NULL, "s", "'hell'", change_theme, { 0 } },
  };
  AdwStyleManager *style_manager = adw_style_manager_get_default ();
  const char *theme = g_getenv ("REVOLUTION_THEME");
  GAction *action;

  g_action_map_add_action_entries (G_ACTION_MAP (app), app_actions, G_N_ELEMENTS (app_actions), app);
  action = g_action_map_lookup_action (G_ACTION_MAP (app), "theme");
  g_signal_connect_object (style_manager, "notify::dark", G_CALLBACK (on_system_dark_changed), action, G_CONNECT_DEFAULT);
  on_system_dark_changed (style_manager, NULL, G_SIMPLE_ACTION (action));
  g_signal_connect (app, "window-added", G_CALLBACK (on_window_added), NULL);

  if (theme != NULL)
    g_action_change_state (action, g_variant_new_string (theme));
}

static void
on_activate (GtkApplication *app)
{
  g_autoptr (GtkBuilder) builder = NULL;
  g_autoptr (GPropertyAction) view_action = NULL;
  GtkWindow *window;

  window = gtk_application_get_active_window (app);
  if (window == NULL)
    {
      g_type_ensure (REVOLUTION_TYPE_MAIL_VIEW);
      g_type_ensure (REVOLUTION_TYPE_CONTACTS_VIEW);
      g_type_ensure (REVOLUTION_TYPE_CALENDAR_VIEW);
      builder = gtk_builder_new_from_resource ("/org/revolution/Mail/window.ui");
      window = GTK_WINDOW (gtk_builder_get_object (builder, "window"));
      /* win.view für Strg+1/2/3 (window.blp): wechselt die AdwViewStack-Seite. */
      view_action = g_property_action_new ("view", gtk_builder_get_object (builder, "app_stack"),
                                           "visible-child-name");
      g_action_map_add_action (G_ACTION_MAP (window), G_ACTION (view_action));
      gtk_window_set_application (window, app);
    }

  gtk_window_present (window);
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (AdwApplication) app = NULL;

  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  g_set_application_name (_("Revolution"));

  app = adw_application_new (APPLICATION_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);

  return g_application_run (G_APPLICATION (app), argc, argv);
}
