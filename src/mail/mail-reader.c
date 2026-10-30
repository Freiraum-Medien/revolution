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
 * einzeln; #355 ergänzt dort die Freigabe je Mail und Absender und meldet Gesperrtes für den
 * Hinweis „Entfernte Inhalte gesperrt“ (Nachricht an die App wie Stamp „image-load-blocked“,
 * Abschnitt „#355: Hinweis und Freigabe“). Ein Inhaltsfilter
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
#include "mail-attachment.h"
#include "mail-invitation.h"
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
  GtkWidget      *notice;
  GMenu          *notice_menu;
  GtkWidget      *notice_button;
  GtkOverlay     *content_overlay;
  AdwSpinner     *spinner;
  RevolutionMailAttachmentBar *attachments;
  RevolutionMailInvitation *invitation;
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
  char           *load_uri;
  guint           load_serial;
  GtkRoot        *root;
  /* #355: entfernte Inhalte der angezeigten Nachricht freigegeben (Nachricht oder Absender) */
  gboolean        remote_allowed;
  char           *sender;
  GSimpleActionGroup *actions;
  /* #357: in der Konversation (mail-conversation.c): ohne eigenen Kopf, Höhe nach dem Inhalt. */
  gboolean        compact;
  int             fitted_width;
  GtkWidget      *sender_row;
  GtkWidget      *header_grid;
  GtkWidget      *content_card;
  AdwClamp       *fit_clamp;
  guint           fit_id;
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
  GPtrArray    *attachments;
  /* #376: erster Kalenderteil (iCalendar-Text) */
  char         *calendar;
  char         *to_short;
  char         *to_full;
  char         *cc_full;
  char         *from_full;
  gint64        date;
  char         *sender;
  gboolean      sender_allowed;
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
  g_clear_pointer (&load->attachments, g_ptr_array_unref);
  g_free (load->calendar);
  g_free (load->to_short);
  g_free (load->to_full);
  g_free (load->cc_full);
  g_free (load->from_full);
  g_free (load->sender);
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
  GPtrArray    *attachments;
  char         *calendar;
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

/* #376: Kalenderteil wie Evolution itip-formatter (e-mail-parser-itip.c:54-58: text/calendar, application/ics);
 * der erste zählt, er bleibt außerdem Anhang wie bisher (#356). */
static gboolean
is_calendar (CamelContentType *type)
{
  return camel_content_type_is (type, "text", "calendar") || camel_content_type_is (type, "application", "ics");
}

static void
note_calendar (Collect       *collect,
               CamelMimePart *part)
{
  if (collect->calendar == NULL && is_calendar (camel_mime_part_get_content_type (part)))
    collect->calendar = decode_text (part, collect->cancellable);
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
 * ohne Signatur/Verschlüsselung; Kalenderteile nur für die Einladungskarte (#376, note_calendar). Darstellbare Texte kommen der Reihe nach in den
 * Körper, HTML unverändert, Text über append_plain. Anhänge (#356, Liste collect->attachments):
 * angehängte Nachrichten, Teile mit Content-Disposition „attachment“ und alle Blätter, die weder
 * text/plain noch text/html sind (Evolution: Teile ohne eigenen Parser werden Anhang,
 * e-mail-parser.c:760-763; message/rfc822 immer, e-mail-parser-multipart-mixed.c:314-319). */
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

          /* #376: Einladungen von Exchange und Google tragen text/calendar als eigene Fassung neben Text und HTML;
           * Evolution zeigt dann die Karte statt des Textes (e-mail-parser-multipart-alternative.c: letzte
           * darstellbare Fassung), Revolution zeigt beides: Karte und Text. */
          for (guint i = 0; i < camel_multipart_get_number (multipart); i++)
            note_calendar (collect, camel_multipart_get_part (multipart, i));
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
                {
                  store_part (collect, child);
                  /* #356: Anhang, wenn der Körper ihn nicht über cid: nutzt (drop_used_parts). */
                  g_ptr_array_add (collect->attachments, g_object_ref (child));
                }
            }
        }
      else if (camel_content_type_is (type, "multipart", "signed"))
        {
          /* #356: nur der signierte Inhalt; die Signatur ist kein Anhang (Evolution
           * e-mail-parser-multipart-signed.c behandelt sie als Signatur, nicht als Anhang). */
          if (camel_multipart_get_number (multipart) > 0)
            collect_part (collect, camel_multipart_get_part (multipart, 0), depth + 1);
        }
      else
        {
          for (guint i = 0; i < camel_multipart_get_number (multipart); i++)
            collect_part (collect, camel_multipart_get_part (multipart, i), depth + 1);
        }
      return;
    }

  store_part (collect, part);
  note_calendar (collect, part);

  if (CAMEL_IS_MIME_MESSAGE (content) || g_strcmp0 (camel_mime_part_get_disposition (part), "attachment") == 0 ||
      !(camel_content_type_is (type, "text", "html") || camel_content_type_is (type, "text", "plain")))
    {
      g_ptr_array_add (collect->attachments, g_object_ref (part));
      return;
    }

  if (camel_content_type_is (type, "text", "html"))
    {
      g_autofree char *html = decode_text (part, collect->cancellable);

      g_string_append (collect->body, html);
      collect->html = TRUE;
    }
  else if (camel_content_type_is (type, "text", "plain"))
    append_plain (collect, part);
}

