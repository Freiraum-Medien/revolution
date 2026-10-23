/* mail-reader.c – Lesebereich: Nachrichtenkopf und Inhalt in WebKitGTK 6.0 (#354)
 *
 * Die Auswahl in der Nachrichtenliste (mail-view.c) ruft revolution_mail_reader_show. Ein
 * Worker-GTask holt die Nachricht (camel_folder_get_message_sync, aus dem Cache oder vom
 * Server) und zerlegt sie (Architektur 1); im UI-Thread füllt das Ergebnis Kopf und WebView.
 * Ein neuer Aufruf bricht das Laden ab (GCancellable) und verwirft verspätete Ergebnisse
 * (Generation), wie die Nachrichtenliste (#352).
 *
 * Vorlagen: Stamp stamp-webview.c (WebView, cid:-Schema, decide-policy, Farbschema),
 * backend/stamp-mime-parser.c (Zerlegen der MIME-Teile), views/mail/message-list/
 * stamp-message-header.blp (Kopf). Verhalten: Evolution em-format/e-mail-formatter.c
 * (Zeichensatz), e-mail-formatter-text-plain.c (Text als HTML), mail/e-mail-display.c
 * (cid:-Teile aus der Nachricht).
 *
 * Netz: JavaScript aus, eigene flüchtige Netzwerksitzung, jede Anfrage außer cid:, data: und
 * about: sperrt die Web-Process-Extension (src/web-extension/web-extension.c). Begründung der
 * Wahl: Stamp (extension.c:157-186) und Evolution (e-web-extension.c:98-143) sperren bzw. leiten
 * je Anfrage im Webprozess über WebKitWebPage::send-request um; nur dort kennt man jede Anfrage
 * einzeln und kann #355 die Freigabe je Mail und den Hinweis „Entfernte Inhalte gesperrt“
 * ergänzen (Nachricht an die App wie Stamp „image-load-blocked“). Ein Inhaltsfilter
 * (WebKitUserContentFilter) sperrt zwar ohne Erweiterung, meldet aber nicht, was gesperrt wurde.
 * Fehlt die Erweiterung, zeigt die App nichts an (guard_page), statt ungeschützt zu laden.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>
#include <webkit/webkit.h>

#include "mail-account.h"
#include "mail-reader.h"
#include "mail-view.h"

typedef enum
{
  GUARD_NONE,
  GUARD_LOADING,
  GUARD_ASKING,
  GUARD_OK,
  GUARD_FAILED,
} GuardState;

struct _RevolutionMailReader
{
  AdwBin          parent_instance;
  AdwAvatar      *avatar;
  GtkLabel       *sender_label;
  GtkImage       *folder_icon;
  GtkLabel       *folder_label;
  GtkLabel       *date_label;
  GtkLabel       *subject_label;
  GtkLabel       *to_caption;
  GtkLabel       *to_label;
  GtkToggleButton *details_button;
  GtkLabel       *from_full;
  GtkLabel       *to_full;
  GtkLabel       *cc_full;
  GtkLabel       *date_full;
  GtkOverlay     *content_overlay;
  AdwSpinner     *spinner;
  WebKitWebView  *web_view;
  GCancellable   *cancellable;
  guint           generation;
  guint           load_timeout;
  struct _LoadMessage *pending;
  GHashTable     *parts;
  char           *html;
  char           *html_light;
  gboolean        own_dark;
  GuardState      guard;
  gboolean        dark;
  gboolean        ua_dark;
  gboolean        showing;
  GtkRoot        *root;
};

/* „shown“: Inhalt der Nachricht fertig geladen (WEBKIT_LOAD_FINISHED); mail-view.c startet damit den
 * Gelesen-Timer wie Evolution (e-mail-reader.c:3784-3801). */
static guint shown_signal;

G_DEFINE_FINAL_TYPE (RevolutionMailReader, revolution_mail_reader, ADW_TYPE_BIN)

/* Inline-Teil einer Nachricht für cid: (Bild mit Content-ID, multipart/related). */
typedef struct
{
  GBytes *data;
  char   *mime_type;
} ReaderPart;

static void
reader_part_free (ReaderPart *part)
{
  g_bytes_unref (part->data);
  g_free (part->mime_type);
  g_free (part);
}

/* ------------------------------------------------------------------ Worker: Nachricht zerlegen */

typedef struct _LoadMessage
{
  CamelSession *session;
  char         *account_uid;
  char         *full_name;
  char         *message_uid;
  guint         generation;
  gint64        started;
  /* Ergebnis (Worker) */
  char         *html;
  char         *html_light;
  gboolean      is_html;
  gboolean      own_dark;
  guint         scheme_queries;
  GHashTable   *parts;
  char         *to_short;
  char         *to_full;
  char         *cc_full;
  char         *from_full;
  gint64        date;
} LoadMessage;

static void
load_message_free (LoadMessage *load)
{
  g_object_unref (load->session);
  g_free (load->account_uid);
  g_free (load->full_name);
  g_free (load->message_uid);
  g_free (load->html);
  g_free (load->html_light);
  g_clear_pointer (&load->parts, g_hash_table_unref);
  g_free (load->to_short);
  g_free (load->to_full);
  g_free (load->cc_full);
  g_free (load->from_full);
  g_free (load);
}

/* Übernommen aus Evolution em-format/e-mail-formatter.c:1087-1162 (e_mail_formatter_format_text).
 * Lizenz: Dateikopf „GNU Lesser General Public License“ ohne Versionsangabe (e-mail-formatter.c:4-6),
 * Evolution COPYING:1-4 „LGPLv2 or (at your choice) LGPLv3“; LGPL erlaubt die Weitergabe unter
 * GPL-3.0-or-later. Inhalt eines Teils dekodieren (Transfer-Encoding) und, wenn to_utf8, aus dem
 * Zeichensatz des Teils nach UTF-8 wandeln. „iso-8859-*“ prüft vorher wie Evolution, ob es
 * eigentlich windows-125x ist (camel_mime_filter_windows). Ohne Zeichensatz bleiben die Bytes
 * unverändert (Evolution: Vorgabe „charset“ leer, org.gnome.evolution.mail.gschema.xml.in:385).
 * Ohne die UTF-16-Erkennung (emf_data_is_utf16) und ohne gewählten Zeichensatz der Ansicht. */
