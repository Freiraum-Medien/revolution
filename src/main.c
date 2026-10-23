/* main.c
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <errno.h>
#include <glib/gi18n.h>
#include <adwaita.h>
#include <camel/camel.h>
#include <libedataserver/libedataserver.h>

#include "contacts/contacts-view.h"
#include "calendar/calendar-view.h"
#include "mail/mail-session.h"
#include "mail/mail-view.h"

/* Drei Themes (Arnd 22.10.2026, #340): stateful Aktion app.theme (Typ „s“) für den
 * Abschnitt „Darstellung“ im Hauptmenü (mail-, calendar-, contacts-view.blp).
 *   hell         force-light
 *   dunkel-hell  force-dark, Mail-Inhalt und Schreibfläche bleiben hell
 *   dunkel       force-dark und Klasse .content-dark an allen Fenstern (style.css)
 * Die Wahl wird im GSettings-Schlüssel „theme“ gespeichert (#342) und beim Start
 * gesetzt; „system“ (Vorgabe) folgt dem System (color-scheme default). Bis zur ersten
 * Wahl zeigt die Radio-Marke das Aussehen des Systems (hell bzw. dunkel-hell).
 * Zum Testen überschreibt die Umgebungsvariable REVOLUTION_THEME den Startwert; die
 * Wahl wird dann nicht gespeichert. */
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
  GSettings *settings = G_SETTINGS (user_data);
  GtkApplication *app = GTK_APPLICATION (g_application_get_default ());
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
  if (g_getenv ("REVOLUTION_THEME") == NULL)
    g_settings_set_string (settings, "theme", theme);

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
on_startup (GtkApplication *app,
            GSettings      *settings)
{
  const GActionEntry app_actions[] = {
    { "theme", NULL, "s", "'hell'", change_theme, { 0 } },
  };
  AdwStyleManager *style_manager = adw_style_manager_get_default ();
  g_autofree char *saved = g_settings_get_string (settings, "theme");
  const char *theme = g_getenv ("REVOLUTION_THEME");
  GAction *action;

  g_action_map_add_action_entries (G_ACTION_MAP (app), app_actions, G_N_ELEMENTS (app_actions), settings);
  action = g_action_map_lookup_action (G_ACTION_MAP (app), "theme");
  g_signal_connect_object (style_manager, "notify::dark", G_CALLBACK (on_system_dark_changed), action, G_CONNECT_DEFAULT);
  on_system_dark_changed (style_manager, NULL, G_SIMPLE_ACTION (action));
  g_signal_connect (app, "window-added", G_CALLBACK (on_window_added), NULL);

  if (theme == NULL && g_strcmp0 (saved, "system") != 0)
    theme = saved;
  if (theme != NULL)
    g_action_change_state (action, g_variant_new_string (theme));
}

/* #343: eigene Verzeichnisse getrennt von Evolution (CLAUDE.md Architektur-Festlegung 4),
 * angelegt wie e_get_user_data_dir/e_get_user_cache_dir (EDS e-data-server-util.c:70-135,
 * g_mkdir_with_parents 0700) und Stamp (stamp-helper.c:46-58). Beide Profile nutzen
 * dieselben Ordner wie bei Stamp, das den Ordner „stamp“ fest verwendet. */
static void
create_user_dirs (RevolutionMailView *mail_view)
{
  const char *bases[] = { g_get_user_data_dir (), g_get_user_cache_dir () };

  for (gsize i = 0; i < G_N_ELEMENTS (bases); i++)
    {
      g_autofree char *dir = g_build_filename (bases[i], "revolution", NULL);
      g_autofree char *message = NULL;

      if (g_mkdir_with_parents (dir, 0700) == 0)
        {
          g_debug ("Verzeichnis bereit: %s", dir);
          continue;
        }

      message = g_strdup_printf (_("Ordner „%s“ konnte nicht angelegt werden: %s"), dir, g_strerror (errno));
      g_warning ("%s", message);
      revolution_mail_view_show_error (mail_view, _("Datenordner fehlt"), message, FALSE);
    }
}