/* Nutzt der Körper „cid:<cid>“? Jede Fundstelle bis zum Ende der Adresse (Anführungszeichen,
 * Klammer, Leerraum, Tag-Ende) genau vergleichen, %-kodiert wie on_cid_request entschlüsselt;
 * „cid:bild1“ zählt nicht für „bild“. */
static gboolean
cid_used (const char *body,
          const char *cid)
{
  for (const char *p = strstr (body, "cid:"); p != NULL; p = strstr (p, "cid:"))
    {
      g_autofree char *token = NULL, *unescaped = NULL;
      gsize length;

      p += 4;
      length = strcspn (p, "\"' )<>\t\r\n");
      token = g_strndup (p, length);
      unescaped = g_uri_unescape_string (token, NULL);
      if (g_strcmp0 (unescaped != NULL ? unescaped : token, cid) == 0)
        return TRUE;
    }

  return FALSE;
}

/* #356: Teile, die der Körper über cid: einbindet (Inline-Bilder), sind kein Anhang, außer mit
 * Content-Disposition „attachment“. Übrige als RevolutionMailAttachment (Name, Typ, Größe). */
static GPtrArray *
attachments_for (Collect *collect)
{
  GPtrArray *attachments = g_ptr_array_new_with_free_func (g_object_unref);

  for (guint i = 0; i < collect->attachments->len; i++)
    {
      CamelMimePart *part = g_ptr_array_index (collect->attachments, i);
      const char *cid = camel_mime_part_get_content_id (part);

      if (cid != NULL && *cid != '\0' && g_strcmp0 (camel_mime_part_get_disposition (part), "attachment") != 0 &&
          cid_used (collect->body->str, cid))
        continue;

      g_ptr_array_add (attachments, revolution_mail_attachment_new (part, collect->cancellable));
    }

  return attachments;
}

/* #359: Inhalt einer Nachricht als HTML wie im Lesebereich (collect_part), für das Zitat im Composer
 * (composer.c). Im Worker. */
char *
revolution_mail_reader_message_html (CamelMimeMessage *message,
                                     GCancellable     *cancellable)
{
  Collect collect = { 0 };

  collect.body = g_string_new (NULL);
  collect.parts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) reader_part_free);
  collect.attachments = g_ptr_array_new_with_free_func (g_object_unref);
  collect.cancellable = cancellable;
  collect_part (&collect, CAMEL_MIME_PART (message), 0);
  g_hash_table_unref (collect.parts);
  g_ptr_array_unref (collect.attachments);
  g_free (collect.calendar);

  return g_string_free (collect.body, FALSE);
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

/* ------------------------------------------------------------------ #355: Freigabeliste */

/* Absender und Domains mit Freigabe für entfernte Inhalte, wie Evolution e-mail-remote-content.c
 * (Tabelle „mails“: Adresse oder „@domain“, klein geschrieben, :300-312, 446-468) und
 * e-mail-display.c:247-269 (geprüft werden die Adressen aus From). Eigene Datei statt Evolutions
 * remote-content.db (e-mail-backend.c:1413, CamelDB): eine Zeile je Eintrag in
 * ~/.local/share/revolution/remote-content.txt (Architektur 4). Lesen und Schreiben nur im Worker;
 * der Mutex ordnet Lesen und Schreiben aus mehreren Worker-Threads. */
static GMutex remote_list_lock;

static char *
remote_list_path (void)
{
  return g_build_filename (g_get_user_data_dir (), USER_DIR_NAME, "remote-content.txt", NULL);
}

