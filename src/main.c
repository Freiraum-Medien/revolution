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
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);

  return g_application_run (G_APPLICATION (app), argc, argv);
}