/* #343 Worker-Muster (README „Architektur“): EDS-Aufruf im Worker-Thread, Ergebnis im
 * Rückruf im UI-Thread. Vorlage Evolution libemail-engine e-mail-session.c:2538-2618
 * (mail_session_get_inbox_thread, e_mail_session_get_inbox). task_data ist der UI-Thread,
 * nur für den Vergleich im Log. */
static void
load_registry_thread (GTask        *task,
                      gpointer      source_object G_GNUC_UNUSED,
                      gpointer      task_data,
                      GCancellable *cancellable)
{
  GError *error = NULL;
  ESourceRegistry *registry;

  g_debug ("ESourceRegistry: lädt im Thread %p, UI-Thread %p, Worker: %s",
           (gpointer) g_thread_self (), task_data, g_thread_self () != task_data ? "ja" : "nein");
  registry = e_source_registry_new_sync (cancellable, &error);

  if (registry == NULL)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, registry, g_object_unref);
}

/* #346: CamelSession im Worker (README „Worker“); revolution_mail_session_new registriert
 * die Stores der Konten. Danach online wie Evolution nach dem Start (e-mail-backend.c:323)
 * und Stamp (stamp-session.c:326); es baut noch keine Verbindung auf.
 * #347: Vorher camel_init wie Evolution (e-mail-backend.c:1301), aber im Worker wie Stamp
 * (stamp-session.c:334-356), denn es lädt die Zertifikatsdatenbank camel-cert.db
 * (EDS camel.c:224-233) aus ~/.local/share/revolution. Ohne NSS: TLS läuft über GIO
 * (camel-network-service.c:978); NSS braucht erst S/MIME. */
static void
load_session_thread (GTask        *task,
                     gpointer      source_object G_GNUC_UNUSED,
                     gpointer      task_data,
                     GCancellable *cancellable G_GNUC_UNUSED)
{
  g_autofree char *camel_dir = g_build_filename (g_get_user_data_dir (), "revolution", NULL);
  RevolutionMailSession *session;

  camel_init (camel_dir, FALSE);
  session = revolution_mail_session_new (task_data);

  camel_session_set_online (CAMEL_SESSION (session), TRUE);
  g_task_return_pointer (task, session, g_object_unref);
}

/* #348: Ergebnis des Worker-Tasks je Store (note_store_thread). */
typedef struct
{
  GError *connect_error;
  GError *folders_error;
  CamelFolderInfo *info;
} NoteStoreResult;

static void
note_store_result_free (NoteStoreResult *result)
{
  g_clear_error (&result->connect_error);
  g_clear_error (&result->folders_error);
  g_clear_pointer (&result->info, camel_folder_info_free);
  g_free (result);
}

/* #347: Store online schalten im Worker (README „Worker“) wie Evolution nach dem Start
 * (e-mail-backend.c:323-339, e_mail_store_go_online_sync e-mail-store-utils.c:214-240).
 * Die Stores entstehen vor camel_session_set_online und damit offline (EDS
 * camel-offline-store.c:142); imapx verbindet offline nicht. camel_offline_store_set_online_sync
 * verbindet (camel-offline-store.c:292-301), dabei meldet sich der Store an
 * (camel_session_authenticate_sync, imapx camel-imapx-server.c:3491, microsoft365
 * camel-m365-store.c:532). Nur Offline-Stores (imapx, microsoft365); andere verbindet
 * Evolution beim Start nicht (e_mail_store_go_online_sync, e-mail-store-utils.c:233-236).
 * #348: Danach die Ordnerliste, nach Evolution mail_folder_cache_note_store_thread
 * (libemail-engine/mail-folder-cache.c:2078-2197), gekürzt:
 *   - Fehler beim Verbinden beenden den Task nicht; offline liefert der Store die Ordner
 *     aus dem Cache (dort 2113-2116).
 *   - Ohne Ordnerbaum (POP, Nur-Versand) keine Ordner (dort 2125-2127,
 *     store_has_folder_hierarchy 1478-1494); die Seitenleiste zeigt das Konto dann nicht.
 *   - Ohne die Ersteinrichtung (dort 2118-2122): sie schreibt Ordner-Einstellungen in die
 *     ESource und damit in Evolutions Konfiguration (mail-folder-cache.c:1996-2015).
 *   - Ohne CAMEL_STORE_FOLDER_INFO_FAST (dort 2140-2142): imapx liefert damit nur seinen
 *     Cache und lädt die Liste in einem Session-Job nach, dessen Ergebnis nur als Signal
 *     „folder-created“ kommt (EDS camel-imapx-store.c:2165-2170, camel-store.c:841-845);
 *     microsoft365 fragt mit FAST den Server nur ohne gespeicherten Delta-Link
 *     (camel-m365-store.c:1190-1201). Der Cache von Revolution (~/.cache/revolution/mail)
 *     ist beim ersten Start leer; ohne FAST fragen beide den Server (LIST bzw. Delta). */