/* Im Worker: Liste lesen (fehlt die Datei, ist sie leer). */
static char **
remote_list_read (void)
{
  g_autofree char *path = remote_list_path ();
  g_autofree char *text = NULL;
  g_autoptr (GError) error = NULL;

  g_mutex_lock (&remote_list_lock);
  if (!g_file_get_contents (path, &text, NULL, &error) && !g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
    g_warning ("Entfernte Inhalte: %s nicht gelesen: %s", path, error->message);
  g_mutex_unlock (&remote_list_lock);

  return g_strsplit (text != NULL ? text : "", "\n", -1);
}

/* Freigabe wie e_mail_remote_content_has_mail (e-mail-remote-content.c:563-583): Adresse oder ihre
 * Domain ab „@“, ohne Rücksicht auf Groß-/Kleinschreibung. */
static gboolean
remote_list_has (char       **list,
                 const char  *address)
{
  const char *at = strchr (address, '@');

  for (char **entry = list; *entry != NULL; entry++)
    if (**entry != '\0' && (g_ascii_strcasecmp (*entry, address) == 0 || (at != NULL && g_ascii_strcasecmp (*entry, at) == 0)))
      return TRUE;

  return FALSE;
}

/* Im Worker: Absender (erste Adresse aus From) und ob eine Adresse aus From freigegeben ist. */
static void
check_sender (LoadMessage          *load,
              CamelInternetAddress *from)
{
  g_auto (GStrv) list = remote_list_read ();

  for (int i = 0; from != NULL && i < camel_address_length (CAMEL_ADDRESS (from)); i++)
    {
      const char *email = NULL;

      if (!camel_internet_address_get (from, i, NULL, &email) || email == NULL || *email == '\0')
        continue;
      if (load->sender == NULL)
        load->sender = g_ascii_strdown (email, -1);
      load->sender_allowed |= remote_list_has (list, email);
    }
}

/* Im Worker: Eintrag (Adresse oder „@domain“) klein geschrieben anhängen, wenn er fehlt. */
static void
remote_list_add_thread (GTask        *task,
                        gpointer      source_object G_GNUC_UNUSED,
                        gpointer      task_data,
                        GCancellable *cancellable G_GNUC_UNUSED)
{
  g_autofree char *path = remote_list_path ();
  g_autofree char *text = NULL;
  g_autofree char *entry = g_ascii_strdown (task_data, -1);
  g_auto (GStrv) list = NULL;
  GError *error = NULL;
  gboolean saved = TRUE;

  /* Ein Eintrag ist eine Zeile: Zeilenumbrüche würden weitere Einträge anlegen. */
  if (*entry == '\0' || strpbrk (entry, "\r\n") != NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, _("Invalid entry “%s”"), entry);
      return;
    }

  g_mutex_lock (&remote_list_lock);
  g_file_get_contents (path, &text, NULL, NULL);
  list = g_strsplit (text != NULL ? text : "", "\n", -1);
  if (!g_strv_contains ((const char * const *) list, entry))
    {
      g_autofree char *updated = g_strconcat (text != NULL ? text : "", entry, "\n", NULL);

      saved = g_file_set_contents_full (path, updated, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, &error);
    }
  g_mutex_unlock (&remote_list_lock);

  if (saved)
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, error);
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
                               _("Account %s unavailable"), load->account_uid);
      return;
    }

  /* Ordner immer über camel_store_get_folder_sync (docs/ENTWICKLUNG.md „Camel-Ordner“). */
  folder = camel_store_get_folder_sync (CAMEL_STORE (service), load->full_name, 0, cancellable, &error);
  if (folder != NULL)
    message = camel_folder_get_message_sync (folder, load->message_uid, cancellable, &error);
  if (message == NULL)
    {
      g_task_return_error (task, error);
      return;
    }

  load->from_full = format_address (camel_mime_message_get_from (message));
  check_sender (load, camel_mime_message_get_from (message));
  load->to_short = names_for (camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_TO));
  load->to_full = format_address (camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_TO));
  load->cc_full = format_address (camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_CC));
  load->date = camel_mime_message_get_date (message, &offset);
  if (load->date == CAMEL_MESSAGE_DATE_CURRENT || load->date <= 0)
    load->date = camel_mime_message_get_date_received (message, &offset);

  collect.body = g_string_new (NULL);
  collect.parts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) reader_part_free);
  collect.attachments = g_ptr_array_new_with_free_func (g_object_unref);
  collect.cancellable = cancellable;
  collect_part (&collect, CAMEL_MIME_PART (message), 0);
  load->attachments = attachments_for (&collect);
  g_ptr_array_unref (collect.attachments);
  load->calendar = collect.calendar;

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
static void fit_content (RevolutionMailReader *self);
static void refit_later (gpointer user_data);

/* Fassung je Theme (PM #354): Theme 1 (Oberfläche hell) die Mail unverändert; Themes 2/3 die helle
 * Fassung (prepare_schemes), außer Theme 3 bei eigener Dunkel-Fassung der Mail. */
static const char *
current_html (RevolutionMailReader *self)
{
  if (self->ua_dark && self->html_light != NULL && !(self->dark && self->own_dark))
    return self->html_light;

  return self->html;
}

/* #355: Die Erweiterung hat die Freigabe für die Ladung mit dieser Adresse bestätigt (Antwort trägt
 * die Adresse); erst jetzt laden. Antworten zu älteren Ladungen bleiben ohne Wirkung. */
static void
on_remote_allow_reply (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  g_autoptr (RevolutionMailReader) self = user_data;
  g_autoptr (WebKitUserMessage) reply = NULL;
  g_autoptr (GError) error = NULL;
  GVariant *parameters;

  reply = webkit_web_view_send_message_to_page_finish (WEBKIT_WEB_VIEW (source), result, &error);
  parameters = reply != NULL ? webkit_user_message_get_parameters (reply) : NULL;

  if (reply == NULL)
    {
      if (self->showing && self->html != NULL)
        {
          self->showing = FALSE;
          show_error (self, _("Remote Content Not Loaded"), error->message);
        }
      return;
    }

  if (!self->showing || self->html == NULL || parameters == NULL ||
      !g_variant_is_of_type (parameters, G_VARIANT_TYPE_STRING) ||
      g_strcmp0 (g_variant_get_string (parameters, NULL), self->load_uri) != 0)
    return;

  g_debug ("Nachricht: zeigt %s mit entfernten Inhalten", self->load_uri);
  webkit_web_view_load_html (self->web_view, current_html (self), self->load_uri);
}

