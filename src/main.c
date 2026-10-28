/* main.c
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gi18n.h>
#include <adwaita.h>
#include <camel/camel.h>
#include <libedataserver/libedataserver.h>

#include "contacts/contacts-view.h"
#include "calendar/calendar-view.h"
#include "eds-compat.h"
#include "mail/mail-favorites.h"
#include "composer/composer.h"
#include "mail/mail-folder.h"
#include "mail/mail-notification.h"
#include "mail/mail-outbox.h"
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

static void on_show_message (GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_quit (GSimpleAction *action, GVariant *parameter, gpointer user_data);

static void
on_startup (GtkApplication *app,
            GSettings      *settings)
{
  const GActionEntry app_actions[] = {
    { "theme", NULL, "s", "'hell'", change_theme, { 0 } },
    /* #366: Klick auf die Benachrichtigung (mail-notification.c), Beenden. */
    { "show-message", on_show_message, "(ss)", NULL, NULL, { 0 } },
    { "quit", on_quit, NULL, NULL, NULL, { 0 } },
  };
  AdwStyleManager *style_manager = adw_style_manager_get_default ();
  g_autofree char *saved = g_settings_get_string (settings, "theme");
  const char *theme = g_getenv ("REVOLUTION_THEME");
  g_autoptr (GAction) background = g_settings_create_action (settings, "run-in-background");
  GAction *action;

  g_action_map_add_action_entries (G_ACTION_MAP (app), app_actions, G_N_ELEMENTS (app_actions), settings);
  /* #366: Hauptmenü „Im Hintergrund weiterlaufen“ (mail-view.blp), Beenden mit Strg+Q wie Stamp
   * (stamp-application.c:493). */
  g_action_map_add_action (G_ACTION_MAP (app), background);
  gtk_application_set_accels_for_action (app, "app.quit", (const char *[]) { "<primary>q", NULL });
  action = g_action_map_lookup_action (G_ACTION_MAP (app), "theme");
  g_signal_connect_object (style_manager, "notify::dark", G_CALLBACK (on_system_dark_changed), action, G_CONNECT_DEFAULT);
  on_system_dark_changed (style_manager, NULL, G_SIMPLE_ACTION (action));
  g_signal_connect (app, "window-added", G_CALLBACK (on_window_added), NULL);
  /* #439: Fenstersymbol je Profil (.Devel-Symbol der Entwicklungsvariante) wie GNOME Calendar
   * (gcal-application.c:369). */
  gtk_window_set_default_icon_name (APPLICATION_ID);

  if (theme == NULL && g_strcmp0 (saved, "system") != 0)
    theme = saved;
  if (theme != NULL)
    g_action_change_state (action, g_variant_new_string (theme));
}

/* #343: eigene Verzeichnisse getrennt von Evolution (CLAUDE.md Architektur-Festlegung 4),
 * angelegt wie e_get_user_data_dir/e_get_user_cache_dir (EDS e-data-server-util.c:70-135,
 * g_mkdir_with_parents 0700) und Stamp (stamp-helper.c:46-58). #439: Name je Profil
 * (USER_DIR_NAME, meson.build): „revolution“ bzw. „revolution-devel“ für die Entwicklungsvariante. */
static void
create_user_dirs (RevolutionMailView *mail_view)
{
  const char *bases[] = { g_get_user_data_dir (), g_get_user_cache_dir () };

  for (gsize i = 0; i < G_N_ELEMENTS (bases); i++)
    {
      g_autofree char *dir = g_build_filename (bases[i], USER_DIR_NAME, NULL);
      g_autofree char *message = NULL;

      if (g_mkdir_with_parents (dir, 0700) == 0)
        {
          g_debug ("Verzeichnis bereit: %s", dir);
          continue;
        }

      message = g_strdup_printf (_("Could not create folder “%s”: %s"), dir, g_strerror (errno));
      g_warning ("%s", message);
      revolution_mail_view_show_error (mail_view, _("Data Folder Missing"), message, FALSE);
    }
}

/* #343 Worker-Muster (docs/ENTWICKLUNG.md „Architektur“): EDS-Aufruf im Worker-Thread, Ergebnis im
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

/* #346: CamelSession im Worker (docs/ENTWICKLUNG.md „Worker“); revolution_mail_session_new registriert
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
  g_autofree char *camel_dir = g_build_filename (g_get_user_data_dir (), USER_DIR_NAME, NULL);
  RevolutionMailSession *session;
  g_autoptr (GError) error = NULL;

  camel_init (camel_dir, FALSE);
  session = revolution_mail_session_new (task_data);
  /* #351: Suchordner der Favoriten, bevor die Konten ihre Quellen melden (on_store_noted). */
  if (!revolution_mail_favorites_setup (CAMEL_SESSION (session), &error))
    g_warning ("Favoriten: %s", error->message);
  g_clear_error (&error);
  /* #362: Postausgang im lokalen Store (mail-outbox.c), wie Evolution aus constructed der Session. */
  if (!revolution_mail_outbox_setup (CAMEL_SESSION (session), &error))
    g_warning ("Postausgang: %s", error->message);

  /* #408: online nur mit Netz wie Evolution beim Start: EShell übernimmt „network-available“
   * des ENetworkMonitor beim Anlegen (e-shell.c:1845-1850, G_BINDING_SYNC_CREATE) und geht ohne
   * Netz sofort offline (e_shell_set_network_available, e-shell.c:3206-3215). Denselben Monitor
   * nutzt die Session (mail-session.c, camel_session_set_network_monitor). */
  camel_session_set_online (CAMEL_SESSION (session),
                            g_network_monitor_get_network_available (G_NETWORK_MONITOR (e_network_monitor_get_default ())));
  g_task_return_pointer (task, session, g_object_unref);
}

/* #421: Fehler beim Verbinden, die heißen „Server nicht erreichbar“: Namensauflösung (G_RESOLVER_ERROR,
 * gresolver.h:293; microsoft365 über libsoup), Netz/Host nicht erreichbar, abgewiesen, Zeitüberschreitung
 * (G_IO_ERROR_*, gioenums.h:566, 579-581) und CAMEL_SERVICE_ERROR_UNAVAILABLE, das imapx statt eines
 * Resolver-Fehlers meldet („You must be working online …“, camel-imapx-conn-manager.c:973-987) und
 * das EDS selbst wie „offline“ behandelt (camel-store.c:302-306). */
static gboolean
server_unreachable (const GError *error)
{
  return error->domain == G_RESOLVER_ERROR ||
         g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE) ||
         g_error_matches (error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE) ||
         g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED) ||
         g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
         g_error_matches (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE);
}

/* #348: Ergebnis des Worker-Tasks je Store (note_store_thread). */
typedef struct
{
  GError *connect_error;
  GError *folders_error;
  CamelFolderInfo *info;
  /* #408: Session offline: Store offline geschaltet (Fehler dabei nur im Log wie Evolution),
   * Ordner aus dem Cache ohne Verbindungsversuch. */
  gboolean offline;
  GError *offline_error;
  /* #421: Verbindung gescheitert bei Session online, Store offline geschaltet, Ordner aus dem Cache. */
  gboolean fallback;
} NoteStoreResult;

static void
note_store_result_free (NoteStoreResult *result)
{
  g_clear_error (&result->connect_error);
  g_clear_error (&result->folders_error);
  g_clear_error (&result->offline_error);
  g_clear_pointer (&result->info, camel_folder_info_free);
  g_free (result);
}

