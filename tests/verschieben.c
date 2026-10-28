/* verschieben.c – Verschieben und Papierkorb ohne Oberfläche gegen ein Testkonto (#416)
 *
 * Nutzt RevolutionMailSession wie die App (Muster tests/anmeldung.c) und die Funktionen aus
 * src/eds-compat.c, die #363 aufruft. Nach jedem Schritt synchronisiert es alle offenen Ordner des
 * Kontos wie Camels Zeitgeber store-changes (EDS camel-folder.c:214-253): ein Echo ginge dann im
 * Graph-Log (M365_DEBUG=1) als PATCH raus. Ablauf und Prüfung: tests/verschieben.sh.
 *
 * Aufruf (G_MESSAGES_DEBUG=revolution zeigt die Rücksetzung „Abgleich: …“):
 *   verschieben [--roh] KONTO QUELLE ZIEL BETREFF   Nachricht mit genau diesem Betreff verschieben
 *   verschieben [--roh] KONTO abgleich ORDNER        Ordner holen und abgleichen (Abgleich mit Echo-Schutz)
 *   KONTO: Adresse eines der Testkonten aus der Umgebung TESTKONTEN (tests/testkonten.sh; Anzeigename
 *   des Stores), ORDNER/QUELLE/ZIEL: voller Camel-Name oder „Papierkorb“
 *   (revolution_eds_store_get_trash_folder_sync).
 *   --roh: Papierkorb und Verschieben direkt über Camel (camel_store_get_trash_folder_sync,
 *   camel_folder_transfer_messages_to_sync), Gegenprobe ohne Echo-Schutz.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <camel/camel.h>
#include <libedataserver/libedataserver.h>

#include "eds-compat.h"
#include "mail/mail-session.h"

static gboolean roh;

static CamelFolder *
ordner (CamelStore  *store,
        const char  *name,
        GError     **error)
{
  if (g_strcmp0 (name, "Papierkorb") == 0)
    return roh ? camel_store_get_trash_folder_sync (store, NULL, error)
               : revolution_eds_store_get_trash_folder_sync (store, NULL, error);

  return camel_store_get_folder_sync (store, name, 0, NULL, error);
}

static char *
uid_mit_betreff (CamelFolder *folder,
                 const char  *betreff)
{
  GPtrArray *uids = camel_folder_get_uids (folder);
  char *found = NULL;

  for (guint i = 0; i < uids->len && found == NULL; i++)
    {
      g_autoptr (CamelMessageInfo) info = camel_folder_get_message_info (folder, uids->pdata[i]);

      if (info != NULL && g_strcmp0 (camel_message_info_get_subject (info), betreff) == 0)
        found = g_strdup (uids->pdata[i]);
    }
  camel_folder_free_uids (folder, uids);

  return found;
}

/* Wie der Zeitgeber store-changes: jeden offenen Ordner synchronisieren (schickt folder_flagged). */
static gboolean
alle_synchronisieren (CamelStore  *store,
                      GError     **error)
{
  GPtrArray *opened = camel_store_dup_opened_folders (store);
  gboolean ok = TRUE;

  for (guint i = 0; i < opened->len; i++)
    {
      if (ok)
        ok = camel_folder_synchronize_sync (opened->pdata[i], FALSE, NULL, error);
      g_object_unref (opened->pdata[i]);
    }
  g_ptr_array_free (opened, TRUE);

  return ok;
}

static gboolean
verschieben (CamelStore  *store,
             const char  *quelle,
             const char  *ziel,
             const char  *betreff,
             GError     **error)
{
  g_autoptr (CamelFolder) source = ordner (store, quelle, error);
  g_autoptr (CamelFolder) destination = NULL;
  g_autoptr (GPtrArray) uids = g_ptr_array_new_with_free_func (g_free);
  char *uid;

  if (source == NULL || !revolution_eds_folder_refresh_info_sync (source, NULL, error))
    return FALSE;
  uid = uid_mit_betreff (source, betreff);
  if (uid == NULL)
    {
      g_set_error (error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID_UID, "„%s“ nicht in %s", betreff, quelle);
      return FALSE;
    }
  g_ptr_array_add (uids, uid);
  destination = ordner (store, ziel, error);
  if (destination == NULL)
    return FALSE;
  g_print ("VERSCHIEBEN %s %s -> %s\n", betreff, camel_folder_get_full_name (source),
           camel_folder_get_full_name (destination));

  return roh ? camel_folder_transfer_messages_to_sync (source, uids, destination, TRUE, NULL, NULL, error)
             : revolution_eds_folder_transfer_messages_to_sync (source, uids, destination, TRUE, NULL, NULL, error);
}