static void
show_html (RevolutionMailReader *self)
{
  const char *html = current_html (self);

  if (html == NULL)
    return;

  /* Nach fehlgeschlagener Bestätigung neu versuchen, mit neuem Webprozess. */
  if (self->guard == GUARD_FAILED)
    {
      self->guard = GUARD_NONE;
      webkit_web_view_terminate_web_process (self->web_view);
    }

  if (self->guard == GUARD_OK)
    {
      /* Sichtbar erst mit dem neuen Dokument (on_load_changed). Eigene Adresse je Ladung, damit
       * nur Ereignisse dieser Ladung zählen; Abfrage statt Fragment, sonst behandelt WebKit den
       * Wechsel von about:blank als Sprung im selben Dokument und lädt nichts (gemessen: keine
       * load-changed-Ereignisse nach „about:blank#…“). */
      self->showing = TRUE;
      g_free (self->load_uri);
      self->load_uri = g_strdup_printf ("about:blank?revolution-%u", ++self->load_serial);
      /* #355: freigegeben: die Erweiterung erlaubt http/https nur für diese Adresse. */
      if (self->remote_allowed)
        {
          webkit_web_view_send_message_to_page (self->web_view,
                                                webkit_user_message_new ("revolution-remote-allow",
                                                                         g_variant_new_string (self->load_uri)),
                                                NULL, on_remote_allow_reply, g_object_ref (self));
          return;
        }
      g_debug ("Nachricht: zeigt %s", self->load_uri);
      webkit_web_view_load_html (self->web_view, html, self->load_uri);
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
      g_autofree char *detail = g_strdup_printf (_("Protection against network access not active: %s"), error->message);

      self->guard = GUARD_FAILED;
      show_error (self, _("Message Not Shown"), detail);
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

/* #357: In der Konversation stehen mehrere Nachrichten untereinander in einer scrollbaren Liste
 * (mail-conversation.c); jede WebView bekommt die Höhe ihres Inhalts wie Stamp (stamp-webview.c:115-141
 * Größe aus dem Webprozess, stamp-message-list-item.c:826-853 als Höhenwunsch). JavaScript bleibt für
 * die Seite aus; die Höhe misst die Web-Process-Extension (web-extension.c, „revolution-height“) und
 * nennt sie mit der Adresse der Ladung, damit nur die aktuelle zählt. Obergrenze wie Stamp
 * (stamp-webview.c:109, 131-132: 8 Mio. Pixel), darüber scrollt die WebView selbst. Gemessen wird nach
 * jeder fertigen Ladung (auch nach Freigabe entfernter Inhalte) und bei neuer Breite. */
#define FIT_MAX_PIXELS (8 * 1024 * 1024)

static void
on_height_reply (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
  g_autoptr (RevolutionMailReader) self = user_data;
  g_autoptr (WebKitUserMessage) reply = webkit_web_view_send_message_to_page_finish (WEBKIT_WEB_VIEW (source), result, NULL);
  GVariant *parameters = reply != NULL ? webkit_user_message_get_parameters (reply) : NULL;
  const char *uri = NULL;
  int height = 0, width;

  if (parameters == NULL || !g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(si)")))
    return;

  g_variant_get (parameters, "(&si)", &uri, &height);
  if (g_strcmp0 (uri, self->load_uri) != 0 || height <= 0)
    return;

  /* Noch ohne Breite (WebView bis zur Anzeige unsichtbar): nach der Zuteilung erneut messen. */
  if (gtk_widget_get_width (GTK_WIDGET (self->web_view)) <= 0 && self->fit_id == 0)
    self->fit_id = g_timeout_add_once (100, refit_later, self);
  width = MAX (gtk_widget_get_width (GTK_WIDGET (self->web_view)), 1);
  height = MIN (height, FIT_MAX_PIXELS / width);
  g_debug ("Nachricht: Höhe %d bei Breite %d (%s)", height, width, uri);
  /* Mindesthöhe der WebView und Obergrenze ihrer natürlichen Höhe: WebKitGTK meldet als natürliche Höhe
   * die Höhe des Inhalts einschließlich der bisherigen Ansicht, sie schrumpft daher nicht von selbst
   * (gemessen: Karte 400 px bei Inhalt 43 px). */
  adw_clamp_set_maximum_size (self->fit_clamp, height);
  adw_clamp_set_tightening_threshold (self->fit_clamp, height);
  gtk_widget_set_size_request (GTK_WIDGET (self->web_view), -1, height);
}

static void
refit_later (gpointer user_data)
{
  RevolutionMailReader *self = user_data;

  self->fit_id = 0;
  fit_content (self);
}

static void
fit_content (RevolutionMailReader *self)
{
  if (!self->compact || self->load_uri == NULL || self->html == NULL)
    return;

  webkit_web_view_send_message_to_page (self->web_view,
                                        webkit_user_message_new ("revolution-height",
                                                                 g_variant_new_string (self->load_uri)),
                                        NULL, on_height_reply, g_object_ref (self));
}

/* Sichtbar bei COMMITTED oder FINISHED der aktuellen Ladung (load_uri); Ereignisse älterer Ladungen
 * bleiben so ohne Wirkung. #412: Unterbricht load_html eine laufende Ladung, meldet WebKitGTK 2.52.6
 * zuerst ein FINISHED der alten Ladung mit schon neuer Adresse, danach (STARTED,) COMMITTED und
 * FINISHED der neuen; beim falschen FINISHED lädt die WebView noch (webkit_web_view_is_loading TRUE),
 * beim echten nicht mehr. Die Adresse bei STARTED ist dabei unzuverlässig (alte Adresse oder
 * about:blank), und folgen zwei Ladungen dicht aufeinander (Theme-Wechsel), kommt für die letzte unter
 * Umständen nur FINISHED (#354 R3). Gemessen: Prüf-Lauf #354 R4 und #412 (ladefolge, rennen). */
static void
on_load_changed (RevolutionMailReader *self,
                 WebKitLoadEvent       event)
{
  const char *uri = webkit_web_view_get_uri (self->web_view);

  if (self->showing && event == WEBKIT_LOAD_FINISHED && g_strcmp0 (uri, self->load_uri) == 0 &&
      webkit_web_view_is_loading (self->web_view))
    {
      g_debug ("Nachricht: %s übergangen (finished der vorigen Ladung, lädt noch)", self->load_uri);
      return;
    }

  if (self->showing && (event == WEBKIT_LOAD_COMMITTED || event == WEBKIT_LOAD_FINISHED) &&
      g_strcmp0 (uri, self->load_uri) == 0)
    {
      if (!gtk_widget_get_visible (GTK_WIDGET (self->web_view)))
        g_debug ("Nachricht: sichtbar %s (%s)", self->load_uri, event == WEBKIT_LOAD_FINISHED ? "finished" : "committed");
      gtk_widget_set_visible (GTK_WIDGET (self->web_view), TRUE);
      if (event == WEBKIT_LOAD_FINISHED)
        {
          g_debug ("Nachricht: fertig %s", self->load_uri);
          self->showing = FALSE;
          fit_content (self);
          g_signal_emit (self, shown_signal, 0);
        }
      return;
    }

  if (event != WEBKIT_LOAD_FINISHED || self->guard != GUARD_LOADING)
    return;

  self->guard = GUARD_ASKING;
  webkit_web_view_send_message_to_page (self->web_view, webkit_user_message_new ("revolution-guard", NULL),
                                        NULL, on_guard_reply, g_object_ref (self));
}

/* Fehlgeschlagenes Laden: kein Fehlerdokument von WebKit (TRUE). Abbruch durch eine neuere Ladung
 * ist normal (WEBKIT_NETWORK_ERROR_CANCELLED); schlägt die aktuelle Ladung fehl, Banner. */
static gboolean
on_load_failed (RevolutionMailReader *self,
                WebKitLoadEvent       event G_GNUC_UNUSED,
                const char           *uri,
                GError               *error)
{
  if (g_error_matches (error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED) ||
      !self->showing || g_strcmp0 (uri, self->load_uri) != 0)
    return TRUE;

  self->showing = FALSE;
  show_error (self, _("Message Not Shown"), error->message);
  return TRUE;
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

/* ------------------------------------------------------------------ #355: Hinweis und Freigabe */

static void
set_notice (RevolutionMailReader *self,
            gboolean              visible)
{
  gtk_widget_set_visible (self->notice, visible);
  /* #422: Rand des Kopfs wie revolution_mail_reader_set_compact (Abstand zwischen Hinweis und „An:“). */
  gtk_widget_set_margin_top (self->header_grid, self->compact && !visible ? 0 : 14);
  g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self->actions), "load-remote")),
                               visible);
  g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self->actions), "allow-remote")),
                               visible);
}