/* #347: Store online schalten im Worker (docs/ENTWICKLUNG.md „Worker“) wie Evolution nach dem Start
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
 *     ist beim ersten Start leer; ohne FAST fragen beide den Server (LIST bzw. Delta).
 * #350: Danach Gesendet, Entwürfe und Archiv als Ordnertyp (revolution_mail_folder_mark_special).
 * #408: Ist die Session offline (kein Netz), verbindet der Task nicht, sondern schaltet den Store
 * offline wie Evolution (e_mail_store_go_offline_sync, e-mail-store-utils.c:147-180); beim Start
 * ist er das schon (angelegt vor camel_session_set_online, camel-offline-store.c:142). Offline
 * liefern die Stores die Ordner aus dem Cache (camel_offline_store_get_online, dort 223-238;
 * microsoft365 camel-m365-store.c:1187, imapx camel-imapx-store.c:2143-2146);
 * camel_store_get_folder_info_sync verbindet bei Session offline nicht neu (camel-store.c:278).
 * Derselbe Task läuft bei jedem Netzwechsel (on_network_changed, go_online). Je Store nacheinander
 * (Sperre „revolution-line-lock“): ein abgebrochener Online-Auftrag endet, bevor der Offline-Auftrag
 * desselben Stores beginnt, statt gleichzeitig zu verbinden und zu trennen. */
static void
note_store_thread (GTask        *task,
                   gpointer      source_object,
                   gpointer      task_data G_GNUC_UNUSED,
                   GCancellable *line_cancellable)
{
  /* #408: Camel bekommt ein eigenes Cancellable, das an dem der Umschaltung hängt
   * (camel_operation_new_proxy): bricht Camel/EDS es von innen ab (microsoft365 beim Trennen), trifft
   * das nur diesen Auftrag, nicht die übrigen Stores derselben Umschaltung. */
  g_autoptr (GCancellable) cancellable = camel_operation_new_proxy (line_cancellable);
  CamelStore *store = CAMEL_STORE (source_object);
  CamelProvider *provider = camel_service_get_provider (CAMEL_SERVICE (store));
  g_autoptr (CamelSession) session = camel_service_ref_session (CAMEL_SERVICE (store));
  g_autoptr (GMutexLocker) locker = g_mutex_locker_new (g_object_get_data (G_OBJECT (store), "revolution-line-lock"));
  NoteStoreResult *result = g_new0 (NoteStoreResult, 1);

  /* Ein neuerer Netzwechsel hat diesen Auftrag schon abgelöst. */
  if (g_task_return_error_if_cancelled (task))
    {
      note_store_result_free (result);
      return;
    }

  result->offline = session == NULL || !camel_session_get_online (session);
  if (CAMEL_IS_OFFLINE_STORE (store) && !result->offline &&
      !camel_offline_store_set_online_sync (CAMEL_OFFLINE_STORE (store), TRUE, cancellable, &result->connect_error) &&
      result->connect_error == NULL)
    result->connect_error = g_error_new_literal (CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE,
                                                 _("Connection closed without an error message"));
  else if (CAMEL_IS_OFFLINE_STORE (store) && result->offline)
    camel_offline_store_set_online_sync (CAMEL_OFFLINE_STORE (store), FALSE, cancellable, &result->offline_error);
  else if (!CAMEL_IS_OFFLINE_STORE (store) && result->offline &&
           camel_service_get_connection_status (CAMEL_SERVICE (store)) != CAMEL_SERVICE_DISCONNECTED)
    camel_service_disconnect_sync (CAMEL_SERVICE (store), TRUE, cancellable, &result->offline_error);

  /* #421: Verbindung gescheitert, obwohl der Netzmonitor „online“ meldet (DNS-Ausfall, Captive
   * Portal). Der Store bleibt dann online, aber getrennt (camel-offline-store.c:293-301), und
   * camel_store_get_folder_info_sync verbindet neu (store_maybe_connect_sync, camel-store.c:262-313,
   * 1713); dessen Fehler gibt es außer G_IO_ERROR und CAMEL_SERVICE_ERROR_UNAVAILABLE weiter (dort
   * 302-309), microsoft365 scheitert mit G_RESOLVER_ERROR: keine Ordner. Evolution zeigt das Konto
   * dann ohne Ordner (mail-folder-cache.c:2138-2149, em-folder-tree.c:233-241). Hier wird der Store
   * offline geschaltet wie ohne Netz (#408); offline liefert er die Ordner aus dem Cache
   * (camel-store.c:288-293, camel-m365-store.c:1187, 1302-1306). camel_offline_store_set_online_sync
   * verbindet dabei einen getrennten Store noch einmal (camel-offline-store.c:288-301); dafür
   * entfällt der Versuch in camel_store_get_folder_info_sync. Wieder verbinden beim nächsten
   * Netzwechsel oder mit F5 (retry_stores).
   * Nur wenn der Server nicht erreichbar ist (server_unreachable); jeder andere Fehler (Anmeldung,
   * Zertifikat, abgebrochene Verbindung) lässt den Store wie vor #421 online: EDS verbindet ihn
   * beim nächsten Zugriff neu (store_maybe_connect_sync, camel-m365-store.c:1754-1762). */
  if (result->connect_error != NULL && CAMEL_IS_OFFLINE_STORE (store) && server_unreachable (result->connect_error))
    {
      result->fallback = TRUE;
      camel_offline_store_set_online_sync (CAMEL_OFFLINE_STORE (store), FALSE, cancellable, &result->offline_error);
    }

  if (provider != NULL && (provider->flags & (CAMEL_PROVIDER_IS_STORAGE | CAMEL_PROVIDER_IS_EXTERNAL)) != 0)
    result->info = camel_store_get_folder_info_sync (store, NULL,
                                                     CAMEL_STORE_FOLDER_INFO_RECURSIVE |
                                                     CAMEL_STORE_FOLDER_INFO_SUBSCRIBED,
                                                     cancellable, &result->folders_error);

  if (result->info != NULL && session != NULL)
    {
      g_autoptr (ESourceRegistry) registry = NULL;

      g_object_get (session, "registry", &registry, NULL);
      revolution_mail_folder_mark_special (store, registry, result->info, cancellable);
      if (provider != NULL && g_strcmp0 (provider->protocol, "imapx") == 0)
        revolution_mail_folder_forget_unread (result->info);
    }

  g_task_return_pointer (task, result, (GDestroyNotify) note_store_result_free);
}

/* #350: Posteingang beim Start aktualisieren wie Evolutions Abruf nach dem Start (mail-send-recv.c,
 * refresh_folders_exec: camel_store_get_folder_sync, camel_folder_refresh_info_sync) und im
 * UI-Thread beobachten (Ungelesen-Zähler, revolution_mail_folder_watch). Ordner immer über
 * camel_store_get_folder_sync (docs/ENTWICKLUNG.md „Camel-Ordner“). Ein Fehler beim Aktualisieren (z. B. ohne
 * Netz) lässt den Stand des Caches stehen. */
