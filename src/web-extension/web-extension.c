/* web-extension.c – Web-Process-Extension des Lesebereichs (#354)
 *
 * Läuft im WebKit-Webprozess, nicht in der App. Sperrt jede Anfrage außer cid:, data: und
 * about: (Anzeige ohne Netzzugriffe, Dokument #10 „#354“). Muster wie Stamp
 * web-process-extension/extension.c:157-213 (send-request, Ausnahmen cid/data/about) und
 * Evolution web-extensions/e-web-extension.c:98-143 (send-request je Seite). #355: gesperrte
 * http-/https-Anfragen meldet die Erweiterung der App („revolution-remote-blocked“ wie Stamp
 * „image-load-blocked“, extension.c:181-182), einmal je Dokument. Freigabe: die App nennt vor dem
 * Laden die Adresse der freigegebenen Ladung („revolution-remote-allow“); nur solange die Seite
 * genau diese Adresse hat, gehen http und https durch. Jede andere Ladung (andere Mail, neues
 * Theme) hat eine neue Adresse und ist wieder gesperrt. Die App lädt eine Nachricht erst, wenn diese Erweiterung auf die
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

/* #355: je Seite die freigegebene Ladung und das zuletzt gemeldete Dokument (Adressen). */
#define ALLOWED_URI  "revolution-remote-allowed"
#define REPORTED_URI "revolution-remote-reported"

static gboolean
scheme_allowed (const char *uri)
{
  g_autofree char *scheme = g_uri_parse_scheme (uri);

  return scheme != NULL &&
         (g_ascii_strcasecmp (scheme, "cid") == 0 ||
          g_ascii_strcasecmp (scheme, "data") == 0 ||
          g_ascii_strcasecmp (scheme, "about") == 0);
}

static gboolean
scheme_remote (const char *uri)
{
  g_autofree char *scheme = g_uri_parse_scheme (uri);

  return scheme != NULL && (g_ascii_strcasecmp (scheme, "http") == 0 || g_ascii_strcasecmp (scheme, "https") == 0);
}

/* TRUE beendet die Anfrage (WebKitWebPage::send-request), es entsteht keine Verbindung. */
static gboolean
on_send_request (WebKitWebPage     *page,
                 WebKitURIRequest  *request,
                 WebKitURIResponse *redirected_response G_GNUC_UNUSED,
                 gpointer           user_data G_GNUC_UNUSED)
{
  const char *uri = webkit_uri_request_get_uri (request);
  const char *page_uri = webkit_web_page_get_uri (page);
  const char *allowed = g_object_get_data (G_OBJECT (page), ALLOWED_URI);

  if (uri != NULL && scheme_allowed (uri))
    return FALSE;

  if (uri != NULL && scheme_remote (uri) && allowed != NULL && g_strcmp0 (page_uri, allowed) == 0)
    {
      g_debug ("Webprozess: Anfrage freigegeben: %s", uri);
      return FALSE;
    }

  g_debug ("Webprozess: Anfrage gesperrt: %s", uri != NULL ? uri : "(ohne URI)");

  /* Hinweis nur für Inhalte, die eine Freigabe laden würde (http, https), einmal je Dokument. */
  if (uri != NULL && scheme_remote (uri) && page_uri != NULL &&
      g_strcmp0 (g_object_get_data (G_OBJECT (page), REPORTED_URI), page_uri) != 0)
    {
      g_object_set_data_full (G_OBJECT (page), REPORTED_URI, g_strdup (page_uri), g_free);
      webkit_web_page_send_message_to_view (page,
                                            webkit_user_message_new ("revolution-remote-blocked",
                                                                     g_variant_new_string (page_uri)),
                                            NULL, NULL, NULL);
    }

  return TRUE;
}

/* Bestätigung an die App, dass die Sperre auf dieser Seite aktiv ist. */
static gboolean
on_user_message_received (WebKitWebPage     *page,
                          WebKitUserMessage *message,
                          gpointer           user_data G_GNUC_UNUSED)
{
  const char *name = webkit_user_message_get_name (message);

  /* #355: Freigabe für genau die Ladung mit dieser Adresse; die App lädt erst nach der Antwort. */
  if (g_strcmp0 (name, "revolution-remote-allow") == 0)
    {
      GVariant *parameters = webkit_user_message_get_parameters (message);

      if (parameters == NULL || !g_variant_is_of_type (parameters, G_VARIANT_TYPE_STRING))
        return FALSE;

      g_object_set_data_full (G_OBJECT (page), ALLOWED_URI, g_variant_dup_string (parameters, NULL), g_free);
      g_debug ("Webprozess: Freigabe für %s", g_variant_get_string (parameters, NULL));
      /* Antwort mit derselben Adresse: die App lädt nur, wenn sie noch die aktuelle ist. */
      webkit_user_message_send_reply (message, webkit_user_message_new (name, parameters));
      return TRUE;
    }

  /* #357: Höhe des Inhalts für die Konversation (mail-reader.c fit_content), mit der Adresse der Ladung,
   * damit die App nur die aktuelle übernimmt. Gemessen über JavaScriptCore der Erweiterung; der Seite
   * selbst bleibt JavaScript verboten (enable-javascript aus, mail-reader.c create_settings): so laufen
   * nur diese Zeilen, keine Skripte der Mail (geprüft: Seite mit <script> ohne Wirkung, Messung geht).
   * Seitenrahmen wie Stamp (extension.c:34-37, webkit_web_page_get_main_frame; das nicht veraltete
   * WebKitScriptWorld::window-object-cleared kommt bei ausgeschaltetem JavaScript nicht). */
  if (g_strcmp0 (name, "revolution-height") == 0)
    {
      const char *page_uri = webkit_web_page_get_uri (page);
      g_autoptr (JSCContext) context = NULL;
      g_autoptr (JSCValue) value = NULL;
      WebKitFrame *frame;
      int height = 0;

      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      frame = webkit_web_page_get_main_frame (page);
      G_GNUC_END_IGNORE_DEPRECATIONS
      context = webkit_frame_get_js_context (frame);
      value = jsc_context_evaluate (context,
                                    "Math.ceil (Math.max (document.documentElement.getBoundingClientRect ().height,"
                                    " document.body ? document.body.scrollHeight : 0))", -1);
      if (value != NULL && jsc_value_is_number (value))
        height = jsc_value_to_int32 (value);
      webkit_user_message_send_reply (message, webkit_user_message_new (name, g_variant_new ("(si)",
                                                                                            page_uri != NULL ? page_uri : "",
                                                                                            height)));
      return TRUE;
    }

  if (g_strcmp0 (name, "revolution-guard") != 0)
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