static GBytes *
decode_part (CamelMimePart *part,
             gboolean       to_utf8,
             GCancellable  *cancellable)
{
  CamelDataWrapper *content = camel_medium_get_content (CAMEL_MEDIUM (part));
  g_autoptr (GOutputStream) memory = g_memory_output_stream_new_resizable ();
  g_autoptr (GOutputStream) stream = NULL;
  g_autoptr (CamelMimeFilter) windows = NULL;
  CamelMimeFilter *filter = NULL;
  const char *charset = NULL;

  if (content == NULL)
    return g_bytes_new (NULL, 0);

  if (to_utf8)
    {
      CamelContentType *type = camel_data_wrapper_get_mime_type_field (CAMEL_DATA_WRAPPER (part));

      charset = type != NULL ? camel_content_type_param (type, "charset") : NULL;
      if (charset != NULL && g_ascii_strncasecmp (charset, "iso-8859-", 9) == 0)
        {
          g_autoptr (GOutputStream) null_stream = camel_null_output_stream_new ();
          g_autoptr (GOutputStream) filter_stream = NULL;

          windows = camel_mime_filter_windows_new (charset);
          filter_stream = camel_filter_output_stream_new (null_stream, windows);
          g_filter_output_stream_set_close_base_stream (G_FILTER_OUTPUT_STREAM (filter_stream), FALSE);
          camel_data_wrapper_decode_to_output_stream_sync (content, filter_stream, cancellable, NULL);
          g_output_stream_flush (filter_stream, cancellable, NULL);
          charset = camel_mime_filter_windows_real_charset (CAMEL_MIME_FILTER_WINDOWS (windows));
        }
      if (charset != NULL)
        filter = camel_mime_filter_charset_new (charset, "UTF-8");
    }

  if (filter != NULL)
    {
      stream = camel_filter_output_stream_new (memory, filter);
      g_filter_output_stream_set_close_base_stream (G_FILTER_OUTPUT_STREAM (stream), FALSE);
      g_object_unref (filter);
    }
  else
    stream = g_object_ref (memory);

  camel_data_wrapper_decode_to_output_stream_sync (content, stream, cancellable, NULL);
  g_output_stream_flush (stream, cancellable, NULL);
  g_output_stream_close (memory, NULL, NULL);

  return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (memory));
}

static char *
decode_text (CamelMimePart *part,
             GCancellable  *cancellable)
{
  g_autoptr (GBytes) bytes = decode_part (part, TRUE, cancellable);
  gsize size;
  const char *data = g_bytes_get_data (bytes, &size);

  return g_utf8_make_valid (data != NULL ? data : "", size);
}

typedef struct
{
  GString      *body;
  gboolean      html;
  GHashTable   *parts;
  GCancellable *cancellable;
} Collect;

static void collect_part (Collect *collect, CamelMimePart *part, guint depth);

/* Teil mit Content-ID für cid: merken (Evolution e-mail-display.c: Schema „cid“ liefert die
 * Teile der Nachricht, e_mail_display_… über EMailRequest). */
static void
store_part (Collect       *collect,
            CamelMimePart *part)
{
  const char *cid = camel_mime_part_get_content_id (part);
  ReaderPart *stored;

  if (cid == NULL || *cid == '\0' || g_hash_table_contains (collect->parts, cid))
    return;

  stored = g_new0 (ReaderPart, 1);
  stored->data = decode_part (part, FALSE, collect->cancellable);
  stored->mime_type = camel_content_type_simple (camel_mime_part_get_content_type (part));
  g_hash_table_insert (collect->parts, g_strdup (cid), stored);
}

/* Text als HTML wie Evolution: Flags aus e-mail-formatter.c:586-591 und
 * e-mail-formatter-text-plain.c:85-98, Zitatfarbe #737373 (Vorgabe „citation-color“,
 * org.gnome.evolution.mail.gschema.xml.in:375-376). Schrift: BASE_CSS. */
static void
append_plain (Collect       *collect,
              CamelMimePart *part)
{
  CamelContentType *type = camel_mime_part_get_content_type (part);
  const char *format = camel_content_type_param (type, "format");
  guint32 flags = CAMEL_MIME_FILTER_TOHTML_CONVERT_URLS | CAMEL_MIME_FILTER_TOHTML_CONVERT_NL |
                  CAMEL_MIME_FILTER_TOHTML_CONVERT_SPACES | CAMEL_MIME_FILTER_TOHTML_CONVERT_ADDRESSES |
                  CAMEL_MIME_FILTER_TOHTML_MARK_CITATION | CAMEL_MIME_FILTER_TOHTML_PRESERVE_TABS;
  g_autofree char *text = decode_text (part, collect->cancellable);
  g_autofree char *html = NULL;

  if (format != NULL && g_ascii_strcasecmp (format, "flowed") == 0)
    flags |= CAMEL_MIME_FILTER_TOHTML_FORMAT_FLOWED;

  html = camel_text_to_html (text, flags, 0x737373);
  g_string_append_printf (collect->body, "<div class=\"revolution-plain\">%s</div>", html);
}

/* multipart/alternative: die beste darstellbare Fassung wie Evolution (HTML vor Text): von
 * hinten (RFC 2046 5.1.4: zuletzt die bevorzugte) die erste HTML- oder Multipart-Fassung,
 * sonst die letzte Text-Fassung. */
static CamelMimePart *
best_alternative (CamelMultipart *multipart)
{
  guint n = camel_multipart_get_number (multipart);

  for (guint i = n; i-- > 0;)
    {
      CamelMimePart *child = camel_multipart_get_part (multipart, i);
      CamelContentType *type = camel_mime_part_get_content_type (child);

      if (camel_content_type_is (type, "text", "html") || camel_content_type_is (type, "multipart", "*"))
        return child;
    }

  for (guint i = n; i-- > 0;)
    {
      CamelMimePart *child = camel_multipart_get_part (multipart, i);

      if (camel_content_type_is (camel_mime_part_get_content_type (child), "text", "plain"))
        return child;
    }

  return n > 0 ? camel_multipart_get_part (multipart, 0) : NULL;
}

/* multipart/related: Wurzel laut Parameter „start“, sonst der erste Teil (RFC 2387 3.2). */
static guint
related_root (CamelMultipart   *multipart,
              CamelContentType *type)
{
  const char *start = camel_content_type_param (type, "start");
  g_autofree char *wanted = NULL;

  if (start == NULL)
    return 0;

  wanted = g_strdup (start[0] == '<' ? start + 1 : start);
  if (g_str_has_suffix (wanted, ">"))
    wanted[strlen (wanted) - 1] = '\0';

  for (guint i = 0; i < camel_multipart_get_number (multipart); i++)
    if (g_strcmp0 (camel_mime_part_get_content_id (camel_multipart_get_part (multipart, i)), wanted) == 0)
      return i;

  return 0;
}

/* Zerlegen wie Stamp stamp-mime-parser.c:850-893, 959-1003, 1192-1234 (Tabelle Typ → Behandlung),
 * ohne Signatur/Verschlüsselung, Kalender und Anhänge (Anhänge: #356). Darstellbare Texte
 * kommen der Reihe nach in den Körper, HTML unverändert, Text über append_plain. */