static void
refresh_inbox_thread (GTask        *task,
                      gpointer      source_object,
                      gpointer      task_data,
                      GCancellable *line_cancellable)
{
  /* #408: eigenes Cancellable wie note_store_thread. */
  g_autoptr (GCancellable) cancellable = camel_operation_new_proxy (line_cancellable);
  g_autoptr (GError) error = NULL;
  gint64 started = g_get_monotonic_time ();
  CamelFolder *folder = camel_store_get_folder_sync (source_object, task_data, 0, cancellable, &error);

  if (folder == NULL)
    {
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  if (revolution_eds_folder_refresh_info_sync (folder, cancellable, &error))
    g_debug ("Posteingang: %s %s aktualisiert nach %.0f ms", camel_service_get_uid (source_object),
             (const char *) task_data, (g_get_monotonic_time () - started) / 1000.0);
  else
    g_debug ("Posteingang: %s %s nicht aktualisiert: %s", camel_service_get_uid (source_object),
             (const char *) task_data, error->message);

  g_task_return_pointer (task, folder, g_object_unref);
}

static void
on_inbox_refreshed (GObject      *source_object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  g_autoptr (RevolutionMailView) mail_view = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (CamelFolder) folder = g_task_propagate_pointer (G_TASK (result), &error);

  if (folder != NULL)
    {
      revolution_mail_view_watch_folder (mail_view, folder, TRUE, TRUE);
      /* #366: neue Nachrichten ab jetzt melden (mail-notification.c). */
      revolution_mail_notification_watch (folder);
    }
  else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_debug ("Posteingang: %s nicht geladen: %s", camel_service_get_uid (CAMEL_SERVICE (source_object)),
             error->message);
}

static const char *
find_inbox (CamelFolderInfo *fi)
{
  for (; fi != NULL; fi = fi->next)
    {
      const char *found = (fi->flags & CAMEL_FOLDER_TYPE_MASK) == CAMEL_FOLDER_TYPE_INBOX ? fi->full_name
                                                                                       : find_inbox (fi->child);

      if (found != NULL)
        return found;
    }

  return NULL;
}

/* Posteingang inbox des Stores service abgleichen (refresh_inbox_thread), danach beobachten. */
static void
refresh_inbox (CamelService       *service,
               GCancellable       *cancellable,
               RevolutionMailView *mail_view,
               const char         *inbox)
{
  g_autoptr (GTask) task = g_task_new (service, cancellable, on_inbox_refreshed, g_object_ref (mail_view));

  g_task_set_source_tag (task, refresh_inbox);
  g_task_set_task_data (task, g_strdup (inbox), g_free);
  g_task_run_in_thread (task, refresh_inbox_thread);
}

/* #408: Netzwechsel. online_id: Quelle des verzögerten Online-Schaltens (go_online). line: der
 * GCancellable der laufenden Umschaltung; jeder Wechsel bricht die vorige ab und beginnt mit einem
 * neuen wie Evolution (shell_cancel_ongoing_preparing_line_change, e-shell.c:369-390, 415-419;
 * je Umschaltung eine eigene EActivity mit eigenem GCancellable, e-mail-backend.c:256-268,
 * 300-308). Ein Netzmonitor und eine Session je Prozess. */
static guint online_id;
static GCancellable *line;
/* #421: Quelle des verzögerten erneuten Verbindens (retry_stores). */
static guint retry_id;

static gboolean go_online (gpointer user_data);
static gboolean retry_stores (gpointer user_data);
static void on_network_changed (GNetworkMonitor *monitor, GParamSpec *pspec, RevolutionMailView *mail_view);

static GCancellable *
new_line_change (void)
{
  if (line != NULL)
    g_cancellable_cancel (line);
  g_set_object (&line, NULL);
  line = g_cancellable_new ();

  return line;
}

/* #351: Favoriten eines Kontos einmal beim ersten Ordnerbaum; mit eigenem GCancellable, den nur
 * das Beenden abbricht (nicht der nächste Netzwechsel). */
static void
add_favorites (RevolutionMailView *mail_view,
               CamelStore         *store,
               CamelFolderInfo    *info)
{
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();

  g_signal_connect_object (g_application_get_default (), "shutdown", G_CALLBACK (g_cancellable_cancel), cancellable,
                           G_CONNECT_SWAPPED);
  revolution_mail_favorites_add_store (mail_view, store, info, cancellable);
}

/* Fehler beim Verbinden wie #347; Fehler beim Laden der Ordner nur, wenn die Verbindung
 * stand (sonst wäre es derselbe Fehler zweimal).
 * #408: Ein Auftrag, den ein neuerer Netzwechsel abgelöst hat, kommt abgebrochen an und geht nur
 * ins Log; der neuere Auftrag desselben Stores läuft danach (Sperre in note_store_thread). Jeder
 * andere Fehler kommt ins Log und ins Sammel-Banner, auch offline. Offline meldet sonst die Pille
 * „Offline“ (revolution_mail_view_set_offline). Nach einem Netzwechsel (Quelle go_online bzw.
 * on_network_changed) wird ein schon gezeigter Ordnerbaum abgeglichen statt ersetzt
 * (revolution_mail_view_update_folders): Ersetzen hebt die Auswahl auf (set_folders leert den
 * Store). Danach Posteingang und, online, die offene Liste des Kontos vom Server
 * (revolution_mail_view_refresh_account). */
static void
on_store_noted (GObject      *source_object,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr (RevolutionMailView) mail_view = user_data;
  CamelService *service = CAMEL_SERVICE (source_object);
  const char *uid = camel_service_get_uid (service);
  g_autoptr (CamelSession) session = camel_service_ref_session (service);
  g_autofree char *display_name = camel_service_dup_display_name (service);
  g_autofree char *detail = NULL;
  g_autoptr (GError) task_error = NULL;
  gboolean online = session != NULL && camel_session_get_online (session);
  gpointer tag = g_task_get_source_tag (G_TASK (result));
  gboolean again = tag == go_online || tag == on_network_changed || tag == retry_stores;
  gboolean merge = again && g_object_get_data (G_OBJECT (service), "revolution-folders") != NULL;
  NoteStoreResult *noted;
  GError *error;
  const char *title;
  gboolean fallback;

  noted = g_task_propagate_pointer (G_TASK (result), &task_error);
  if (noted == NULL)
    {
      if (g_error_matches (task_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_debug ("Netz: %s Umschaltung abgebrochen (neuerer Netzwechsel oder Beenden)", uid);
      else
        g_warning ("Konto %s (%s): %s", display_name, uid, task_error->message);
      return;
    }

  if (noted->connect_error != NULL)
    {
      error = noted->connect_error;
      g_warning ("Anmeldung %s (%s): %s", display_name, uid, error->message);

      if (g_error_matches (error, G_TLS_ERROR, G_TLS_ERROR_BAD_CERTIFICATE))
        title = _("Certificate Not Trusted");
      else if (g_error_matches (error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_CANT_AUTHENTICATE))
        title = _("Authentication Failed");
      else
        title = _("No Connection");

      /* #420: Die Ordnerliste danach verbindet einen online gebliebenen Store neu
       * (store_maybe_connect_sync, camel-store.c:262-315); ist er schon verbunden, ist der Fehler
       * überholt und kommt nur ins Log (Abnahme #358: info@ vor dieser Meldung wieder angemeldet). Sonst
       * nimmt ihn on_connection_status beim nächsten Verbinden wieder aus dem Banner. */
      if (camel_service_get_connection_status (service) == CAMEL_SERVICE_CONNECTED)
        g_debug ("Verbindung gescheitert: %s, inzwischen verbunden, kein Banner", uid);
      else
        {
          detail = g_strdup_printf ("%s: %s", display_name, error->message);
          revolution_mail_view_show_account_error (mail_view, uid, title, detail);
        }
      g_debug ("Verbindung gescheitert: %s, %s (%s %d)", uid,
               noted->fallback ? "Ordner aus dem Cache" : "Store bleibt online",
               g_quark_to_string (error->domain), error->code);
    }
  else if (noted->offline_error != NULL)
    /* Wie Evolution (mail_backend_store_operation_done_cb, e-mail-backend.c:128-138): offline
     * schaltet der Store trotzdem (camel-offline-store.c:356-367). */
    g_debug ("offline: %s, Ordner aus dem Cache, Trennen mit Fehler: %s", uid, noted->offline_error->message);
  else if (noted->offline)
    g_debug ("offline: %s, Ordner aus dem Cache", uid);
  else if (CAMEL_IS_OFFLINE_STORE (service))
    g_debug ("verbunden: %s", uid);

  if (noted->folders_error == NULL)
    {
      const char *inbox = find_inbox (noted->info);

      if (merge)
        revolution_mail_view_update_folders (mail_view, uid, noted->info);
      else
        {
          revolution_mail_view_set_folders (mail_view, uid, noted->info);
          /* #351: Posteingang, Entwürfe und Gesendet dieses Kontos in die Favoriten. */
          /* #408: nur mit Ordnerliste; ohne Netz und ohne Cache (info NULL) trägt der nächste
           * Netzwechsel die Favoriten nach (set_folders, solange der Baum leer ist). */
          if (noted->info != NULL && g_object_get_data (G_OBJECT (service), "revolution-favorites") == NULL)
            {
              add_favorites (mail_view, CAMEL_STORE (service), noted->info);
              g_object_set_data (G_OBJECT (service), "revolution-favorites", GINT_TO_POINTER (TRUE));
            }
        }
      if (noted->info != NULL)
        g_object_set_data (G_OBJECT (service), "revolution-folders", GINT_TO_POINTER (TRUE));
      if (inbox != NULL)
        {
          /* #366: für den Abruf alle n Minuten (on_refresh_timeout). */
          g_object_set_data_full (G_OBJECT (service), "revolution-inbox", g_strdup (inbox), g_free);
          refresh_inbox (service, g_task_get_cancellable (G_TASK (result)), mail_view, inbox);
        }
    }
  else if (noted->connect_error == NULL)
    {
      g_warning ("Ordner %s (%s): %s", display_name, uid, noted->folders_error->message);
      detail = g_strdup_printf ("%s: %s", display_name, noted->folders_error->message);
      revolution_mail_view_show_error (mail_view, _("Folders Not Loaded"), detail, TRUE);
    }
  else
    g_debug ("Ordner: %s nicht geladen: %s", uid, noted->folders_error->message);

  /* #350: Konto ohne Ordner gilt für die Erstauswahl als fertig (mail-view.c). */
  if (noted->folders_error != NULL && !merge)
    revolution_mail_view_set_folders (mail_view, uid, NULL);

  /* #421: Store mit gescheiterter Verbindung beim nächsten Netzwechsel erneut verbinden (retry_stores). */
  fallback = noted->fallback;
  g_object_set_data (G_OBJECT (service), "revolution-retry", GINT_TO_POINTER (fallback));
  note_store_result_free (noted);

  if (again && online && !fallback)
    revolution_mail_view_refresh_account (mail_view, uid);
}

/* #408: Ein Auftrag je gemeldetem Store (note_stores) für einen Netzwechsel, mit dem
 * GCancellable dieser Umschaltung. Rückgabe: Zahl der Aufträge. */
static guint
switch_stores (RevolutionMailSession *session,
               RevolutionMailView    *mail_view,
               gpointer               tag)
{
  g_autolist (CamelService) services = camel_session_list_services (CAMEL_SESSION (session));
  GCancellable *cancellable = new_line_change ();
  guint count = 0;

  for (GList *l = services; l != NULL; l = l->next)
    {
      g_autoptr (GTask) task = NULL;

      if (!CAMEL_IS_STORE (l->data) || g_object_get_data (l->data, "revolution-noted") == NULL)
        continue;

      task = g_task_new (l->data, cancellable, on_store_noted, g_object_ref (mail_view));
      g_task_set_source_tag (task, tag);
      g_task_run_in_thread (task, note_store_thread);
      count++;
    }

  return count;
}

static void
on_folder_opened (RevolutionMailView *mail_view,
                  CamelFolder        *folder)
{
  revolution_mail_view_watch_folder (mail_view, folder, FALSE, FALSE);
}

/* #362: Postausgang senden (mail-outbox.c) im Worker, ein Lauf zur Zeit; eine Anforderung während eines
 * Laufs startet danach einen weiteren (wie Evolution info->again, mail-send-recv.c:1119-1137). Auslöser
 * wie Evolution: neue Nachricht im Postausgang (Signal „flush-outbox“ der Session, e-mail-backend.c:
 * 1319-1321), Start („send-recv-on-start“, Vorgabe an, „This includes also sending messages from Outbox“,
 * org.gnome.evolution.mail.gschema.xml.in:794-797), Netz zurück (go_online); dazu nach einem
 * gescheiterten Lauf F5 (Evolution: Senden/Empfangen), Netzwechsel (retry_stores) und ein verbundener
 * Store (on_connection_status). Offline bleibt alles im Postausgang (mail-ops.c:656-660). Fehler je
 * Versanddienst ins Sammel-Banner (#420), weg nach dem nächsten erfolgreichen Versand über ihn. */
static gboolean outbox_running;
static gboolean outbox_again;
static gboolean outbox_again_all;
/* Letzter Lauf mit Fehler: weitere Auslöser (F5, Netzwechsel, verbunden) senden erneut. */
static gboolean outbox_failed;
static GCancellable *outbox_cancellable;

static void outbox_flush (RevolutionMailView *mail_view, gboolean all, const char *reason);

static void
outbox_flush_thread (GTask        *task,
                     gpointer      source_object,
                     gpointer      task_data,
                     GCancellable *cancellable)
{
  g_task_return_pointer (task,
                         revolution_mail_outbox_flush_sync (source_object, GPOINTER_TO_INT (task_data), cancellable),
                         (GDestroyNotify) revolution_mail_outbox_flush_free);
}

static void
on_outbox_flushed (GObject      *source_object G_GNUC_UNUSED,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  g_autoptr (RevolutionMailView) mail_view = user_data;
  RevolutionMailOutboxFlush *flush = g_task_propagate_pointer (G_TASK (result), NULL);
  gboolean all = outbox_again_all;

  outbox_running = FALSE;
  g_clear_object (&outbox_cancellable);
  /* Auch nach einem abgebrochenen Lauf (Netz weg): Ergebnis und Folgelauf nicht verlieren
   * (g_task_set_check_cancellable in outbox_flush). */
  if (flush != NULL)
    {
      for (guint i = 0; i < flush->sent_uids->len; i++)
        revolution_mail_view_clear_account_errors (mail_view, flush->sent_uids->pdata[i]);
      for (guint i = 0; i < flush->failed_uids->len; i++)
        {
          g_debug ("Postausgang: Fehler im Banner: %s %s", (const char *) flush->failed_uids->pdata[i],
                   (const char *) flush->failed_titles->pdata[i]);
          revolution_mail_view_show_account_error (mail_view, flush->failed_uids->pdata[i],
                                                   flush->failed_titles->pdata[i], flush->failed_details->pdata[i]);
        }
      outbox_failed = flush->failed_uids->len > 0 && flush->left > 0;
      revolution_mail_view_set_outbox (mail_view, flush->left);
      revolution_mail_outbox_flush_free (flush);
    }

  if (outbox_again)
    {
      outbox_again = FALSE;
      outbox_again_all = FALSE;
      outbox_flush (mail_view, all, "erneut angefordert");
    }
}

static void
outbox_flush (RevolutionMailView *mail_view,
              gboolean            all,
              const char         *reason)
{
  RevolutionMailSession *session = g_object_get_data (G_OBJECT (g_application_get_default ()), "session");
  g_autoptr (GTask) task = NULL;

  if (session == NULL)
    return;

  if (outbox_running)
    {
      outbox_again = TRUE;
      outbox_again_all |= all;
      return;
    }

  g_debug ("Postausgang: Sendelauf (%s)", reason);
  outbox_running = TRUE;
  /* Camel-Vorgang: Netz weg bricht ihn ab wie alle (camel_operation_cancel_all, on_network_changed). */
  outbox_cancellable = camel_operation_new ();
  task = g_task_new (session, outbox_cancellable, on_outbox_flushed, g_object_ref (mail_view));
  g_task_set_source_tag (task, outbox_flush);
  /* Auch ein abgebrochener Lauf liefert sein Ergebnis (Banner, Zahl, Folgelauf in on_outbox_flushed). */
  g_task_set_check_cancellable (task, FALSE);
  g_task_set_task_data (task, GINT_TO_POINTER (all), NULL);
  g_task_run_in_thread (task, outbox_flush_thread);
}

static void
on_flush_outbox (RevolutionMailView *mail_view)
{
  outbox_flush (mail_view, FALSE, "neue Nachricht");
}

/* #362: Nur für Tests (REVOLUTION_TEST_HOOKS=1, tests/senden.sh): Aktion app.test-send (Datei, Identität)
 * über org.gtk.Actions. Liest die .eml-Datei im Worker und sendet sie wie der Composer
 * (revolution_mail_send_message). */
static void
test_read_eml_thread (GTask        *task,
                      gpointer      source_object G_GNUC_UNUSED,
                      gpointer      task_data,
                      GCancellable *cancellable)
{
  g_autoptr (CamelMimeMessage) message = camel_mime_message_new ();
  g_autoptr (CamelStream) stream = NULL;
  GError *error = NULL;

  stream = camel_stream_fs_new_with_name (task_data, O_RDONLY, 0, &error);
  if (stream == NULL ||
      !camel_data_wrapper_construct_from_stream_sync (CAMEL_DATA_WRAPPER (message), stream, cancellable, &error))
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, g_steal_pointer (&message), g_object_unref);
}

static void
on_test_sent (GObject      *source_object G_GNUC_UNUSED,
              GAsyncResult *result,
              gpointer      user_data)
{
  g_autofree char *path = user_data;
  g_autoptr (GError) error = NULL;

  if (revolution_mail_send_message_finish (result, &error))
    g_debug ("Test: %s im Postausgang", path);
  else
    g_debug ("Test: %s nicht im Postausgang: %s", path, error->message);
}

static void
on_test_eml_read (GObject      *source_object G_GNUC_UNUSED,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  g_autofree char *identity = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (CamelMimeMessage) message = g_task_propagate_pointer (G_TASK (result), &error);
  const char *path = g_task_get_task_data (G_TASK (result));

  if (message == NULL)
    {
      g_debug ("Test: %s nicht gelesen: %s", path, error->message);
      return;
    }

  revolution_mail_send_message (message, identity, NULL, on_test_sent, g_strdup (path));
}

static void
on_test_send (GSimpleAction *action G_GNUC_UNUSED,
              GVariant      *parameter,
              gpointer       user_data)
{
  const char *path, *identity;
  g_autoptr (GTask) task = NULL;

  g_variant_get (parameter, "(&s&s)", &path, &identity);
  task = g_task_new (user_data, NULL, on_test_eml_read, g_strdup (identity));
  g_task_set_source_tag (task, on_test_send);
  g_task_set_task_data (task, g_strdup (path), g_free);
  g_task_run_in_thread (task, test_read_eml_thread);
}

/* #420: Verbindungsfehler eines Kontos aus dem Sammel-Banner, sobald sein Store verbunden ist, egal
 * wodurch: Start, Netzwechsel, F5 (retry_stores) oder EDS beim nächsten Zugriff
 * (store_maybe_connect_sync, folder_maybe_connect_sync: camel-store.c:262-315, camel-folder.c:643-688).
 * Nur bei CONNECTED. Muster: Evolutions Status-Symbol des Kontos folgt demselben Signal
 * (em-folder-tree-model.c:260-267, 1931-1940). CamelService meldet den Status im
 * Hauptkontext der Session (camel-service.c:458-477, camel_session_idle_add), hier der Standard-Kontext
 * (Session im Worker ohne eigenen Kontext angelegt, camel-session.c:748). */
static void
on_connection_status (CamelService       *service,
                      GParamSpec         *pspec G_GNUC_UNUSED,
                      RevolutionMailView *mail_view)
{
  if (camel_service_get_connection_status (service) == CAMEL_SERVICE_CONNECTED)
    {
      revolution_mail_view_clear_account_errors (mail_view, camel_service_get_uid (service));
      /* #362: Verbindung wieder da: Postausgang nach gescheitertem Lauf erneut senden. */
      if (outbox_failed)
        outbox_flush (mail_view, FALSE, "Store verbunden");
    }
}

static void
free_lock (GMutex *lock)
{
  g_mutex_clear (lock);
  g_free (lock);
}

/* #366: Abruf alle n Minuten wie Evolution: je Store ein Zeitgeber nach der ESourceRefresh des Kontos
 * (libemail-engine/e-mail-session.c:638-650, e_source_refresh_add_timeout; Standard an, 60 Minuten,
 * EDS e-source-refresh.c:357-360, 385-388). Bei Ablauf ruft Evolution den Store ab, wenn die Session
 * online ist (mail/e-mail-ui-session.c:1058-1065); hier der Posteingang wie nach dem Start. Für
 * microsoft365 der einzige Weg zu neuen Mails ohne F5 (kein Push, docs/ENTWICKLUNG.md #353). */
static void
on_refresh_timeout (ESource  *source,
                    gpointer  user_data)
{
  CamelSession *session = g_object_get_data (G_OBJECT (g_application_get_default ()), "session");
  g_autoptr (CamelService) service = NULL;
  const char *inbox = NULL;

  if (session == NULL || !camel_session_get_online (session))
    return;

  service = camel_session_ref_service (session, e_source_get_uid (source));
  if (service != NULL)
    inbox = g_object_get_data (G_OBJECT (service), "revolution-inbox");
  g_debug ("Abruf: %s %s", e_source_get_uid (source), inbox != NULL ? inbox : "ohne Posteingang");
  if (inbox != NULL)
    refresh_inbox (service, line, user_data, inbox);
}

static void
add_refresh_timeout (CamelService       *service,
                     RevolutionMailView *mail_view)
{
  ESourceRegistry *registry = g_object_get_data (G_OBJECT (g_application_get_default ()), "registry");
  g_autoptr (ESource) source = registry != NULL ? e_source_registry_ref_source (registry, camel_service_get_uid (service))
                                                : NULL;
  ESourceRefresh *refresh;

  /* Einmal je Konto, auch wenn die Session den Store neu anlegt. */
  if (source == NULL || g_object_get_data (G_OBJECT (source), "revolution-refresh") != NULL)
    return;

  g_object_set_data (G_OBJECT (source), "revolution-refresh", GINT_TO_POINTER (TRUE));
  e_source_refresh_add_timeout (source, NULL, on_refresh_timeout, g_object_ref (mail_view), g_object_unref);
  refresh = e_source_get_extension (source, E_SOURCE_EXTENSION_REFRESH);
  g_debug ("Abruf: %s alle %u Minuten%s", e_source_get_uid (source), e_source_refresh_get_interval_minutes (refresh),
           e_source_refresh_get_enabled (refresh) ? "" : " (aus)");
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
  /* #408: GCancellable der laufenden Umschaltung (new_line_change), damit ein Netzwechsel auch
   * das erste Verbinden ablöst. */
  GCancellable *cancellable = line;

  for (guint i = 0; errors[i] != NULL; i++)
    revolution_mail_view_show_error (mail_view, _("Account Not Set Up"), errors[i], TRUE);

  /* #351: Quellen entfernter Stores aus den Favoriten nehmen (mail-favorites.c). */
  revolution_mail_favorites_sync_session (CAMEL_SESSION (session));

  for (GList *l = services; l != NULL; l = l->next)
    {
      g_autoptr (GTask) task = NULL;

      /* #351: der Suchordner-Store der Favoriten hat kein Konto (mail-favorites.c); #362 ebenso der Store
       * des Postausgangs (mail-outbox.c). */
      if (!CAMEL_IS_STORE (l->data) || CAMEL_IS_VEE_STORE (l->data) ||
          g_strcmp0 (camel_service_get_uid (l->data), REVOLUTION_MAIL_LOCAL_UID) == 0 ||
          g_object_get_data (l->data, "revolution-noted") != NULL)
        continue;

      g_object_set_data (l->data, "revolution-noted", GINT_TO_POINTER (TRUE));
      {
        GMutex *lock = g_new0 (GMutex, 1);

        /* #408: Aufträge je Store nacheinander (note_store_thread). */
        g_mutex_init (lock);
        g_object_set_data_full (l->data, "revolution-line-lock", lock, (GDestroyNotify) free_lock);
      }
      /* #350: geöffnete Ordner für den Ungelesen-Zähler beobachten wie Evolution
       * (store_folder_opened_cb, mail-folder-cache.c:1311-1317); das Signal kommt im
       * Hauptkontext (EDS camel-store.c:895-925). */
      g_signal_connect_object (l->data, "folder-opened", G_CALLBACK (on_folder_opened), mail_view,
                               G_CONNECT_SWAPPED);
      g_signal_connect_object (l->data, "notify::connection-status", G_CALLBACK (on_connection_status), mail_view,
                               G_CONNECT_DEFAULT);
      add_refresh_timeout (l->data, mail_view);
      task = g_task_new (l->data, cancellable, on_store_noted, g_object_ref (mail_view));
      g_task_set_source_tag (task, note_stores);
      g_task_run_in_thread (task, note_store_thread);
    }
}

/* #351: Nur für Tests (REVOLUTION_TEST_HOOKS=1, tests/kombiniert.sh), Muster wie REVOLUTION_FOLDER:
 * Aktion app.test-store (UID, vorhanden) über org.gtk.Actions am Session-Bus. FALSE entfernt den
 * Store dieses Kontos aus der Session wie mail-session.c beim Abgleich (camel_session_remove_service)
 * und meldet danach „services-changed“ wie dort; TRUE stößt den Abgleich der Session über das
 * Registry-Signal „source-changed“ an (mail-session.c, mail_session_registry_changed_cb), der den
 * Store des weiterhin angezeigten Kontos neu anlegt. Ohne Änderung an GOA oder ESource. */
static void
test_remove_store_thread (GTask        *task,
                          gpointer      source_object,
                          gpointer      task_data,
                          GCancellable *cancellable G_GNUC_UNUSED)
{
  g_autoptr (CamelService) service = camel_session_ref_service (source_object, task_data);

  if (service != NULL)
    camel_session_remove_service (source_object, service);
  g_debug ("Test: Store %s %s", (const char *) task_data, service != NULL ? "entfernt" : "nicht gefunden");
  g_task_return_boolean (task, TRUE);
}

static void
on_test_store_removed (GObject      *source_object,
                       GAsyncResult *result G_GNUC_UNUSED,
                       gpointer      user_data G_GNUC_UNUSED)
{
  g_signal_emit_by_name (source_object, "services-changed");
}

static void
on_test_store (GSimpleAction *action G_GNUC_UNUSED,
               GVariant      *parameter,
               gpointer       user_data)
{
  GApplication *app = user_data;
  CamelSession *session = g_object_get_data (G_OBJECT (app), "session");
  ESourceRegistry *registry = g_object_get_data (G_OBJECT (app), "registry");
  g_autoptr (ESource) source = NULL;
  const char *uid;
  gboolean present;

  g_variant_get (parameter, "(&sb)", &uid, &present);
  if (session == NULL || registry == NULL)
    return;

  if (!present)
    {
      g_autoptr (GTask) task = g_task_new (session, NULL, on_test_store_removed, NULL);

      g_task_set_source_tag (task, on_test_store);
      g_task_set_task_data (task, g_strdup (uid), g_free);
      g_task_run_in_thread (task, test_remove_store_thread);
      return;
    }

  source = e_source_registry_ref_source (registry, uid);
  g_debug ("Test: Store %s Abgleich", uid);
  if (source != NULL)
    g_signal_emit_by_name (registry, "source-changed", source);
}

/* #366: Nur für Tests (REVOLUTION_TEST_HOOKS=1): Aktion app.test-refresh (UID) löst den Abruf-Zeitgeber
 * des Kontos sofort aus (e_source_refresh_force_timeout, EDS e-source-refresh.c), statt n Minuten zu warten. */
static void
on_test_refresh (GSimpleAction *action G_GNUC_UNUSED,
                 GVariant      *parameter,
                 gpointer       user_data)
{
  ESourceRegistry *registry = g_object_get_data (G_OBJECT (user_data), "registry");
  g_autoptr (ESource) source = registry != NULL ? e_source_registry_ref_source (registry, g_variant_get_string (parameter, NULL))
                                                : NULL;

  g_debug ("Test: Abruf %s %s", g_variant_get_string (parameter, NULL), source != NULL ? "ausgelöst" : "unbekannt");
  if (source != NULL)
    e_source_refresh_force_timeout (source);
}

/* #408: Online/offline nach Netzstatus, nach Evolution: EShell folgt „network-available“ des
 * ENetworkMonitor (e-shell.c:1845-1850); ohne Netz sofort offline, mit Netz erst nach
 * SET_ONLINE_TIMEOUT_SECONDS wieder online, „damit das Netz ganz verfügbar ist“; ein erneuter
 * Ausfall in dieser Zeit hebt das Online-Schalten auf (e_shell_set_network_available,
 * e-shell.c:45, 3185-3232). Die Mail folgt in mail_backend_prepare_for_offline_cb und
 * _online_cb (e-mail-backend.c:211-253, 294-340): Session offline, laufende Camel-Vorgänge
 * abbrechen (camel_operation_cancel_all), jeden Store im Worker offline
 * (e_mail_store_go_offline_sync, e-mail-store-utils.c:147-180); online: Session online, jeden
 * Store im Worker online (e_mail_store_go_online_sync, dort 214-240) und abrufen. Beides im selben
 * Auftrag je Store (note_store_thread, switch_stores).
 * Gekürzt: ohne Rückfrage zum Herunterladen für offline (mail:ask-quick-offline, e-mail-backend.c:
 * 242-254; Revolution lädt nichts für offline vor) und ohne die EActivity des Shells. Abrufen
 * nach dem Online-Schalten wie beim Start (note_store_thread: Ordnerliste, Posteingang) statt
 * e_source_refresh_force_timeout (e-mail-backend.c:141-176). Revolution hat keinen Schalter
 * „Offline arbeiten“; daher entfällt auto_reconnect (online geht es immer, wenn das Netz da ist). */
#define SET_ONLINE_TIMEOUT_SECONDS 5

static RevolutionMailSession *
app_session (void)
{
  return g_object_get_data (G_OBJECT (g_application_get_default ()), "session");
}

static gboolean
go_online (gpointer user_data)
{
  RevolutionMailView *mail_view = user_data;
  RevolutionMailSession *session = app_session ();

  online_id = 0;
  camel_session_set_online (CAMEL_SESSION (session), TRUE);
  revolution_mail_view_set_offline (mail_view, FALSE);
  g_debug ("Netz: online, %u Stores verbinden", switch_stores (session, mail_view, go_online));
  /* #362 */
  outbox_flush (mail_view, FALSE, "Netz zurück");

  return G_SOURCE_REMOVE;
}

static void
on_network_changed (GNetworkMonitor    *monitor,
                    GParamSpec         *pspec G_GNUC_UNUSED,
                    RevolutionMailView *mail_view)
{
  RevolutionMailSession *session = app_session ();

  if (session == NULL)
    return;

  if (g_network_monitor_get_network_available (monitor))
    {
      if (camel_session_get_online (CAMEL_SESSION (session)) || online_id != 0)
        return;

      g_debug ("Netz: verfügbar, online in %d s", SET_ONLINE_TIMEOUT_SECONDS);
      online_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, SET_ONLINE_TIMEOUT_SECONDS, go_online,
                                              g_object_ref (mail_view), g_object_unref);
      return;
    }

  g_clear_handle_id (&online_id, g_source_remove);
  g_clear_handle_id (&retry_id, g_source_remove);
  if (!camel_session_get_online (CAMEL_SESSION (session)))
    return;

  camel_session_set_online (CAMEL_SESSION (session), FALSE);
  camel_operation_cancel_all ();
  revolution_mail_view_set_offline (mail_view, TRUE);
  g_debug ("Netz: nicht verfügbar, offline, %u Stores trennen", switch_stores (session, mail_view, on_network_changed));
}

/* #421: Stores, deren Verbindung bei Session online scheiterte (note_store_thread, Marke
 * „revolution-retry“ aus on_store_noted), erneut verbinden: derselbe Auftrag wie beim Netzwechsel,
 * im GCancellable der laufenden Umschaltung (ein Wechsel nach offline bricht ihn ab). Auslöser ist
 * „network-changed“ des Netzmonitors ohne Wechsel von „network-available“ (z. B. neue Route), wie
 * Stamp jedes Konto bei „network-changed“ verbindet (stamp-account-item.c:324-350, 719); EDS prüft
 * dabei die Erreichbarkeit seiner Stores nach 5 s neu (camel-network-service.c:414-469, 507),
 * Evolution schaltet nur bei „network-available“ (e-shell.c:1845-1850). Hier ebenso nach
 * SET_ONLINE_TIMEOUT_SECONDS seit dem letzten Signal. #420: Den Fehler eines Kontos nimmt
 * on_connection_status aus dem Banner, sobald es verbunden ist; ein erneuter Fehler ersetzt ihn. */
static gboolean
retry_stores (gpointer user_data)
{
  RevolutionMailView *mail_view = user_data;
  RevolutionMailSession *session = app_session ();
  g_autolist (CamelService) services = NULL;
  guint count = 0;

  retry_id = 0;
  if (session == NULL || !camel_session_get_online (CAMEL_SESSION (session)))
    return G_SOURCE_REMOVE;

  services = camel_session_list_services (CAMEL_SESSION (session));
  for (GList *l = services; l != NULL; l = l->next)
    {
      g_autoptr (GTask) task = NULL;

      if (!CAMEL_IS_STORE (l->data) || g_object_get_data (l->data, "revolution-retry") == NULL)
        continue;

      g_object_set_data (l->data, "revolution-retry", NULL);
      count++;
      task = g_task_new (l->data, line, on_store_noted, g_object_ref (mail_view));
      g_task_set_source_tag (task, retry_stores);
      g_task_run_in_thread (task, note_store_thread);
    }
  if (count > 0)
    g_debug ("Netz: %u Stores erneut verbinden", count);
  /* #362 */
  if (outbox_failed)
    outbox_flush (mail_view, FALSE, "Netzwechsel");

  return G_SOURCE_REMOVE;
}

/* Nur wenn schon ein Store gescheitert ist: ein Netzwechsel davor (auch der beim Anlegen des
 * Monitors, e-network-monitor.c:636) betrifft keine gescheiterte Verbindung. */
static void
on_network_event (RevolutionMailView *mail_view)
{
  RevolutionMailSession *session = app_session ();
  g_autolist (CamelService) services = NULL;
  gboolean failed = FALSE;

  if (session == NULL || !camel_session_get_online (CAMEL_SESSION (session)) || online_id != 0)
    return;

  /* #362: auch nach gescheitertem Sendelauf. */
  failed = outbox_failed;
  services = camel_session_list_services (CAMEL_SESSION (session));
  for (GList *l = services; l != NULL && !failed; l = l->next)
    failed = g_object_get_data (l->data, "revolution-retry") != NULL;
  if (!failed)
    return;

  g_debug ("Netz: Netzwechsel, erneut verbinden in %d s", SET_ONLINE_TIMEOUT_SECONDS);
  g_clear_handle_id (&retry_id, g_source_remove);
  retry_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, SET_ONLINE_TIMEOUT_SECONDS, retry_stores,
                                         g_object_ref (mail_view), g_object_unref);
}

