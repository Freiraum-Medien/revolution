/* anmeldung.c – Anmeldung einzelner Konten ohne Oberfläche prüfen (#347)
 *
 * Nutzt RevolutionMailSession wie die App (src/main.c: camel_init, Session, Store
 * online schalten) und gibt je Konto das Ergebnis aus. Für Fehlerfälle lassen sich Benutzer
 * bzw. Server NUR IM SPEICHER dieses Prozesses ändern; nichts wird gespeichert
 * (kein e_source_write). Zugangsdaten nur zur Laufzeit aus GOA bzw. Schlüsselbund.
 *
 * Aufruf (G_MESSAGES_DEBUG=revolution zeigt „angemeldet: …“):
 *   anmeldung UID…                              Konten verbinden
 *   anmeldung --benutzer NAME UID               fremder Benutzer (echte Ablehnung,
 *                                               genau ein Fehlversuch)
 *   anmeldung --zertifikat UID                  Server expired.badssl.com:443 (abgelaufenes
 *                                               Zertifikat, öffentlicher Prüfdienst); der
 *                                               TLS-Abbruch kommt vor jeder Anmeldung. Bewusst
 *                                               kein freier Host: bei gültigem Zertifikat
 *                                               ginge das Passwort an diesen Server.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <camel/camel.h>
#include <libedataserver/libedataserver.h>

#include "mail/mail-session.h"

int
main (int    argc,
      char **argv)
{
  g_autofree char *user = NULL;
  gboolean certificate = FALSE;
  const GOptionEntry entries[] = {
    { "benutzer", 0, 0, G_OPTION_ARG_STRING, &user, "Benutzername im Speicher ersetzen", "NAME" },
    { "zertifikat", 0, 0, G_OPTION_ARG_NONE, &certificate, "Server mit abgelaufenem Zertifikat", NULL },
    G_OPTION_ENTRY_NULL
  };
  g_autoptr (GOptionContext) context = g_option_context_new ("UID…");
  g_autoptr (GError) error = NULL;
  g_autoptr (ESourceRegistry) registry = NULL;
  g_autoptr (RevolutionMailSession) session = NULL;
  g_autofree char *camel_dir = NULL;
  int failures = 0;

  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error) || argc < 2)
    {
      g_printerr ("%s\n", error != NULL ? error->message : "UID fehlt");
      return 2;
    }

  registry = e_source_registry_new_sync (NULL, &error);
  if (registry == NULL)
    {
      g_printerr ("Registry: %s\n", error->message);
      return 2;
    }

  camel_dir = g_build_filename (g_get_user_data_dir (), USER_DIR_NAME, NULL);
  camel_init (camel_dir, FALSE);
  session = revolution_mail_session_new (registry);
  camel_session_set_online (CAMEL_SESSION (session), TRUE);

  for (int i = 1; i < argc; i++)
    {
      g_autoptr (CamelService) service = camel_session_ref_service (CAMEL_SESSION (session), argv[i]);
      g_autoptr (CamelSettings) settings = NULL;
      g_autoptr (GError) local_error = NULL;

      if (service == NULL)
        {
          g_print ("kein Store: %s\n", argv[i]);
          failures++;
          continue;
        }

      if (!CAMEL_IS_OFFLINE_STORE (service))
        {
          g_print ("kein Offline-Store: %s\n", argv[i]);
          failures++;
          continue;
        }

      /* Nur Stores mit Server und Port (imapx); microsoft365 hat keinen Host. */
      if (certificate && g_strcmp0 (camel_service_get_provider (service)->protocol, "imapx") != 0)
        {
          g_print ("--zertifikat nur für imapx: %s\n", argv[i]);
          failures++;
          continue;
        }

      settings = camel_service_ref_settings (service);
      if (user != NULL)
        camel_network_settings_set_user (CAMEL_NETWORK_SETTINGS (settings), user);
      if (certificate)
        {
          camel_network_settings_set_host (CAMEL_NETWORK_SETTINGS (settings), "expired.badssl.com");
          camel_network_settings_set_port (CAMEL_NETWORK_SETTINGS (settings), 443);
          camel_network_settings_set_security_method (CAMEL_NETWORK_SETTINGS (settings),
                                                      CAMEL_NETWORK_SECURITY_METHOD_SSL_ON_ALTERNATE_PORT);
        }

      /* Wie die App (src/main.c connect_service_thread). */
      if (camel_offline_store_set_online_sync (CAMEL_OFFLINE_STORE (service), TRUE, NULL, &local_error))
        {
          g_print ("verbunden: %s\n", argv[i]);
          camel_service_disconnect_sync (service, TRUE, NULL, NULL);
        }
      else
        {
          g_print ("Fehler: %s: %s (%s %d)\n", argv[i], local_error->message,
                   g_quark_to_string (local_error->domain), local_error->code);
          failures++;
        }
    }

  /* Ausstehende Signale (z. B. „changed“ der im Speicher geänderten ESource) zustellen,
   * solange Session und Stores noch leben. */
  while (g_main_context_iteration (NULL, FALSE))
    ;

  return failures == 0 ? 0 : 1;
}