static void
note_store_thread (GTask        *task,
                   gpointer      source_object,
                   gpointer      task_data G_GNUC_UNUSED,
                   GCancellable *cancellable)
{
  CamelStore *store = CAMEL_STORE (source_object);
  CamelProvider *provider = camel_service_get_provider (CAMEL_SERVICE (store));
  NoteStoreResult *result = g_new0 (NoteStoreResult, 1);

  if (CAMEL_IS_OFFLINE_STORE (store) &&
      !camel_offline_store_set_online_sync (CAMEL_OFFLINE_STORE (store), TRUE, cancellable, &result->connect_error) &&
      result->connect_error == NULL)
    result->connect_error = g_error_new_literal (CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE,
                                                 _("Verbindung ohne Fehlermeldung abgebrochen"));

  if (provider != NULL && (provider->flags & (CAMEL_PROVIDER_IS_STORAGE | CAMEL_PROVIDER_IS_EXTERNAL)) != 0)
    result->info = camel_store_get_folder_info_sync (store, NULL,
                                                     CAMEL_STORE_FOLDER_INFO_RECURSIVE |
                                                     CAMEL_STORE_FOLDER_INFO_SUBSCRIBED,
                                                     cancellable, &result->folders_error);

  g_task_return_pointer (task, result, (GDestroyNotify) note_store_result_free);
}

/* Fehler beim Verbinden wie #347; Fehler beim Laden der Ordner nur, wenn die Verbindung
 * stand (sonst wäre es derselbe Fehler zweimal). */
static void
on_store_noted (GObject      *source_object,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr (RevolutionMailView) mail_view = user_data;
  CamelService *service = CAMEL_SERVICE (source_object);
  g_autofree char *display_name = camel_service_dup_display_name (service);
  g_autofree char *detail = NULL;
  NoteStoreResult *noted;
  GError *error;
  const char *title;

  /* NULL nur nach Abbruch beim Beenden (g_task_propagate_pointer prüft den GCancellable). */
  noted = g_task_propagate_pointer (G_TASK (result), NULL);
  if (noted == NULL)
    return;

  if (noted->connect_error != NULL)
    {
      error = noted->connect_error;
      g_warning ("Anmeldung %s (%s): %s", display_name, camel_service_get_uid (service), error->message);

      if (g_error_matches (error, G_TLS_ERROR, G_TLS_ERROR_BAD_CERTIFICATE))
        title = _("Zertifikat nicht vertrauenswürdig");
      else if (g_error_matches (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_CANT_AUTHENTICATE))
        title = _("Anmeldung fehlgeschlagen");
      else
        title = _("Keine Verbindung");

      detail = g_strdup_printf ("%s: %s", display_name, error->message);
      revolution_mail_view_show_error (mail_view, title, detail, TRUE);
    }
  else if (CAMEL_IS_OFFLINE_STORE (service))
    g_debug ("verbunden: %s", camel_service_get_uid (service));

  if (noted->folders_error == NULL)
    revolution_mail_view_set_folders (mail_view, camel_service_get_uid (service), noted->info);
  else if (noted->connect_error == NULL)
    {
      g_warning ("Ordner %s (%s): %s", display_name, camel_service_get_uid (service), noted->folders_error->message);
      detail = g_strdup_printf ("%s: %s", display_name, noted->folders_error->message);
      revolution_mail_view_show_error (mail_view, _("Ordner nicht geladen"), detail, TRUE);
    }
  else
    g_debug ("Ordner: %s nicht geladen: %s", camel_service_get_uid (service), noted->folders_error->message);

  note_store_result_free (noted);
}