/* #421: F5 (mail-view.c, Signal „retry-stores“): gescheiterte Stores gleich erneut verbinden. Rückgabe
 * TRUE, wenn der Store des Kontos uid dazugehört; dessen Liste folgt dann aus on_store_noted. */
static gboolean
on_retry_requested (RevolutionMailView *mail_view,
                    const char         *uid,
                    gpointer            user_data G_GNUC_UNUSED)
{
  RevolutionMailSession *session = app_session ();
  g_autoptr (CamelService) service = NULL;
  gboolean waits;

  if (session == NULL || !camel_session_get_online (CAMEL_SESSION (session)))
    return FALSE;

  service = uid != NULL ? camel_session_ref_service (CAMEL_SESSION (session), uid) : NULL;
  waits = service != NULL && g_object_get_data (G_OBJECT (service), "revolution-retry") != NULL;
  /* #362: F5 sendet den Postausgang, auch Nachrichten mit unklarer Zustellung (mail-outbox.c); vor
   * retry_stores, dessen eigener Auslöser dann nur einen Folgelauf anfordert. */
  outbox_flush (mail_view, TRUE, "F5");
  g_clear_handle_id (&retry_id, g_source_remove);
  retry_stores (mail_view);

  return waits;
}

static void
on_shutdown_network (GApplication *app G_GNUC_UNUSED)
{
  g_clear_handle_id (&online_id, g_source_remove);
  g_clear_handle_id (&retry_id, g_source_remove);
  if (line != NULL)
    g_cancellable_cancel (line);
  g_clear_object (&line);
  /* #362 */
  if (outbox_cancellable != NULL)
    g_cancellable_cancel (outbox_cancellable);
}