static void
collect_part (Collect       *collect,
              CamelMimePart *part,
              guint          depth)
{
  CamelDataWrapper *content = camel_medium_get_content (CAMEL_MEDIUM (part));
  CamelContentType *type = camel_mime_part_get_content_type (part);

  if (content == NULL || type == NULL || depth > 32 || g_cancellable_is_cancelled (collect->cancellable))
    return;

  if (CAMEL_IS_MULTIPART (content))
    {
      CamelMultipart *multipart = CAMEL_MULTIPART (content);

      if (camel_content_type_is (type, "multipart", "alternative"))
        {
          CamelMimePart *best = best_alternative (multipart);

          if (best != NULL)
            collect_part (collect, best, depth + 1);
        }
      else if (camel_content_type_is (type, "multipart", "related"))
        {
          guint root = related_root (multipart, type);

          for (guint i = 0; i < camel_multipart_get_number (multipart); i++)
            {
              CamelMimePart *child = camel_multipart_get_part (multipart, i);

              if (i == root)
                collect_part (collect, child, depth + 1);
              else if (!CAMEL_IS_MULTIPART (camel_medium_get_content (CAMEL_MEDIUM (child))))
                store_part (collect, child);
            }
        }
      else
        {
          for (guint i = 0; i < camel_multipart_get_number (multipart); i++)
            collect_part (collect, camel_multipart_get_part (multipart, i), depth + 1);
        }
      return;
    }

  store_part (collect, part);

  /* Angehängte Nachrichten und Anhänge zeigt #356. */
  if (CAMEL_IS_MIME_MESSAGE (content) || g_strcmp0 (camel_mime_part_get_disposition (part), "attachment") == 0)
    return;

  if (camel_content_type_is (type, "text", "html"))
    {
      g_autofree char *html = decode_text (part, collect->cancellable);

      g_string_append (collect->body, html);
      collect->html = TRUE;
    }
  else if (camel_content_type_is (type, "text", "plain"))
    append_plain (collect, part);
}

/* Anzeigenamen der Empfänger für die Zeile „An:“, sonst die Adresse. Ein Name in spitzen
 * Klammern gilt als kein Name (microsoft365, camel-m365-folder.c:787-800, wie mail-message.c). */
static char *
names_for (CamelInternetAddress *address)
{
  GString *names = g_string_new (NULL);

  for (int i = 0; address != NULL && i < camel_address_length (CAMEL_ADDRESS (address)); i++)
    {
      const char *name = NULL, *email = NULL;
      g_autofree char *bare = NULL;

      if (!camel_internet_address_get (address, i, &name, &email))
        continue;

      bare = g_strstrip (g_strdup (name != NULL ? name : ""));
      if (*bare == '<' || (email != NULL && g_ascii_strcasecmp (bare, email) == 0))
        bare[0] = '\0';

      if (names->len > 0)
        g_string_append (names, ", ");
      g_string_append (names, *bare != '\0' ? bare : (email != NULL ? email : ""));
    }

  return g_string_free (names, FALSE);
}

static char *
format_address (CamelInternetAddress *address)
{
  char *text = address != NULL ? camel_address_format (CAMEL_ADDRESS (address)) : NULL;

  return text != NULL ? text : g_strdup ("");
}

/* Krücke: WebKitGTK 2.52.6 setzt prefers-color-scheme fensterweit nach GtkSettings
 * gtk-application-prefer-dark-theme, keine API je WebView (Beleg: strings libwebkitgtk-6.0.so.4 kennt
 * nur „notify::gtk-application-prefer-dark-theme“ und „notify::gtk-theme-name“; Testprogramm mit
 * prefer-dark: matchMedia('(prefers-color-scheme: dark)') = true, auch mit <meta name="color-scheme"
 * content="light only">). In den Themes 2/3 (App dunkel) griffe daher in jeder Mail ihr Dunkel-CSS.
 * Abhilfe (PM #354, aus Dokument #5 „HTML behält eigenen Hintergrund“): eine zweite Fassung, in der
 * „prefers-color-scheme: dark“ und „light“ getauscht sind, nur in <style>-Elementen und in den Tags
 * link, style, source, img, video, audio, picture (media-Attribut); Fließtext bleibt unverändert.
 * Unter dunkler Oberfläche greifen so die hellen Regeln. */
static GRegex *
scheme_regex (guint which)
{
  static GRegex *regex[3];
  static const char *patterns[3] = {
    /* Bereiche: <style>…</style> und Tags mit media-Attribut (nur innerhalb des Tags) */
    "<style\\b[^>]*>.*?</style\\s*>|<(?:link|source|img|video|audio|picture)\\b[^>]*>",
    /* Medienabfrage selbst */
    "(prefers-color-scheme\\s*:\\s*)(dark|light)\\b",
    /* Die Mail erklärt eine Dunkel-Fassung: color-scheme bzw. supported-color-schemes mit „dark“ als
     * <meta> oder CSS-Eigenschaft (CSS Color Adjustment 1 „color-scheme“; ohne Angabe gilt eine
     * Seite als nur hell). Nicht „prefers-color-scheme“ (Lookbehind). */
    "<meta\\b[^>]*\\b(?:supported-)?color-schemes?\\b[^>]*\\bdark\\b[^>]*>"
    "|(?<![-\\w])(?:supported-)?color-schemes?\\s*:[^;}<\"]*\\bdark\\b",
  };
  static gsize done;

  if (g_once_init_enter (&done))
    {
      for (guint i = 0; i < G_N_ELEMENTS (regex); i++)
        regex[i] = g_regex_new (patterns[i], G_REGEX_CASELESS | G_REGEX_DOTALL | G_REGEX_OPTIMIZE, 0, NULL);
      g_once_init_leave (&done, 1);
    }

  return regex[which];
}

typedef struct
{
  guint dark;
  guint total;
} SchemeCount;

static gboolean
swap_scheme (const GMatchInfo *info,
             GString          *result,
             gpointer          user_data)
{
  SchemeCount *count = user_data;
  g_autofree char *prefix = g_match_info_fetch (info, 1);
  g_autofree char *value = g_match_info_fetch (info, 2);
  gboolean dark = g_ascii_strcasecmp (value, "dark") == 0;

  count->total++;
  count->dark += dark;
  g_string_append (result, prefix);
  g_string_append (result, dark ? "light" : "dark");

  return FALSE;
}

static gboolean
swap_region (const GMatchInfo *info,
             GString          *result,
             gpointer          user_data)
{
  g_autofree char *region = g_match_info_fetch (info, 0);
  g_autofree char *swapped = g_regex_replace_eval (scheme_regex (1), region, -1, 0, 0, swap_scheme, user_data, NULL);

  g_string_append (result, swapped != NULL ? swapped : region);

  return FALSE;
}