/* #348: Neue Stores anmelden und ihre Ordner laden, beim Start (on_session_loaded) und nach
 * jedem Abgleich der Session („services-changed“, Konto zur Laufzeit hinzugefügt oder
 * aktiviert); bereits gemeldete Stores sind markiert. Dazu Fehler aus add_service ins Banner. */
static void
note_stores (RevolutionMailSession *session,
             RevolutionMailView    *mail_view)
{
  g_auto (GStrv) errors = revolution_mail_session_take_errors (session);
  g_autolist (CamelService) services = camel_session_list_services (CAMEL_SESSION (session));
  GCancellable *cancellable = g_object_get_data (G_OBJECT (session), "cancellable");

  for (guint i = 0; errors[i] != NULL; i++)
    revolution_mail_view_show_error (mail_view, _("Konto nicht eingerichtet"), errors[i], TRUE);

  for (GList *l = services; l != NULL; l = l->next)
    {
      g_autoptr (GTask) task = NULL;

      if (!CAMEL_IS_STORE (l->data) || g_object_get_data (l->data, "revolution-noted") != NULL)
        continue;

      g_object_set_data (l->data, "revolution-noted", GINT_TO_POINTER (TRUE));
      task = g_task_new (l->data, cancellable, on_store_noted, g_object_ref (mail_view));
      g_task_set_source_tag (task, note_stores);
      g_task_run_in_thread (task, note_store_thread);
    }
}

static void
on_session_loaded (GObject      *source_object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  g_autoptr (RevolutionMailView) mail_view = user_data;
  RevolutionMailSession *session = g_task_propagate_pointer (G_TASK (result), NULL);
  g_autoptr (GCancellable) cancellable = NULL;
  g_autolist (CamelService) services = camel_session_list_services (CAMEL_SESSION (session));

  g_debug ("CamelSession: bereit, Rückruf im Thread %p, %u Stores",
           (gpointer) g_thread_self (), g_list_length (services));
  /* An der Anwendung gehalten wie die Registry (g_object_get_data (app, "session")). */
  g_object_set_data_full (source_object, "session", session, g_object_unref);

  /* Beim Beenden abbrechen wie das Laden der Registry (on_activate). */
  cancellable = g_cancellable_new ();
  g_signal_connect_object (source_object, "shutdown", G_CALLBACK (g_cancellable_cancel), cancellable, G_CONNECT_SWAPPED);
  g_object_set_data_full (G_OBJECT (session), "cancellable", g_object_ref (cancellable), g_object_unref);

  revolution_mail_view_set_session (mail_view, CAMEL_SESSION (session));
  g_signal_connect_object (session, "services-changed", G_CALLBACK (note_stores), mail_view, G_CONNECT_DEFAULT);
  note_stores (session, mail_view);
}

static void
on_registry_loaded (GObject      *source_object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr (RevolutionMailView) mail_view = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *message = NULL;
  g_autoptr (GTask) task = NULL;
  ESourceRegistry *registry;

  registry = g_task_propagate_pointer (G_TASK (result), &error);
  if (registry != NULL)
    {
      g_autolist (ESource) accounts = e_source_registry_list_sources (registry, E_SOURCE_EXTENSION_MAIL_ACCOUNT);

      g_debug ("ESourceRegistry: geladen, Rückruf im Thread %p, %u Mail-Konten",
               (gpointer) g_thread_self (), g_list_length (accounts));
      /* An der Anwendung gehalten (g_object_get_data (app, "registry")); die Mail-Ansicht
       * zeigt daraus die Konten (#345). */
      g_object_set_data_full (source_object, "registry", registry, g_object_unref);
      revolution_mail_view_set_registry (mail_view, registry);

      task = g_task_new (source_object, NULL, on_session_loaded, g_object_ref (mail_view));
      g_task_set_source_tag (task, on_registry_loaded);
      g_task_set_task_data (task, g_object_ref (registry), g_object_unref);
      g_task_run_in_thread (task, load_session_thread);
      return;
    }

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  message = g_strdup_printf (_("Konten konnten nicht geladen werden: %s"), error->message);
  g_warning ("%s", message);
  revolution_mail_view_show_error (mail_view, _("Konten nicht verfügbar"), message, FALSE);
}