/* #366: mailto-Links, bis Registry und Session geladen sind (Absender, Senden), wie Stamp pending_mailto
 * (stamp-application.c:176-187, 211-214). */
static GPtrArray *pending_mailto;
/* #366: Hauptfenster (on_activate), NULL ohne. Composer-Fenster gehören auch zur Anwendung, daher nicht
 * gtk_application_get_active_window. */
static GtkWindow *main_window;

static void
open_mailto (GApplication *app,
             const char   *uri)
{
  g_debug ("mailto: Composer für %s", uri);
  gtk_window_present (revolution_composer_new_mailto (GTK_APPLICATION (app), g_object_get_data (G_OBJECT (app), "registry"),
                                                      g_object_get_data (G_OBJECT (app), "session"), uri));
}

static void
on_session_loaded (GObject      *source_object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
  g_autoptr (RevolutionMailView) mail_view = user_data;
  RevolutionMailSession *session = g_task_propagate_pointer (G_TASK (result), NULL);
  g_autolist (CamelService) services = camel_session_list_services (CAMEL_SESSION (session));

  g_debug ("CamelSession: bereit, Rückruf im Thread %p, %u Stores",
           (gpointer) g_thread_self (), g_list_length (services));
  /* An der Anwendung gehalten wie die Registry (g_object_get_data (app, "session")). */
  g_object_set_data_full (source_object, "session", session, g_object_unref);

  revolution_mail_view_set_session (mail_view, CAMEL_SESSION (session));
  for (guint i = 0; pending_mailto != NULL && i < pending_mailto->len; i++)
    open_mailto (G_APPLICATION (source_object), pending_mailto->pdata[i]);
  g_clear_pointer (&pending_mailto, g_ptr_array_unref);
  if (g_strcmp0 (g_getenv ("REVOLUTION_TEST_HOOKS"), "1") == 0)
    {
      const GActionEntry test_actions[] = {
        { "test-store", on_test_store, "(sb)", NULL, NULL, { 0 } },
        { "test-refresh", on_test_refresh, "s", NULL, NULL, { 0 } },
        { "test-send", on_test_send, "(ss)", NULL, NULL, { 0 } },
      };

      g_action_map_add_action_entries (G_ACTION_MAP (source_object), test_actions, G_N_ELEMENTS (test_actions),
                                       source_object);
    }

  /* #408: Start ohne Netz: Session offline (load_session_thread), Hinweis gleich zeigen. Danach
   * dem Netzmonitor folgen; der Abgleich hier fängt eine Änderung seit load_session_thread. */
  if (!camel_session_get_online (CAMEL_SESSION (session)))
    {
      g_debug ("Netz: nicht verfügbar, Start offline");
      revolution_mail_view_set_offline (mail_view, TRUE);
    }
  g_signal_connect_object (e_network_monitor_get_default (), "notify::network-available",
                           G_CALLBACK (on_network_changed), mail_view, G_CONNECT_DEFAULT);
  /* #421: Netzwechsel ohne Wechsel der Verfügbarkeit (retry_stores). */
  g_signal_connect_object (e_network_monitor_get_default (), "network-changed",
                           G_CALLBACK (on_network_event), mail_view, G_CONNECT_SWAPPED);
  g_signal_connect (mail_view, "retry-stores", G_CALLBACK (on_retry_requested), NULL);
  /* Beim Beenden die laufende Umschaltung abbrechen (auch das erste Verbinden, note_stores). */
  g_signal_connect (source_object, "shutdown", G_CALLBACK (on_shutdown_network), NULL);

  g_signal_connect_object (session, "services-changed", G_CALLBACK (note_stores), mail_view, G_CONNECT_DEFAULT);
  /* #362 */
  g_signal_connect_object (session, "flush-outbox", G_CALLBACK (on_flush_outbox), mail_view, G_CONNECT_SWAPPED);
  new_line_change ();
  note_stores (session, mail_view);
  on_network_changed (G_NETWORK_MONITOR (e_network_monitor_get_default ()), NULL, mail_view);
  outbox_flush (mail_view, FALSE, "Start");
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

  message = g_strdup_printf (_("Could not load accounts: %s"), error->message);
  g_warning ("%s", message);
  revolution_mail_view_show_error (mail_view, _("Accounts Unavailable"), message, FALSE);
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

  window = main_window;
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
      /* #366: Schließen verbirgt das Fenster nur, die Anwendung läuft weiter, wie Stamp
       * (stamp-window.c:203); nur lesend gebunden, damit on_quit es zerstören kann. */
      g_settings_bind (settings, "run-in-background", window, "hide-on-close", G_SETTINGS_BIND_GET);
      main_window = window;
      g_object_add_weak_pointer (G_OBJECT (window), (gpointer *) &main_window);
#ifdef DEVEL
      /* Entwicklungsvariante gestreift wie Stamp (stamp-window.c:221-223). */
      gtk_widget_add_css_class (GTK_WIDGET (window), "devel");
#endif

      mail_view = REVOLUTION_MAIL_VIEW (gtk_builder_get_object (builder, "mail_view"));
      g_object_set_data (G_OBJECT (window), "mail-view", mail_view);
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

/* #366: mailto-Link als Argument oder von der Standard-Mail-App (Desktop-Datei MimeType
 * x-scheme-handler/mailto, Exec %U): Hauptfenster und Composer wie Stamp (stamp_application_command_line,
 * stamp-application.c:191-221). Läuft Revolution schon, kommt der Link über D-Bus an diese Instanz. */
static void
on_open (GApplication  *app,
         GFile        **files,
         int            n_files,
         const char    *hint G_GNUC_UNUSED,
         GSettings     *settings)
{
  on_activate (GTK_APPLICATION (app), settings);

  for (int i = 0; i < n_files; i++)
    {
      g_autofree char *uri = g_file_get_uri (files[i]);

      if (g_ascii_strncasecmp (uri, "mailto:", 7) != 0)
        g_debug ("Öffnen: %s ist kein mailto-Link", uri);
      else if (g_object_get_data (G_OBJECT (app), "session") != NULL)
        open_mailto (app, uri);
      else
        {
          if (pending_mailto == NULL)
            pending_mailto = g_ptr_array_new_with_free_func (g_free);
          g_ptr_array_add (pending_mailto, g_steal_pointer (&uri));
        }
    }
}

/* #366: Klick auf die Benachrichtigung wie Evolution notify_default_action_cb
 * (plugins/mail-notification/mail-notification.c:409-476): Fenster zeigen, zur Mail-Ansicht, Ordner und
 * Nachricht wählen, Benachrichtigung entfernen. */
static void
on_show_message (GSimpleAction *action G_GNUC_UNUSED,
                 GVariant      *parameter,
                 gpointer       user_data)
{
  GApplication *app = g_application_get_default ();
  const char *folder_uri, *uid;

  g_variant_get (parameter, "(&s&s)", &folder_uri, &uid);
  g_debug ("Benachrichtigung: geöffnet %s %s", folder_uri, uid);
  on_activate (GTK_APPLICATION (app), user_data);
  revolution_mail_view_show_message (g_object_get_data (G_OBJECT (main_window), "mail-view"), folder_uri, uid);
  revolution_mail_notification_reset ();
}

/* #366: Beenden (Strg+Q, Hauptmenü), nötig, wenn Schließen das Fenster nur verbirgt (run-in-background).
 * Das Hauptfenster wird zerstört, die übrigen (Composer) geschlossen; die Anwendung endet mit dem letzten
 * Fenster und wartet dabei laufende Flag-Aufträge ab (mail-view.c, hold_for_flags). */
static void
on_quit (GSimpleAction *action G_GNUC_UNUSED,
         GVariant      *parameter G_GNUC_UNUSED,
         gpointer       user_data G_GNUC_UNUSED)
{
  g_autolist (GtkWindow) windows = g_list_copy (gtk_application_get_windows (GTK_APPLICATION (g_application_get_default ())));

  g_debug ("Beenden: %u Fenster", g_list_length (windows));
  for (GList *l = windows; l != NULL; l = l->next)
    {
      g_object_ref (l->data);
      if (l->data == main_window)
        gtk_window_destroy (l->data);
      else
        gtk_window_close (l->data);
    }
}

int
main (int    argc,
      char **argv)
{
  g_autoptr (AdwApplication) app = NULL;
  g_autoptr (GSettings) settings = NULL;
  g_autofree char *exe = g_file_read_link ("/proc/self/exe", NULL);
  g_autofree char *exe_dir = exe != NULL ? g_path_get_dirname (exe) : NULL;

  /* #438: Aus dem Build-Ordner gestartet die dort gebauten Übersetzungen, sonst die installierten
   * (gleiche Erkennung wie die Web-Erweiterung, mail-reader.c extensions_directory). */
  bindtextdomain (GETTEXT_PACKAGE, g_strcmp0 (exe_dir, BUILD_SRC_DIR) == 0 ? BUILD_LOCALE_DIR : LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  g_set_application_name (_("Revolution"));

  /* Ein Schema für beide Profile wie GNOME Calendar (org.gnome.calendar). */
  settings = g_settings_new ("de.freiraum_medien.Revolution");
  app = adw_application_new (APPLICATION_ID, G_APPLICATION_HANDLES_OPEN);
  /* Fester Pfad für beide Profile (style.css, Icons), wie GNOME Calendar
   * (gcal-application.c:614) und Stamp (stamp-application.c:57). */
  g_application_set_resource_base_path (G_APPLICATION (app), "/de/freiraum_medien/Revolution");
  g_signal_connect (app, "startup", G_CALLBACK (on_startup), settings);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), settings);
  g_signal_connect (app, "open", G_CALLBACK (on_open), settings);

  return g_application_run (G_APPLICATION (app), argc, argv);
}