/* Meldung der Erweiterung: im Dokument dieser Ladung wurden http-/https-Inhalte gesperrt. */
static gboolean
on_user_message (RevolutionMailReader *self,
                 WebKitUserMessage    *message)
{
  GVariant *parameters = webkit_user_message_get_parameters (message);

  if (g_strcmp0 (webkit_user_message_get_name (message), "revolution-remote-blocked") != 0)
    return FALSE;

  if (self->html != NULL && !self->remote_allowed && parameters != NULL &&
      g_variant_is_of_type (parameters, G_VARIANT_TYPE_STRING) &&
      g_strcmp0 (g_variant_get_string (parameters, NULL), self->load_uri) == 0)
    {
      if (!gtk_widget_get_visible (self->notice))
        g_debug ("Entfernte Inhalte: gesperrt in %s (Absender %s), Hinweis sichtbar", self->load_uri,
                 self->sender != NULL ? self->sender : "-");
      set_notice (self, TRUE);
    }

  return TRUE;
}

/* reader.load-remote (Strg+I wie Evolution „mail-load-images“, e-mail-reader.c:3116-3121): nur
 * diese Nachricht, wie e_mail_display_load_images (e-mail-display.c:3641-3647); die nächste
 * Nachricht ist wieder gesperrt (e_mail_display_load setzt zurück, :3469-3473). */