int
main (int    argc,
      char **argv)
{
  const GOptionEntry entries[] = {
    { "roh", 0, 0, G_OPTION_ARG_NONE, &roh, "ohne Echo-Schutz (Gegenprobe)", NULL },
    G_OPTION_ENTRY_NULL
  };
  g_autoptr (GOptionContext) context = g_option_context_new ("KONTO QUELLE ZIEL BETREFF | KONTO abgleich ORDNER");
  g_autoptr (GError) error = NULL;
  g_autoptr (ESourceRegistry) registry = NULL;
  g_autoptr (RevolutionMailSession) session = NULL;
  g_autolist (CamelService) services = NULL;
  g_autofree char *camel_dir = NULL;
  CamelStore *store = NULL;
  g_auto (GStrv) testkonten = NULL;
  gboolean ok;

  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error) ||
      !(argc == 5 || (argc == 4 && g_strcmp0 (argv[2], "abgleich") == 0)))
    {
      g_printerr ("%s\n", error != NULL ? error->message : "Aufruf: siehe tests/verschieben.c");
      return 2;
    }
  /* Schreibend freigegebene Testkonten aus tests/testkonten.sh (TESTKONTEN, ~/.config/revolution/testkonten.ini). */
  if (g_getenv ("TESTKONTEN") == NULL)
    {
      g_printerr ("TESTKONTEN fehlt (tests/testkonten.sh)\n");
      return 2;
    }
  testkonten = g_strsplit (g_getenv ("TESTKONTEN"), " ", -1);
  if (!g_strv_contains ((const char * const *) testkonten, argv[1]))
    {
      g_printerr ("kein Testkonto: %s\n", argv[1]);
      return 2;
    }

  registry = e_source_registry_new_sync (NULL, &error);
  if (registry == NULL)
    {
      g_printerr ("Registry: %s\n", error->message);
      return 2;
    }

  camel_dir = g_build_filename (g_get_user_data_dir (), "revolution", NULL);
  camel_init (camel_dir, FALSE);
  session = revolution_mail_session_new (registry);
  camel_session_set_online (CAMEL_SESSION (session), TRUE);

  services = camel_session_list_services (CAMEL_SESSION (session));
  for (GList *l = services; l != NULL && store == NULL; l = l->next)
    if (CAMEL_IS_STORE (l->data) && g_strcmp0 (camel_service_get_display_name (l->data), argv[1]) == 0)
      store = l->data;
  if (store == NULL)
    {
      g_printerr ("kein Store: %s\n", argv[1]);
      return 2;
    }

  /* Wie die App (src/main.c connect_service_thread). */
  ok = !CAMEL_IS_OFFLINE_STORE (store) ||
       camel_offline_store_set_online_sync (CAMEL_OFFLINE_STORE (store), TRUE, NULL, &error);
  if (ok && argc == 4)
    {
      g_autoptr (CamelFolder) folder = ordner (store, argv[3], &error);

      ok = folder != NULL && revolution_eds_folder_refresh_info_sync (folder, NULL, &error);
    }
  else if (ok)
    ok = verschieben (store, argv[2], argv[3], argv[4], &error);
  if (ok)
    ok = alle_synchronisieren (store, &error);
  camel_service_disconnect_sync (CAMEL_SERVICE (store), TRUE, NULL, NULL);

  /* Ausstehende Signale zustellen, solange Session und Store noch leben (wie tests/anmeldung.c). */
  while (g_main_context_iteration (NULL, FALSE))
    ;

  if (!ok)
    {
      g_print ("Fehler: %s\n", error != NULL ? error->message : "?");
      return 1;
    }
  g_print ("FERTIG\n");

  return 0;
}