static void
on_activate (GtkApplication *app,
             GSettings      *settings)
{
  g_autoptr (GtkBuilder) builder = NULL;
  g_autoptr (GPropertyAction) view_action = NULL;
  g_autoptr (GCancellable) cancellable = NULL;
  g_autoptr (GTask) task = NULL;
  RevolutionMailView *mail_view;
  GtkWindow *window;

  window = gtk_application_get_active_window (app);
  if (window == NULL)
    {
      g_type_ensure (REVOLUTION_TYPE_MAIL_VIEW);
      g_type_ensure (REVOLUTION_TYPE_CONTACTS_VIEW);
      g_type_ensure (REVOLUTION_TYPE_CALENDAR_VIEW);
      builder = gtk_builder_new_from_resource ("/de/freiraum_medien/Revolution/window.ui");
      window = GTK_WINDOW (gtk_builder_get_object (builder, "window"));
      /* win.view für Strg+1/2/3 (window.blp): wechselt die AdwViewStack-Seite. */
      view_action = g_property_action_new ("view", gtk_builder_get_object (builder, "app_stack"),
                                           "visible-child-name");
      g_action_map_add_action (G_ACTION_MAP (window), G_ACTION (view_action));
      gtk_window_set_application (window, app);
      /* Fenstergröße und maximiert wie Stamp (stamp-window.c:216-218). */
      g_settings_bind (settings, "window-width", window, "default-width", G_SETTINGS_BIND_DEFAULT);
      g_settings_bind (settings, "window-height", window, "default-height", G_SETTINGS_BIND_DEFAULT);
      g_settings_bind (settings, "window-maximized", window, "maximized", G_SETTINGS_BIND_DEFAULT);
#ifdef DEVEL
      /* Entwicklungsvariante gestreift wie Stamp (stamp-window.c:221-223). */
      gtk_widget_add_css_class (GTK_WIDGET (window), "devel");
#endif

      mail_view = REVOLUTION_MAIL_VIEW (gtk_builder_get_object (builder, "mail_view"));
      create_user_dirs (mail_view);

      /* Beim Beenden abbrechen; der Task hält den GCancellable bis zum Rückruf. */
      cancellable = g_cancellable_new ();
      g_signal_connect_object (app, "shutdown", G_CALLBACK (g_cancellable_cancel), cancellable, G_CONNECT_SWAPPED);
      task = g_task_new (app, cancellable, on_registry_loaded, g_object_ref (mail_view));
      g_task_set_source_tag (task, on_activate);
      g_task_set_task_data (task, g_thread_self (), NULL);
      g_task_run_in_thread (task, load_registry_thread);
    }

  gtk_window_present (window);
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (AdwApplication) app = NULL;
  g_autoptr (GSettings) settings = NULL;

  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  g_set_application_name (_("Revolution"));

  /* Ein Schema für beide Profile wie GNOME Calendar (org.gnome.calendar). */
  settings = g_settings_new ("de.freiraum_medien.Revolution");
  app = adw_application_new (APPLICATION_ID, G_APPLICATION_DEFAULT_FLAGS);
  /* Fester Pfad für beide Profile (style.css, Icons), wie GNOME Calendar
   * (gcal-application.c:614) und Stamp (stamp-application.c:57). */
  g_application_set_resource_base_path (G_APPLICATION (app), "/de/freiraum_medien/Revolution");
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), settings);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), settings);

  return g_application_run (G_APPLICATION (app), argc, argv);
}