static void
on_load_remote (GSimpleAction *action G_GNUC_UNUSED,
                GVariant      *parameter G_GNUC_UNUSED,
                gpointer       user_data)
{
  RevolutionMailReader *self = user_data;

  if (self->html == NULL)
    return;

  g_debug ("Entfernte Inhalte: für diese Nachricht freigegeben");
  self->remote_allowed = TRUE;
  set_notice (self, FALSE);
  show_html (self);
}

static void
on_remote_list_saved (GObject      *source_object,
                      GAsyncResult *result,
                      gpointer      user_data)
{
  RevolutionMailReader *self = REVOLUTION_MAIL_READER (source_object);
  g_autoptr (GError) error = NULL;
  guint generation = GPOINTER_TO_UINT (user_data);

  if (!g_task_propagate_boolean (G_TASK (result), &error))
    {
      show_error (self, _("Permission Not Saved"), error->message);
      return;
    }

  g_debug ("Entfernte Inhalte: Freigabe gespeichert");
  /* Inzwischen andere Nachricht gewählt: die zeigt der nächste Ladevorgang ohnehin mit Liste. */
  if (generation != self->generation || self->html == NULL)
    return;

  self->remote_allowed = TRUE;
  show_html (self);
}

/* reader.allow-remote (Adresse oder „@domain“): dauerhaft wie Evolution „Allow remote content for
 * …“ (e-mail-remote-content-popover.c:140-160, 225-240), danach diese Nachricht neu laden. */
static void
on_allow_remote (GSimpleAction *action G_GNUC_UNUSED,
                 GVariant      *parameter,
                 gpointer       user_data)
{
  RevolutionMailReader *self = user_data;
  g_autoptr (GTask) task = NULL;
  const char *entry = g_variant_get_string (parameter, NULL);

  if (*entry == '\0')
    return;

  g_debug ("Entfernte Inhalte: immer für %s freigeben", entry);
  set_notice (self, FALSE);
  task = g_task_new (self, NULL, on_remote_list_saved, GUINT_TO_POINTER (self->generation));
  g_task_set_source_tag (task, on_allow_remote);
  g_task_set_task_data (task, g_strdup (entry), g_free);
  g_task_run_in_thread (task, remote_list_add_thread);
}

static const GActionEntry reader_actions[] = {
  { "load-remote", on_load_remote, NULL, NULL, NULL, { 0 } },
  { "allow-remote", on_allow_remote, "s", NULL, NULL, { 0 } },
};

/* Menü des Hinweises: Absender und seine Domain wie Evolution (e-mail-remote-content-popover.c:
 * 60-106, 225-240). */