/* Im Worker: helle Fassung (NULL ohne Medienabfrage) und ob die Mail eine eigene Dunkel-Fassung hat
 * (Medienabfrage „dark“ und color-scheme mit „dark“). */
static void
prepare_schemes (LoadMessage *load)
{
  SchemeCount count = { 0 };
  g_autofree char *swapped = g_regex_replace_eval (scheme_regex (0), load->html, -1, 0, 0, swap_region, &count, NULL);

  load->scheme_queries = count.total;
  if (count.total > 0 && swapped != NULL)
    load->html_light = g_steal_pointer (&swapped);
  load->own_dark = count.dark > 0 && g_regex_match (scheme_regex (2), load->html, 0, NULL);
}

static void
load_message_thread (GTask        *task,
                     gpointer      source_object G_GNUC_UNUSED,
                     gpointer      task_data,
                     GCancellable *cancellable)
{
  LoadMessage *load = task_data;
  g_autoptr (CamelService) service = camel_session_ref_service (load->session, load->account_uid);
  g_autoptr (CamelFolder) folder = NULL;
  g_autoptr (CamelMimeMessage) message = NULL;
  Collect collect = { 0 };
  GError *error = NULL;
  int offset = 0;

  if (!CAMEL_IS_STORE (service))
    {
      g_task_return_new_error (task, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE,
                               _("Konto %s nicht verfügbar"), load->account_uid);
      return;
    }

  /* Ordner immer über camel_store_get_folder_sync (README „Camel-Ordner“). */
  folder = camel_store_get_folder_sync (CAMEL_STORE (service), load->full_name, 0, cancellable, &error);
  if (folder != NULL)
    message = camel_folder_get_message_sync (folder, load->message_uid, cancellable, &error);
  if (message == NULL)
    {
      g_task_return_error (task, error);
      return;
    }

  load->from_full = format_address (camel_mime_message_get_from (message));
  load->to_short = names_for (camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_TO));
  load->to_full = format_address (camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_TO));
  load->cc_full = format_address (camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_CC));
  load->date = camel_mime_message_get_date (message, &offset);
  if (load->date == CAMEL_MESSAGE_DATE_CURRENT || load->date <= 0)
    load->date = camel_mime_message_get_date_received (message, &offset);

  collect.body = g_string_new (NULL);
  collect.parts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) reader_part_free);
  collect.cancellable = cancellable;
  collect_part (&collect, CAMEL_MIME_PART (message), 0);

  if (!collect.html)
    {
      /* Nur Text: eigenes Gerüst; HTML-Mails bleiben unverändert (Quirks-Modus erhalten). */
      g_string_prepend (collect.body, "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body>");
      g_string_append (collect.body, "</body></html>");
    }

  load->is_html = collect.html;
  load->html = g_string_free (collect.body, FALSE);
  load->parts = collect.parts;
  if (load->is_html)
    prepare_schemes (load);

  if (g_task_return_error_if_cancelled (task))
    return;
  g_task_return_boolean (task, TRUE);
}

/* ------------------------------------------------------------------ WebView */

/* Text-Mails in der Festbreitenschrift wie Evolution (e-web-view.c:3440-3500: body in der
 * Oberflächenschrift, .pre in der Festbreitenschrift). Rand: ersetzt nur den Standardrand des
 * Browsers (8px, Stufe „user“ ohne !important); setzt die Mail selbst einen Rand, gilt ihrer.
 * So klebt einfacher Text nicht an der abgerundeten Karte. Hell (Themes 1/2): Inhalt bleibt hell. */
static const char BASE_CSS[] =
  "body { margin: 14px 18px; } .revolution-plain { font-family: monospace; overflow-wrap: anywhere; }";
static const char LIGHT_CSS[] = ":root { color-scheme: light only !important; }";
/* Dunkel (Theme 3) für Mails ohne eigene Dunkel-Fassung (Design/Prototyp/theme-dunkel-mail.png:
 * Farben der Mail bleiben erkennbar, Flächen dunkel): helle Fassung der Seite, dann invertiert und um 180° im Farbton gedreht, sodass Farbtöne
 * erhalten bleiben; Bilder zurückgedreht. Seitenhintergrund durchsichtig, damit die dunkle Karte
 * durchscheint. Abweichung von Stamp stamp-webview.c:174-184 (alle Farben auf „inherit“): dort
 * verschwanden im ersten Lauf Akzentfarben, Zitatfarbe und Hinterlegungen. */
static const char DARK_CSS[] =
  ":root { color-scheme: light only !important; filter: invert(1) hue-rotate(180deg) !important; }"
  " html, body { background-color: transparent !important; }"
  " img, picture, video, canvas, svg image { filter: invert(1) hue-rotate(180deg) !important; }";

static void
apply_styles (RevolutionMailReader *self)
{
  WebKitUserContentManager *manager = webkit_web_view_get_user_content_manager (self->web_view);
  /* Theme 3 mit eigener Dunkel-Fassung der Mail: diese unverändert (eigener Hintergrund). */
  const char *sheets[] = { BASE_CSS, self->dark ? (self->own_dark ? NULL : DARK_CSS) : LIGHT_CSS };

  webkit_user_content_manager_remove_all_style_sheets (manager);
  for (gsize i = 0; i < G_N_ELEMENTS (sheets) && sheets[i] != NULL; i++)
    {
      g_autoptr (WebKitUserStyleSheet) sheet =
        webkit_user_style_sheet_new (sheets[i], WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                     WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);

      webkit_user_content_manager_add_style_sheet (manager, sheet);
    }
}

static void show_html (RevolutionMailReader *self);

/* Themes (#340, main.c apply_content_class): nur „Dunkel“ setzt .content-dark am Fenster. Ein
 * Wechsel lädt die angezeigte Mail neu (andere Fassung, andere Stile). */
static void
on_root_classes (RevolutionMailReader *self)
{
  gboolean dark = self->root != NULL && gtk_widget_has_css_class (GTK_WIDGET (self->root), "content-dark");

  if (dark == self->dark)
    return;

  self->dark = dark;
  apply_styles (self);
  show_html (self);
}

/* Theme-Wechsel hell ↔ dunkel (AdwStyleManager:dark, main.c change_theme): Fassung neu wählen. */
static void
on_ua_dark_changed (RevolutionMailReader *self)
{
  gboolean dark = adw_style_manager_get_dark (adw_style_manager_get_default ());

  if (dark == self->ua_dark)
    return;

  self->ua_dark = dark;
  show_html (self);
}

static void
show_error (RevolutionMailReader *self,
            const char           *title,
            const char           *detail)
{
  GtkWidget *view = gtk_widget_get_ancestor (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_VIEW);

  g_warning ("%s: %s", title, detail);
  if (view != NULL)
    revolution_mail_view_show_error (REVOLUTION_MAIL_VIEW (view), title, detail, FALSE);
}

