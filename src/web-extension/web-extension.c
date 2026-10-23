/* web-extension.c – Web-Process-Extension des Lesebereichs (#354)
 *
 * Läuft im WebKit-Webprozess, nicht in der App. Sperrt jede Anfrage außer cid:, data: und
 * about: (Anzeige ohne Netzzugriffe, Dokument #10 „#354“). Muster wie Stamp
 * web-process-extension/extension.c:157-213 (send-request, Ausnahmen cid/data/about) und
 * Evolution web-extensions/e-web-extension.c:98-143 (send-request je Seite); #355 ergänzt hier
 * die Freigabe mit Hinweis. Die App lädt eine Nachricht erst, wenn diese Erweiterung auf die
 * Nachricht „revolution-guard“ geantwortet hat (mail-reader.c, guard_page): fehlt sie, wird
 * nichts angezeigt statt ungeschützt geladen.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include <webkit/webkit-web-process-extension.h>

G_MODULE_EXPORT void webkit_web_process_extension_initialize (WebKitWebProcessExtension *extension);

static gboolean
scheme_allowed (const char *uri)
{
  g_autofree char *scheme = g_uri_parse_scheme (uri);

  return scheme != NULL &&
         (g_ascii_strcasecmp (scheme, "cid") == 0 ||
          g_ascii_strcasecmp (scheme, "data") == 0 ||
          g_ascii_strcasecmp (scheme, "about") == 0);
}

/* TRUE beendet die Anfrage (WebKitWebPage::send-request), es entsteht keine Verbindung. */
static gboolean
on_send_request (WebKitWebPage     *page G_GNUC_UNUSED,
                 WebKitURIRequest  *request,
                 WebKitURIResponse *redirected_response G_GNUC_UNUSED,
                 gpointer           user_data G_GNUC_UNUSED)
{
  const char *uri = webkit_uri_request_get_uri (request);

  if (uri != NULL && scheme_allowed (uri))
    return FALSE;

  g_debug ("Webprozess: Anfrage gesperrt: %s", uri != NULL ? uri : "(ohne URI)");
  return TRUE;
}

/* Bestätigung an die App, dass die Sperre auf dieser Seite aktiv ist. */
static gboolean
on_user_message_received (WebKitWebPage     *page G_GNUC_UNUSED,
                          WebKitUserMessage *message,
                          gpointer           user_data G_GNUC_UNUSED)
{
  if (g_strcmp0 (webkit_user_message_get_name (message), "revolution-guard") != 0)
    return FALSE;

  webkit_user_message_send_reply (message, webkit_user_message_new ("revolution-guard", NULL));
  return TRUE;
}

static void
on_page_created (WebKitWebProcessExtension *extension G_GNUC_UNUSED,
                 WebKitWebPage             *page,
                 gpointer                   user_data G_GNUC_UNUSED)
{
  g_signal_connect (page, "send-request", G_CALLBACK (on_send_request), NULL);
  g_signal_connect (page, "user-message-received", G_CALLBACK (on_user_message_received), NULL);
}

G_MODULE_EXPORT void
webkit_web_process_extension_initialize (WebKitWebProcessExtension *extension)
{
  g_signal_connect (extension, "page-created", G_CALLBACK (on_page_created), NULL);
}