static void
fill_notice_menu (RevolutionMailReader *self)
{
  const char *at = self->sender != NULL ? strchr (self->sender, '@') : NULL;

  g_menu_remove_all (self->notice_menu);
  if (self->sender == NULL)
    return;

  {
    /* Translators: menu of the “Remote Content Blocked” notice, %s is the sender address. */
    g_autofree char *label = g_strdup_printf (_("Always Load for %s"), self->sender);
    g_autoptr (GMenuItem) item = g_menu_item_new (label, NULL);

    g_menu_item_set_action_and_target_value (item, "reader.allow-remote", g_variant_new_string (self->sender));
    g_menu_append_item (self->notice_menu, item);
  }

  if (at != NULL && at != self->sender && at[1] != '\0')
    {
      /* Translators: menu of the notice, %s is the domain with “@”, e.g. “@example.org”. */
      g_autofree char *label = g_strdup_printf (_("Always Load for Everyone at %s"), at);
      g_autoptr (GMenuItem) item = g_menu_item_new (label, NULL);

      g_menu_item_set_action_and_target_value (item, "reader.allow-remote", g_variant_new_string (at));
      g_menu_append_item (self->notice_menu, item);
    }
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

/* Erweiterung aus dem Build-Ordner, wenn die App von dort läuft (docs/ENTWICKLUNG.md „Build und Test“),
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

/* Vorab-Verbindungen aus <link rel=preconnect|dns-prefetch|preload|prefetch> und Speculation
 * Rules laufen nicht über WebKitWebPage::send-request: im ersten Lauf unter strace baute der
 * Netzwerkprozess für <link rel=preconnect> eine TCP-Verbindung zum Bildserver auf. Daher aus.
 * #359: auch für den Composer (zitierte Mails, composer.c create_editor). */
void
revolution_mail_reader_disable_prefetch (WebKitSettings *settings)
{
  g_autoptr (WebKitFeatureList) features = webkit_settings_get_all_features ();

  for (gsize i = 0; i < webkit_feature_list_get_length (features); i++)
    {
      WebKitFeature *feature = webkit_feature_list_get (features, i);

      if (g_strv_contains (PREFETCH_FEATURES, webkit_feature_get_identifier (feature)))
        webkit_settings_set_feature_enabled (settings, feature, FALSE);
    }
}

static gpointer
create_settings (gpointer data G_GNUC_UNUSED)
{
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
  revolution_mail_reader_disable_prefetch (settings);

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
  gtk_accessible_update_property (GTK_ACCESSIBLE (web_view), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Message Content"), -1);

  g_signal_connect_object (web_view, "decide-policy", G_CALLBACK (on_decide_policy), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (web_view, "context-menu", G_CALLBACK (on_context_menu), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (web_view, "load-changed", G_CALLBACK (on_load_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (web_view, "load-failed", G_CALLBACK (on_load_failed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (web_view, "user-message-received", G_CALLBACK (on_user_message), self, G_CONNECT_SWAPPED);
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
    /* Translators: date in the message header, %s is the time. */
    return g_strdup_printf (_("Today, %s"), time);
  if (days == 1)
    /* Translators: date in the message header, %s is the time. */
    return g_strdup_printf (_("Yesterday, %s"), time);

  /* Translators: date in the message header (g_date_time_format), e.g. “10/17/2026, 14:05”. */
  return g_date_time_format (local, _("%m/%d/%Y, %H:%M"));
}

static void
set_header (RevolutionMailReader *self,
            LoadMessage          *load)
{
  g_autoptr (GDateTime) local = load->date > 0 ? g_date_time_new_from_unix_local (load->date) : NULL;
  g_autofree char *date = local != NULL ? header_date (local) : g_strdup ("");
  /* Translators: full date in the details (g_date_time_format). */
  g_autofree char *full = local != NULL ? g_date_time_format (local, _("%A, %B %-d, %Y, %H:%M:%S")) : g_strdup ("");

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

      show_error (self, _("Message Not Loaded"), detail);
      return;
    }

  g_debug ("Nachricht: %s %s %s geladen nach %.0f ms (%s, %zu Bytes, %u Teile mit Content-ID, "
           "%u Abfragen prefers-color-scheme, eigene Dunkel-Fassung %s, %u Anhänge)",
           load->account_uid, load->full_name, load->message_uid,
           (g_get_monotonic_time () - load->started) / 1000.0, load->is_html ? "HTML" : "Text",
           strlen (load->html), g_hash_table_size (load->parts), load->scheme_queries,
           load->own_dark ? "ja" : "nein", load->attachments->len);

  set_header (self, load);
  revolution_mail_attachment_bar_set (self->attachments, load->attachments);
  revolution_mail_invitation_set (self->invitation, load->account_uid, load->calendar);
  /* #355: Absender freigegeben (Liste im Worker gelesen): gleich mit entfernten Inhalten laden. */
  g_free (self->sender);
  self->sender = g_steal_pointer (&load->sender);
  self->remote_allowed = load->sender_allowed;
  if (load->sender_allowed)
    g_debug ("Entfernte Inhalte: Absender %s freigegeben", self->sender);
  fill_notice_menu (self);
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

/* #357: neue Breite in der Konversation: Höhe neu messen (Umbruch ändert sich). */
static void
revolution_mail_reader_size_allocate (GtkWidget *widget,
                                      int        width,
                                      int        height,
                                      int        baseline)
{
  RevolutionMailReader *self = REVOLUTION_MAIL_READER (widget);
  int web_width;

  GTK_WIDGET_CLASS (revolution_mail_reader_parent_class)->size_allocate (widget, width, height, baseline);

  /* Breite der WebView, nicht des Lesebereichs: sie ist bis zur ersten Anzeige unsichtbar (Breite 0), die
   * Messung beim Laden kann also vor ihrer Breite liegen (gemessen: „Höhe 268 bei Breite 1“). */
  web_width = gtk_widget_get_width (GTK_WIDGET (self->web_view));
  if (self->compact && web_width > 0 && web_width != self->fitted_width)
    {
      self->fitted_width = web_width;
      fit_content (self);
    }
}

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
  /* #357: Lesebereiche der Konversation verschwinden beim Zuklappen und vor dem Beenden der App
   * (on_application_shutdown kommt dann nicht mehr an): Webprozess wie dort zuerst beenden, sonst meldet er
   * „Error releasing name …WebProcess…“ (gemessen, build/m4-357/probe3.log). */
  g_clear_handle_id (&self->fit_id, g_source_remove);
  if (self->compact && self->web_view != NULL)
    webkit_web_view_terminate_web_process (self->web_view);
  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_READER);
  g_clear_pointer (&self->parts, g_hash_table_unref);
  g_clear_pointer (&self->html, g_free);
  g_clear_pointer (&self->html_light, g_free);
  g_clear_pointer (&self->load_uri, g_free);
  g_clear_pointer (&self->sender, g_free);
  g_clear_object (&self->actions);

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
  widget_class->size_allocate = revolution_mail_reader_size_allocate;

  g_type_ensure (REVOLUTION_TYPE_MAIL_ATTACHMENT_BAR);
  g_type_ensure (REVOLUTION_TYPE_MAIL_INVITATION);
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
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, notice);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, notice_menu);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, notice_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, content_overlay);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, spinner);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, attachments);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, invitation);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, sender_row);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, header_grid);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailReader, content_card);
}