static void guard_page (RevolutionMailReader *self);

/* Fassung je Theme (PM #354): Theme 1 (Oberfläche hell) die Mail unverändert; Themes 2/3 die helle
 * Fassung (prepare_schemes), außer Theme 3 bei eigener Dunkel-Fassung der Mail. */
static void
show_html (RevolutionMailReader *self)
{
  const char *html = self->html;

  if (html == NULL)
    return;

  if (self->ua_dark && self->html_light != NULL && !(self->dark && self->own_dark))
    html = self->html_light;

  /* Nach fehlgeschlagener Bestätigung neu versuchen, mit neuem Webprozess. */
  if (self->guard == GUARD_FAILED)
    {
      self->guard = GUARD_NONE;
      webkit_web_view_terminate_web_process (self->web_view);
    }

  if (self->guard == GUARD_OK)
    {
      /* Sichtbar erst mit dem neuen Dokument (on_load_changed, WEBKIT_LOAD_COMMITTED). */
      self->showing = TRUE;
      webkit_web_view_load_html (self->web_view, html, NULL);
    }
  else
    guard_page (self);
}

static void
on_guard_reply (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr (RevolutionMailReader) self = user_data;
  g_autoptr (WebKitUserMessage) reply = NULL;
  g_autoptr (GError) error = NULL;

  reply = webkit_web_view_send_message_to_page_finish (WEBKIT_WEB_VIEW (source), result, &error);
  if (self->guard != GUARD_ASKING)
    return;

  if (reply == NULL)
    {
      g_autofree char *detail = g_strdup_printf (_("Schutz vor Netzzugriffen nicht aktiv: %s"), error->message);

      self->guard = GUARD_FAILED;
      show_error (self, _("Nachricht nicht angezeigt"), detail);
      return;
    }

  g_debug ("Nachricht: Sperre der Netzzugriffe aktiv");
  self->guard = GUARD_OK;
  show_html (self);
}

/* Erst eine leere Seite laden und von der Web-Process-Extension bestätigen lassen, dass ihre
 * Sperre (send-request) auf dieser Seite hängt; erst danach kommt Mail-Inhalt in die WebView.
 * Ohne Erweiterung antwortet WebKit mit Fehler (WEBKIT_USER_MESSAGE_UNHANDLED_MESSAGE). */
static void
guard_page (RevolutionMailReader *self)
{
  if (self->guard != GUARD_NONE)
    return;

  self->guard = GUARD_LOADING;
  webkit_web_view_load_html (self->web_view, "<!DOCTYPE html><html></html>", NULL);
}

static void
on_load_changed (RevolutionMailReader *self,
                 WebKitLoadEvent       event)
{
  if (event == WEBKIT_LOAD_COMMITTED && self->showing)
    gtk_widget_set_visible (GTK_WIDGET (self->web_view), TRUE);

  if (event == WEBKIT_LOAD_FINISHED && self->showing)
    {
      self->showing = FALSE;
      g_signal_emit (self, shown_signal, 0);
      return;
    }

  if (event != WEBKIT_LOAD_FINISHED || self->guard != GUARD_LOADING)
    return;

  self->guard = GUARD_ASKING;
  webkit_web_view_send_message_to_page (self->web_view, webkit_user_message_new ("revolution-guard", NULL),
                                        NULL, on_guard_reply, g_object_ref (self));
}

/* Neuer Webprozess: Erweiterung erneut bestätigen lassen, Inhalt neu laden. Beim Beenden der
 * App (on_application_shutdown, WEBKIT_WEB_PROCESS_TERMINATED_BY_API) nichts tun. */
static void
on_web_process_terminated (RevolutionMailReader             *self,
                           WebKitWebProcessTerminationReason reason)
{
  if (reason == WEBKIT_WEB_PROCESS_TERMINATED_BY_API)
    return;

  g_warning ("Nachricht: Webprozess beendet (%d)", reason);
  self->guard = GUARD_NONE;
  show_html (self);
}

static gboolean
scheme_is (const char *uri,
           const char *scheme)
{
  return g_str_has_prefix (uri, scheme) && uri[strlen (scheme)] == ':';
}

static gboolean
uri_internal (const char *uri)
{
  return scheme_is (uri, "about") || scheme_is (uri, "cid") || scheme_is (uri, "data");
}

/* Links öffnet der Standardbrowser (GtkUriLauncher), die WebView navigiert nie weg; wie Stamp
 * stamp-webview.c:232-265. Eigene Inhalte (about:, cid:, data:) dürfen laden, alles andere nicht. */
static gboolean
on_decide_policy (RevolutionMailReader    *self,
                  WebKitPolicyDecision    *decision,
                  WebKitPolicyDecisionType type)
{
  WebKitNavigationAction *action;
  const char *uri;

  if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE)
    {
      WebKitURIRequest *request = webkit_response_policy_decision_get_request (WEBKIT_RESPONSE_POLICY_DECISION (decision));

      if (uri_internal (webkit_uri_request_get_uri (request)))
        webkit_policy_decision_use (decision);
      else
        webkit_policy_decision_ignore (decision);
      return TRUE;
    }

  action = webkit_navigation_policy_decision_get_navigation_action (WEBKIT_NAVIGATION_POLICY_DECISION (decision));
  uri = webkit_uri_request_get_uri (webkit_navigation_action_get_request (action));

  if (webkit_navigation_action_get_navigation_type (action) == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED &&
      !uri_internal (uri))
    {
      webkit_policy_decision_ignore (decision);
      if (!scheme_is (uri, "javascript") && !scheme_is (uri, "file"))
        {
          g_autoptr (GtkUriLauncher) launcher = gtk_uri_launcher_new (uri);
          GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));

          g_debug ("Nachricht: Link im Browser öffnen");
          gtk_uri_launcher_launch (launcher, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL, NULL, NULL, NULL);
        }
      return TRUE;
    }

  if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && uri_internal (uri))
    webkit_policy_decision_use (decision);
  else
    webkit_policy_decision_ignore (decision);

  return TRUE;
}

/* Kontextmenü nur mit Kopieren und Alles auswählen: „Link herunterladen“, „Bild in neuem
 * Fenster“ usw. liefen am Webprozess (und seiner Sperre) vorbei. */
static gboolean
on_context_menu (RevolutionMailReader *self G_GNUC_UNUSED,
                 WebKitContextMenu    *menu)
{
  GList *items = g_list_copy (webkit_context_menu_get_items (menu));

  for (GList *l = items; l != NULL; l = l->next)
    {
      switch (webkit_context_menu_item_get_stock_action (l->data))
        {
        case WEBKIT_CONTEXT_MENU_ACTION_COPY:
        case WEBKIT_CONTEXT_MENU_ACTION_COPY_LINK_TO_CLIPBOARD:
        case WEBKIT_CONTEXT_MENU_ACTION_COPY_IMAGE_TO_CLIPBOARD:
        case WEBKIT_CONTEXT_MENU_ACTION_SELECT_ALL:
          break;
        default:
          webkit_context_menu_remove (menu, l->data);
        }
    }
  g_list_free (items);

  return webkit_context_menu_get_n_items (menu) == 0;
}

/* cid: aus den Teilen der angezeigten Nachricht (Stamp stamp-webview.c:88-107), nur aus dem
 * Speicher; ohne Treffer ein Fehler. */
static void
on_cid_request (WebKitURISchemeRequest *request,
                gpointer                user_data G_GNUC_UNUSED)
{
  RevolutionMailReader *self = g_object_get_data (G_OBJECT (webkit_uri_scheme_request_get_web_view (request)),
                                                  "revolution-reader");
  g_autofree char *cid = g_uri_unescape_string (webkit_uri_scheme_request_get_path (request), NULL);
  ReaderPart *part = NULL;

  if (self != NULL && self->parts != NULL && cid != NULL)
    part = g_hash_table_lookup (self->parts, cid);

  if (part == NULL)
    {
      g_autoptr (GError) error = g_error_new (G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "cid:%s", cid != NULL ? cid : "");

      webkit_uri_scheme_request_finish_error (request, error);
      return;
    }

  {
    g_autoptr (GInputStream) stream = g_memory_input_stream_new_from_bytes (part->data);

    webkit_uri_scheme_request_finish (request, stream, g_bytes_get_size (part->data), part->mime_type);
  }
}

/* Erweiterung aus dem Build-Ordner, wenn die App von dort läuft (README „Build und Test“),
 * sonst aus dem Installationsort. */
static const char *
extensions_directory (void)
{
  g_autofree char *exe = g_file_read_link ("/proc/self/exe", NULL);
  g_autofree char *dir = exe != NULL ? g_path_get_dirname (exe) : NULL;

  return g_strcmp0 (dir, BUILD_SRC_DIR) == 0 ? BUILD_SRC_DIR "/web-extension" : WEB_EXTENSIONS_DIR;
}

static void
on_initialize_extensions (WebKitWebContext *context,
                          gpointer          user_data)
{
  g_debug ("Nachricht: Web-Process-Extension aus %s", (const char *) user_data);
  webkit_web_context_set_web_process_extensions_directory (context, user_data);
}

static gpointer
create_web_context (gpointer data G_GNUC_UNUSED)
{
  WebKitWebContext *context = webkit_web_context_new ();
  const char *extensions = extensions_directory ();

  /* Der Webprozess läuft in der Sandbox (bubblewrap) und sieht den Ordner sonst nicht: ohne diese
   * Zeile antwortete er im Build-Ordner auf „revolution-guard“ mit „nicht verarbeitet“. */
  if (g_file_test (extensions, G_FILE_TEST_IS_DIR))
    webkit_web_context_add_path_to_sandbox (context, extensions, TRUE);
  g_signal_connect (context, "initialize-web-process-extensions", G_CALLBACK (on_initialize_extensions),
                    (gpointer) extensions);
  webkit_web_context_register_uri_scheme (context, "cid", on_cid_request, NULL, NULL);

  return context;
}

/* Einstellungen: JavaScript aus, keine Plugins/Medien/Speicher; Schrift aus GTK wie Evolution
 * (e-web-view.c:3440-3476, Oberflächenschrift aus „font-name“). */
static const char * const PREFETCH_FEATURES[] = {
  "LinkPreconnect", "LinkPreconnectEarlyHints", "LinkDNSPrefetch", "LinkPrefetch", "LinkPreload",
  "SpeculationRulesPrefetch", NULL
};

static gpointer
create_settings (gpointer data G_GNUC_UNUSED)
{
  g_autoptr (WebKitFeatureList) features = webkit_settings_get_all_features ();
  g_autofree char *font_name = NULL;
  PangoFontDescription *font;
  WebKitSettings *settings;
  guint32 size = 15;

  g_object_get (gtk_settings_get_default (), "gtk-font-name", &font_name, NULL);
  font = pango_font_description_from_string (font_name != NULL ? font_name : "Sans 11");
  if (pango_font_description_get_size (font) > 0)
    size = webkit_settings_font_size_to_pixels ((guint32) (pango_font_description_get_size (font) / PANGO_SCALE));

  settings = webkit_settings_new_with_settings ("enable-javascript", FALSE,
                                                "enable-javascript-markup", FALSE,
                                                "auto-load-images", TRUE,
                                                "enable-media", FALSE,
                                                "enable-webaudio", FALSE,
                                                "enable-webgl", FALSE,
                                                "enable-webrtc", FALSE,
                                                "enable-mediasource", FALSE,
                                                "enable-encrypted-media", FALSE,
                                                "enable-html5-database", FALSE,
                                                "enable-html5-local-storage", FALSE,
                                                "enable-page-cache", FALSE,
                                                "enable-fullscreen", FALSE,
                                                "allow-modal-dialogs", FALSE,
                                                "enable-back-forward-navigation-gestures", FALSE,
                                                "default-font-family", pango_font_description_get_family (font),
                                                "default-font-size", size,
                                                "monospace-font-family", "monospace",
                                                "default-monospace-font-size", size,
                                                NULL);
  pango_font_description_free (font);

  /* Hyperlink-Auditing (<a ping>): „enable-hyperlink-auditing“ ist in WebKitGTK 2.52.6 ohne Wirkung
   * (Laufzeit: „webkit_settings_set_enable_hyperlink_auditing is deprecated and does nothing“, Wert
   * bleibt TRUE), daher nicht gesetzt. Die ping-Anfrage läuft über send-request und wird gesperrt
   * (Beleg build/m4-354/m4-354-ping.log, -ping-strace.txt: 0 connect zum ping-Host). */
  /* Vorab-Verbindungen aus <link rel=preconnect|dns-prefetch|preload|prefetch> und Speculation
   * Rules laufen nicht über WebKitWebPage::send-request: im ersten Lauf unter strace baute der
   * Netzwerkprozess für <link rel=preconnect> eine TCP-Verbindung zum Bildserver auf. Daher aus. */
  for (gsize i = 0; i < webkit_feature_list_get_length (features); i++)
    {
      WebKitFeature *feature = webkit_feature_list_get (features, i);

      if (g_strv_contains (PREFETCH_FEATURES, webkit_feature_get_identifier (feature)))
        webkit_settings_set_feature_enabled (settings, feature, FALSE);
    }

  return settings;
}