static void
revolution_mail_reader_init (RevolutionMailReader *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  /* #355: Aktionen des Hinweises, Kürzel in mail-reader.blp. */
  self->actions = g_simple_action_group_new ();
  g_action_map_add_action_entries (G_ACTION_MAP (self->actions), reader_actions, G_N_ELEMENTS (reader_actions), self);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "reader", G_ACTION_GROUP (self->actions));
  set_notice (self, FALSE);
  {
    /* Kürzel aus dem Accelerator (mail-reader.blp, Strg+I), nicht als fester Text in der Übersetzung. */
    g_autofree char *accel = gtk_accelerator_get_label (GDK_KEY_i, GDK_CONTROL_MASK);
    /* Translators: tooltip of the “Load” button in the notice, %s is the shortcut, e.g. “Ctrl+I”. */
    g_autofree char *tooltip = g_strdup_printf (_("Load Remote Content of This Message (%s)"), accel);

    gtk_widget_set_tooltip_text (self->notice_button, tooltip);
  }

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
                             RevolutionMailFolder  *place_folder,
                             RevolutionMailMessage *message)
{
  g_autoptr (RevolutionMailAccount) account = NULL;
  g_autoptr (RevolutionMailAccount) place_account = NULL;
  g_autofree char *sender = NULL, *subject = NULL, *folder_name = NULL, *icon = NULL, *account_name = NULL;
  g_autofree char *place = NULL;
  LoadMessage *load;

  g_return_if_fail (REVOLUTION_IS_MAIL_READER (self));
  g_return_if_fail (CAMEL_IS_SESSION (session));
  g_return_if_fail (REVOLUTION_IS_MAIL_FOLDER (folder));
  g_return_if_fail (REVOLUTION_IS_MAIL_FOLDER (place_folder));
  g_return_if_fail (REVOLUTION_IS_MAIL_MESSAGE (message));

  revolution_mail_reader_clear (self);

  g_object_get (message, "sender", &sender, "subject", &subject, NULL);
  /* #351: Geladen wird aus folder, angezeigt wird der Ort place_folder: bei Nachrichten aus den
   * Favoriten der echte Ordner wie der Prototyp („Eingang — Privat“, mail-view.c). */
  g_object_get (folder, "account", &account, NULL);
  g_object_get (place_folder, "account", &place_account, "display-name", &folder_name, "icon-name", &icon, NULL);
  g_object_get (place_account, "display-name", &account_name, NULL);
  /* Translators: folder and account in the message header, e.g. “Inbox — Private”. */
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
  self->remote_allowed = FALSE;
  g_clear_pointer (&self->sender, g_free);
  set_notice (self, FALSE);
  gtk_widget_set_visible (GTK_WIDGET (self->spinner), FALSE);
  revolution_mail_attachment_bar_set (self->attachments, NULL);
  revolution_mail_invitation_set (self->invitation, NULL, NULL);

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

/* #357: Darstellung in der Konversation (mail-conversation.c): ohne Bild, Absender, Datum und Betreff
 * (stehen im Kopf der Nachricht in der Konversation), Inhalt so hoch wie die Mail (fit_content). */
void
revolution_mail_reader_set_compact (RevolutionMailReader *self,
                                    gboolean              compact)
{
  g_return_if_fail (REVOLUTION_IS_MAIL_READER (self));

  self->compact = compact;
  if (compact && self->fit_clamp == NULL)
    {
      g_autoptr (WebKitWebView) web_view = g_object_ref (self->web_view);

      self->fit_clamp = ADW_CLAMP (adw_clamp_new ());
      gtk_orientable_set_orientation (GTK_ORIENTABLE (self->fit_clamp), GTK_ORIENTATION_VERTICAL);
      adw_clamp_set_unit (self->fit_clamp, ADW_LENGTH_UNIT_PX);
      adw_clamp_set_maximum_size (self->fit_clamp, 1);
      adw_clamp_set_tightening_threshold (self->fit_clamp, 1);
      gtk_overlay_set_child (self->content_overlay, NULL);
      adw_clamp_set_child (self->fit_clamp, GTK_WIDGET (web_view));
      gtk_overlay_set_child (self->content_overlay, GTK_WIDGET (self->fit_clamp));
    }
  gtk_widget_set_visible (GTK_WIDGET (self->avatar), !compact);
  gtk_widget_set_visible (self->sender_row, !compact);
  gtk_widget_set_visible (GTK_WIDGET (self->subject_label), !compact);
  /* #422: „An:“ direkt unter dem Kopf der Nachricht in der Konversation (sonst Lücke, Rand 14 wie mail-reader.blp);
   * mit sichtbarem Hinweis bleibt der Rand als Abstand zur Pille (set_notice). */
  gtk_widget_set_margin_top (self->header_grid, compact && !gtk_widget_get_visible (self->notice) ? 0 : 14);
  gtk_widget_set_vexpand (self->content_card, !compact);
  gtk_widget_set_size_request (self->content_card, -1, compact ? 48 : -1);
}