static WebKitNetworkSession *
network_session (void)
{
  static WebKitNetworkSession *session;

  /* Flüchtig: keine Cookies, kein Cache auf der Platte (Architektur 4). */
  if (session == NULL)
    session = webkit_network_session_new_ephemeral ();

  return session;
}

static WebKitWebView *
create_web_view (RevolutionMailReader *self)
{
  static GOnce context_once = G_ONCE_INIT, settings_once = G_ONCE_INIT;
  g_autoptr (WebKitUserContentManager) manager = webkit_user_content_manager_new ();
  WebKitWebView *web_view;
  GdkRGBA transparent = { 0, 0, 0, 0 };

  web_view = g_object_new (WEBKIT_TYPE_WEB_VIEW,
                           "web-context", g_once (&context_once, create_web_context, NULL),
                           "network-session", network_session (),
                           "settings", g_once (&settings_once, create_settings, NULL),
                           "user-content-manager", manager,
                           NULL);
  /* Durchsichtig: die Karte (.html-mail) gibt den Hintergrund je Theme. */
  webkit_web_view_set_background_color (web_view, &transparent);
  g_object_set_data (G_OBJECT (web_view), "revolution-reader", self);
  gtk_accessible_update_property (GTK_ACCESSIBLE (web_view), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Nachrichteninhalt"), -1);

  g_signal_connect_object (web_view, "decide-policy", G_CALLBACK (on_decide_policy), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (web_view, "context-menu", G_CALLBACK (on_context_menu), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (web_view, "load-changed", G_CALLBACK (on_load_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (web_view, "web-process-terminated", G_CALLBACK (on_web_process_terminated), self,
                           G_CONNECT_SWAPPED);

  return web_view;
}

/* ------------------------------------------------------------------ Kopf */

/* Datum im Kopf wie der Prototyp („Heute, 07:30“, Design/Prototyp/mail-hell.png). */
static char *
header_date (GDateTime *local)
{
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autoptr (GDate) day = g_date_new_dmy (g_date_time_get_day_of_month (local), g_date_time_get_month (local),
                                          g_date_time_get_year (local));
  g_autoptr (GDate) today = g_date_new_dmy (g_date_time_get_day_of_month (now), g_date_time_get_month (now),
                                            g_date_time_get_year (now));
  int days = g_date_days_between (day, today);
  g_autofree char *time = g_date_time_format (local, "%H:%M");

  if (days == 0)
    /* Translators: Datum im Nachrichtenkopf, %s ist die Uhrzeit. */
    return g_strdup_printf (_("Heute, %s"), time);
  if (days == 1)
    /* Translators: Datum im Nachrichtenkopf, %s ist die Uhrzeit. */
    return g_strdup_printf (_("Gestern, %s"), time);

  /* Translators: Datum im Nachrichtenkopf (g_date_time_format), z. B. „17.10.2026, 14:05“. */
  return g_date_time_format (local, _("%d.%m.%Y, %H:%M"));
}

static void
set_header (RevolutionMailReader *self,
            LoadMessage          *load)
{
  g_autoptr (GDateTime) local = load->date > 0 ? g_date_time_new_from_unix_local (load->date) : NULL;
  g_autofree char *date = local != NULL ? header_date (local) : g_strdup ("");
  /* Translators: vollständiges Datum in den Details (g_date_time_format). */
  g_autofree char *full = local != NULL ? g_date_time_format (local, _("%A, %-d. %B %Y, %H:%M:%S")) : g_strdup ("");

  gtk_label_set_label (self->date_label, date);
  gtk_label_set_label (self->to_label, load->to_short);
  gtk_widget_set_visible (GTK_WIDGET (self->to_caption), *load->to_short != '\0');
  gtk_label_set_label (self->from_full, load->from_full);
  gtk_label_set_label (self->to_full, load->to_full);
  gtk_label_set_label (self->cc_full, load->cc_full);
  gtk_widget_set_visible (GTK_WIDGET (self->cc_full), *load->cc_full != '\0');
  gtk_label_set_label (self->date_full, full);
}

static void
on_message_loaded (GObject      *source_object,
                   GAsyncResult *result,
                   gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionMailReader *self = REVOLUTION_MAIL_READER (source_object);
  LoadMessage *load = g_task_get_task_data (G_TASK (result));
  g_autoptr (GError) error = NULL;

  if (!g_task_propagate_boolean (G_TASK (result), &error) &&
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  if (load->generation != self->generation)
    return;

  gtk_widget_set_visible (GTK_WIDGET (self->spinner), FALSE);

  if (error != NULL)
    {
      g_autofree char *detail = g_strdup_printf ("%s: %s", gtk_label_get_label (self->subject_label), error->message);

      show_error (self, _("Nachricht nicht geladen"), detail);
      return;
    }

  g_debug ("Nachricht: %s %s %s geladen nach %.0f ms (%s, %zu Bytes, %u Teile mit Content-ID, "
           "%u Abfragen prefers-color-scheme, eigene Dunkel-Fassung %s)",
           load->account_uid, load->full_name, load->message_uid,
           (g_get_monotonic_time () - load->started) / 1000.0, load->is_html ? "HTML" : "Text",
           strlen (load->html), g_hash_table_size (load->parts), load->scheme_queries,
           load->own_dark ? "ja" : "nein");

  set_header (self, load);
  g_clear_pointer (&self->parts, g_hash_table_unref);
  self->parts = g_steal_pointer (&load->parts);
  g_free (self->html);
  self->html = g_steal_pointer (&load->html);
  g_free (self->html_light);
  self->html_light = g_steal_pointer (&load->html_light);
  if (self->own_dark != load->own_dark)
    {
      self->own_dark = load->own_dark;
      apply_styles (self);
    }
  show_html (self);
}

/* ------------------------------------------------------------------ Klasse */

static void
revolution_mail_reader_root (GtkWidget *widget)
{
  RevolutionMailReader *self = REVOLUTION_MAIL_READER (widget);

  GTK_WIDGET_CLASS (revolution_mail_reader_parent_class)->root (widget);

  self->root = gtk_widget_get_root (widget);
  g_signal_connect_object (self->root, "notify::css-classes", G_CALLBACK (on_root_classes), self, G_CONNECT_SWAPPED);
  on_root_classes (self);
}

static void
revolution_mail_reader_unroot (GtkWidget *widget)
{
  RevolutionMailReader *self = REVOLUTION_MAIL_READER (widget);

  g_signal_handlers_disconnect_by_func (self->root, on_root_classes, self);
  self->root = NULL;

  GTK_WIDGET_CLASS (revolution_mail_reader_parent_class)->unroot (widget);
}

/* Webprozess beim Beenden der App zuerst beenden: sonst meldet er danach auf stderr
 * „GLib-GIO-WARNING … Error releasing name …Sandboxed.WebProcess…: Verbindung ist geschlossen“
 * (gemessen im ersten Lauf, build/m4-354/m4-354-netz.log; Gegenprobe mit kleinem Testprogramm:
 * ohne Beenden Warnung, mit webkit_web_view_terminate_web_process in „shutdown“ keine). */
static void
on_application_shutdown (RevolutionMailReader *self)
{
  webkit_web_view_terminate_web_process (self->web_view);
}

static void
revolution_mail_reader_dispose (GObject *object)
{
  RevolutionMailReader *self = REVOLUTION_MAIL_READER (object);

  self->generation++;
  g_clear_handle_id (&self->load_timeout, g_source_remove);
  g_clear_pointer (&self->pending, load_message_free);
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_READER);
  g_clear_pointer (&self->parts, g_hash_table_unref);
  g_clear_pointer (&self->html, g_free);
  g_clear_pointer (&self->html_light, g_free);

  G_OBJECT_CLASS (revolution_mail_reader_parent_class)->dispose (object);
}

static void
revolution_mail_reader_class_init (RevolutionMailReaderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = revolution_mail_reader_dispose;
  widget_class->root = revolution_mail_reader_root;
  widget_class->unroot = revolution_mail_reader_unroot;

  shown_signal = g_signal_new ("shown", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                               G_TYPE_NONE, 0);

  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/mail-reader.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, avatar);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, sender_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, folder_icon);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, folder_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, date_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, subject_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, to_caption);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, to_label);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, details_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, from_full);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, to_full);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, cc_full);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, date_full);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, content_overlay);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, spinner);
}

static void
revolution_mail_reader_init (RevolutionMailReader *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  self->web_view = create_web_view (self);
  gtk_overlay_set_child (self->content_overlay, GTK_WIDGET (self->web_view));
  self->ua_dark = adw_style_manager_get_dark (adw_style_manager_get_default ());
  g_signal_connect_object (adw_style_manager_get_default (), "notify::dark", G_CALLBACK (on_ua_dark_changed), self,
                           G_CONNECT_SWAPPED);
  apply_styles (self);
  if (g_application_get_default () != NULL)
    g_signal_connect_object (g_application_get_default (), "shutdown", G_CALLBACK (on_application_shutdown), self,
                             G_CONNECT_SWAPPED);
}

static void
start_load (gpointer user_data)
{
  RevolutionMailReader *self = user_data;
  LoadMessage *load = g_steal_pointer (&self->pending);
  g_autoptr (GTask) task = NULL;

  self->load_timeout = 0;
  /* Kreisel erst mit dem Laden, nicht schon beim Blättern (Wartezeit 100 ms). */
  gtk_widget_set_visible (GTK_WIDGET (self->spinner), TRUE);
  load->started = g_get_monotonic_time ();
  self->cancellable = g_cancellable_new ();
  task = g_task_new (self, self->cancellable, on_message_loaded, NULL);
  g_task_set_source_tag (task, revolution_mail_reader_show);
  g_task_set_task_data (task, load, (GDestroyNotify) load_message_free);
  g_task_run_in_thread (task, load_message_thread);
}

/* Auswahl in der Liste (mail-view.c): Kopf sofort aus dem Listeneintrag, Rest und Inhalt aus
 * dem Worker. Bricht ein laufendes Laden ab. */
void
revolution_mail_reader_show (RevolutionMailReader  *self,
                             CamelSession          *session,
                             RevolutionMailFolder  *folder,
                             RevolutionMailMessage *message)
{
  g_autoptr (RevolutionMailAccount) account = NULL;
  g_autofree char *sender = NULL, *subject = NULL, *folder_name = NULL, *icon = NULL, *account_name = NULL;
  g_autofree char *place = NULL;
  LoadMessage *load;

  g_return_if_fail (REVOLUTION_IS_MAIL_READER (self));
  g_return_if_fail (CAMEL_IS_SESSION (session));
  g_return_if_fail (REVOLUTION_IS_MAIL_FOLDER (folder));
  g_return_if_fail (REVOLUTION_IS_MAIL_MESSAGE (message));

  revolution_mail_reader_clear (self);

  g_object_get (message, "sender", &sender, "subject", &subject, NULL);
  g_object_get (folder, "account", &account, "display-name", &folder_name, "icon-name", &icon, NULL);
  g_object_get (account, "display-name", &account_name, NULL);
  /* Translators: Ordner und Konto im Nachrichtenkopf, z. B. „Eingang — Privat“. */
  place = g_strdup_printf (_("%s — %s"), folder_name, account_name);

  adw_avatar_set_text (self->avatar, sender);
  gtk_label_set_label (self->sender_label, sender);
  gtk_label_set_label (self->subject_label, subject);
  gtk_image_set_from_icon_name (self->folder_icon, icon);
  gtk_label_set_label (self->folder_label, place);

  load = g_new0 (LoadMessage, 1);
  load->session = g_object_ref (session);
  load->account_uid = g_strdup (revolution_mail_account_get_uid (account));
  load->full_name = g_strdup (revolution_mail_folder_get_full_name (folder));
  load->message_uid = g_strdup (revolution_mail_message_get_uid (message));
  load->generation = self->generation;
  self->pending = load;

  /* Laden erst nach 100 ms wie Evolution (e-mail-reader.c:4258-4268, „for when we're scrolling
   * rapidly through the message list“): schnelles Blättern lädt nur die letzte Nachricht. */
  self->load_timeout = g_timeout_add_once (100, start_load, self);
}

/* Keine Auswahl: Laden abbrechen, Inhalt leeren (mail-view.c zeigt dann den Leerzustand). */
void
revolution_mail_reader_clear (RevolutionMailReader *self)
{
  g_return_if_fail (REVOLUTION_IS_MAIL_READER (self));

  self->generation++;
  g_clear_handle_id (&self->load_timeout, g_source_remove);
  g_clear_pointer (&self->pending, load_message_free);
  g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_pointer (&self->html, g_free);
  g_clear_pointer (&self->html_light, g_free);
  g_clear_pointer (&self->parts, g_hash_table_unref);
  self->showing = FALSE;
  gtk_widget_set_visible (GTK_WIDGET (self->spinner), FALSE);

  gtk_label_set_label (self->date_label, "");
  gtk_label_set_label (self->to_label, "");
  gtk_label_set_label (self->from_full, "");
  gtk_label_set_label (self->to_full, "");
  gtk_label_set_label (self->cc_full, "");
  gtk_label_set_label (self->date_full, "");

  /* Alte Nachricht sofort ausblenden: WebView unsichtbar statt leere Seite laden (beim Blättern
   * lud sonst jeder Auswahlwechsel eine Seite, PM #354 Runde 2: Frames > 16,7 ms 3 → 12). */
  gtk_widget_set_visible (GTK_WIDGET (self->web_view), FALSE);
}
