/* composer.c – Composer-Fenster: Neu, Antworten, Allen antworten (Prototyp #337, #359)
 *
 * Vorlage: quelltext/stamp/src/views/mail/composer/stamp-composer.c (AdwApplicationWindow als
 * Template-Klasse, Aktionsgruppe „composer“, Typen NEW/REPLY/REPLY_ALL, WebView als Editor mit
 * Formatierungsbefehlen 231-240, Nachricht als multipart/alternative 243-349). Verhalten: Evolution
 * mail/em-composer-utils.c (Empfänger, Betreff „Re:“, In-Reply-To/References, Zuschreibung, Zitat).
 * Oberfläche in composer.blp.
 *
 * Ablauf: Ein Worker-GTask (Architektur 1) holt Absender (Identität des Kontos) und bei Antworten die
 * Originalnachricht (camel_folder_get_message_sync), berechnet Empfänger, Betreff und Kopfzeilen und
 * baut das Zitat; im UI-Thread füllt das Ergebnis Felder und Editor. Senden ist #362; bis dahin ist
 * composer.send aus, außer im Test (REVOLUTION_TEST_HOOKS=1 und REVOLUTION_TEST_MIME=<Datei>): dann
 * baut ein Worker die CamelMimeMessage und schreibt sie in die Datei, statt zu senden.
 *
 * Editor: WebKitGTK 6.0, editierbar (webkit_web_view_set_editable wie Stamp stamp-composer.c:2324).
 * Zitierte Mails sind fremdes HTML: JavaScript im Markup aus (enable-javascript-markup), Netz gesperrt
 * per Content-Security-Policy im eigenen Dokument (nur data: und Inline-Stile), keine Navigation nach
 * dem ersten Laden (decide-policy). JavaScript läuft nur über die API (Inhalt holen, Formatierung).
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <string.h>

#include <glib/gi18n.h>
#include <webkit/webkit.h>

#include "composer.h"
#include "mail/mail-reader.h"

struct _RevolutionComposer
{
  AdwApplicationWindow parent_instance;

  AdwToastOverlay *toast_overlay;
  AdwWindowTitle  *window_title;
  GtkWidget       *format_button;
  GtkWidget       *to_entry;
  GtkWidget       *cc_entry;
  GtkWidget       *bcc_entry;
  GtkWidget       *subject_entry;
  GtkLabel        *from_value;
  AdwBin          *editor_bin;
  GtkDropDown     *paragraph_dropdown;
  WebKitWebView   *editor;
  GtkWidget       *emoji_chooser;
  GSimpleActionGroup *actions;
  GCancellable    *cancellable;
  gboolean         editor_loaded;
  gboolean         updating_state;
  gboolean         focus_editor;
  char            *from_name;
  char            *from_address;
  char            *in_reply_to;
  char            *references;
};

G_DEFINE_FINAL_TYPE (RevolutionComposer, revolution_composer, ADW_TYPE_APPLICATION_WINDOW)

/* Zitat wie Evolution (data/webkit/e-editor.js:24, BLOCKQUOTE_STYLE). */
#define CITE_STYLE "margin:0 0 0 .8ex; border-left:2px #729fcf solid;padding-left:1ex"

/* Eigenes Dokument des Editors. CSP: keine Anfrage ins Netz (auch nicht aus zitiertem HTML), Bilder nur
 * data:, Stile inline wie in Mails üblich. a11y: Titel und aria-label „Nachrichtentext“ (AT-SPI zeigt den
 * Inhalt der WebView als Dokument mit diesem Namen; ein GTK-Label an der WebView erschien dort nicht,
 * gemessen build/m5-359-probe/m5-359-a11y.txt); gesendet
 * wird nur der Inhalt von body (GET_CONTENT_JS). */
static const char EDITOR_HEAD[] =
  "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
  "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src data:; style-src 'unsafe-inline'\">"
  "<style>body { margin: 24px 28px; overflow-wrap: anywhere; } body:focus { outline: none; }</style>"
  "<title>%s</title></head><body aria-label=\"%s\">";

/* Inhalt holen: HTML des Körpers und Text für text/plain. Zitate (blockquote) werden wie in Evolution zu
 * Zeilen mit „> “ (je Ebene eines), über Platzhalter, damit innerText die Zeilenumbrüche des Layouts behält. */
static const char GET_CONTENT_JS[] =
  "(function () {"
  "  function plain (el) {"
  "    var top = el.tagName === 'BLOCKQUOTE' ? el : null;"
  "    var quotes = Array.prototype.filter.call (el.querySelectorAll ('blockquote'),"
  "      function (q) { return q.parentElement.closest ('blockquote') === top; });"
  "    var marks = quotes.map (function (q, i) {"
  "      var m = document.createElement ('div'); m.textContent = '\\u0001' + i + '\\u0001'; q.replaceWith (m); return m; });"
  "    var text = el.innerText;"
  "    marks.forEach (function (m, i) { m.replaceWith (quotes[i]); });"
  "    return text.replace (/\\u0001(\\d+)\\u0001/g, function (s, i) {"
  "      return plain (quotes[i]).replace (/\\n+$/, '').split ('\\n').map (function (l) { return '> ' + l; }).join ('\\n'); });"
  "  }"
  "  return [document.body.innerHTML, plain (document.body)];"
  "}) ()";

/* Zustand der Formatierung an die App (Knöpfe, Absatzformat), wie Stamp update_actions (210-219). */
static const char STATE_JS[] =
  "window.revolutionState = function () {"
  "  var on = ['bold', 'italic', 'underline', 'strikeThrough', 'insertUnorderedList', 'insertOrderedList']"
  "    .filter (function (c) { return document.queryCommandState (c); });"
  "  on.push ('block:' + document.queryCommandValue ('formatBlock'));"
  "  window.webkit.messageHandlers.state.postMessage (on.join (' '));"
  "};"
  "document.addEventListener ('selectionchange', window.revolutionState);"
  "document.addEventListener ('input', window.revolutionState);";

/* Schreibmarke an den Anfang (Antwort oben wie Evolution, composer-reply-start-bottom false,
 * org.gnome.evolution.mail.gschema.xml.in:132-133). */
static const char CARET_START_JS[] =
  "(function () { var r = document.createRange (); r.setStart (document.body, 0); r.collapse (true);"
  " var s = getSelection (); s.removeAllRanges (); s.addRange (r); }) ()";

static const char CARET_RECT_JS[] =
  "(function () { var s = getSelection (); if (!s.rangeCount) return [0, 0];"
  " var r = s.getRangeAt (0).getBoundingClientRect ();"
  " if (!r.height) { var n = s.focusNode; if (n && n.nodeType === 3) n = n.parentElement;"
  "   if (n && n.getBoundingClientRect) r = n.getBoundingClientRect (); }"
  " return [Math.round (r.left), Math.round (r.bottom)]; }) ()";

/* Formatierung: Aktion → Editor-Befehl (WebCore) und Name in queryCommandState. */
static const struct
{
  const char *action;
  const char *command;
  const char *state;
} FORMATS[] = {
  { "bold", "Bold", "bold" },
  { "italic", "Italic", "italic" },
  { "underline", "Underline", "underline" },
  { "strikethrough", "Strikethrough", "strikeThrough" },
  { "bullet-list", "InsertUnorderedList", "insertUnorderedList" },
  { "numbered-list", "InsertOrderedList", "insertOrderedList" },
};

/* Absatzformat (composer.blp, Liste „Text, Überschrift 1–3“). */
static const char * const BLOCKS[] = { "p", "h1", "h2", "h3" };

static void
show_toast (RevolutionComposer *self,
            const char         *title)
{
  adw_toast_overlay_add_toast (self->toast_overlay, adw_toast_new (title));
}

/* ------------------------------------------------------------------ Antwort (Worker) */

/* Übernommen aus Evolution 3.56.2 src/mail/em-utils.c:1575-1625 (check_prefix) und 1630-1680
 * (em_utils_is_re_in_subject): Präfix „Re“ und lokalisiert (1661-1670, deutsch „AW“ wie Evolution
 * po/de.po:20408-20411), je mit „:“ bzw. „︰“ (Vorgaben composer-localized-re und
 * composer-localized-re-separators leer). Lizenz: LGPL (Evolution COPYING:1-4), Weitergabe unter
 * GPL-3.0-or-later. */
static gsize
check_prefix (const char *subject,
              const char *prefix)
{
  gsize len = strlen (prefix);

  if (strlen (subject) < 3 || g_ascii_strncasecmp (subject, prefix, len) != 0)
    return 0;

  if (g_ascii_isspace (subject[len]))
    len++;
  if (subject[len] == ':')
    len++;
  else if (g_str_has_prefix (subject + len, "︰"))
    len += strlen ("︰");
  else
    return 0;
  if (g_ascii_isspace (subject[len]))
    len++;

  return len;
}

static gsize
re_prefix_length (const char *subject)
{
  gsize len = check_prefix (subject, "Re");

  /* Translators: lokalisiertes „Re“ im Betreff einer Antwort (Evolution: „AW“). */
  return len > 0 ? len : check_prefix (subject, C_("reply-attribution", "AW"));
}

/* Übernommen aus Evolution em-composer-utils.c:3210-3250 (emcu_construct_reply_subject): vorhandenes
 * „Re:“ entfernen, unlokalisiert „Re: “ davor (Vorgabe composer-use-localized-fwd-re false,
 * org.gnome.evolution.mail.gschema.xml.in:270-271). */
static char *
reply_subject (const char *subject)
{
  if (subject == NULL)
    return g_strdup ("");

  return g_strdup_printf ("Re: %s", subject + re_prefix_length (subject));
}

/* Übernommen aus Evolution em-composer-utils.c:3469-3545 (get_reply_to, get_reply_sender), ohne
 * Newsgroups (kein NNTP) und ohne composer-ignore-list-reply-to (Vorgabe false,
 * org.gnome.evolution.mail.gschema.xml.in:255-256): Mail-Reply-To, sonst Reply-To, sonst From. */
static void
reply_sender (CamelMimeMessage     *message,
              CamelInternetAddress *to)
{
  const char *mail_reply_to = camel_medium_get_header (CAMEL_MEDIUM (message), "Mail-Reply-To");
  g_autoptr (CamelInternetAddress) reply_to = NULL;
  const char *name, *addr;

  if (mail_reply_to != NULL && *mail_reply_to != '\0')
    {
      reply_to = camel_internet_address_new ();
      if (camel_address_decode (CAMEL_ADDRESS (reply_to), mail_reply_to) <= 0)
        g_clear_object (&reply_to);
    }
  if (reply_to == NULL && camel_mime_message_get_reply_to (message) != NULL)
    reply_to = g_object_ref (camel_mime_message_get_reply_to (message));
  if (reply_to == NULL && camel_mime_message_get_from (message) != NULL)
    reply_to = g_object_ref (camel_mime_message_get_from (message));

  for (int i = 0; reply_to != NULL && camel_internet_address_get (reply_to, i, &name, &addr); i++)
    camel_internet_address_add (to, name, addr);
}

/* Übernommen aus Evolution em-composer-utils.c:3669-3734 (generate_recipient_hash): Adressen und Aliase
 * aller eingeschalteten Mail-Identitäten. */
static GHashTable *
own_addresses (ESourceRegistry *registry)
{
  GHashTable *own = g_hash_table_new_full (camel_strcase_hash, camel_strcase_equal, g_free, NULL);
  g_autolist (ESource) identities = e_source_registry_list_sources (registry, E_SOURCE_EXTENSION_MAIL_IDENTITY);

  for (GList *l = identities; l != NULL; l = l->next)
    {
      ESourceMailIdentity *extension = e_source_get_extension (l->data, E_SOURCE_EXTENSION_MAIL_IDENTITY);
      g_autofree char *address = NULL;
      GHashTable *aliases;

      if (!e_source_registry_check_enabled (registry, l->data))
        continue;

      address = e_source_mail_identity_dup_address (extension);
      if (address != NULL && *address != '\0')
        g_hash_table_add (own, g_steal_pointer (&address));

      aliases = e_source_mail_identity_get_aliases_as_hash_table (extension);
      if (aliases != NULL)
        {
          GHashTableIter iter;
          gpointer key;

          g_hash_table_iter_init (&iter, aliases);
          while (g_hash_table_iter_next (&iter, &key, NULL))
            g_hash_table_add (own, g_strdup (key));
          g_hash_table_destroy (aliases);
        }
    }

  return own;
}

/* Übernommen aus Evolution em-composer-utils.c:3622-3636 (concat_unique_addrs). */
static void
add_unique (CamelInternetAddress *dest,
            CamelInternetAddress *src,
            GHashTable           *seen)
{
  const char *name, *addr;

  for (int i = 0; src != NULL && camel_internet_address_get (src, i, &name, &addr); i++)
    if (!g_hash_table_contains (seen, addr))
      {
        camel_internet_address_add (dest, name, addr);
        g_hash_table_add (seen, g_strdup (addr));
      }
}

/* Übernommen aus Evolution em-composer-utils.c:3736-3838 (em_utils_get_reply_all), ohne Newsgroups:
 * Mail-Followup-To, sonst Reply-To/From + To nach An und Cc nach Kopie, ohne eigene Adressen und
 * Doppelte; leeres An wird aus Reply-To, Kopie bzw. dem ursprünglichen An aufgefüllt. */
static void
reply_all (ESourceRegistry      *registry,
           CamelMimeMessage     *message,
           CamelInternetAddress *to,
           CamelInternetAddress *cc)
{
  const char *followup = camel_medium_get_header (CAMEL_MEDIUM (message), "Mail-Followup-To");
  g_autoptr (GHashTable) seen = own_addresses (registry);
  g_autoptr (CamelInternetAddress) to_addrs = NULL;
  CamelInternetAddress *reply_to = NULL, *cc_addrs = NULL;
  const char *name, *addr;

  if (followup != NULL && *followup != '\0')
    {
      to_addrs = camel_internet_address_new ();
      if (camel_address_decode (CAMEL_ADDRESS (to_addrs), followup) <= 0)
        g_clear_object (&to_addrs);
    }

  if (to_addrs == NULL)
    {
      reply_to = camel_mime_message_get_reply_to (message);
      if (reply_to == NULL)
        reply_to = camel_mime_message_get_from (message);
      to_addrs = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_TO);
      cc_addrs = camel_mime_message_get_recipients (message, CAMEL_RECIPIENT_TYPE_CC);
      if (to_addrs != NULL)
        g_object_ref (to_addrs);
    }

  for (int i = 0; reply_to != NULL && camel_internet_address_get (reply_to, i, &name, &addr); i++)
    if (addr != NULL && !g_hash_table_contains (seen, addr))
      {
        camel_internet_address_add (to, name, addr);
        g_hash_table_add (seen, g_strdup (addr));
      }

  add_unique (to, to_addrs, seen);
  add_unique (cc, cc_addrs, seen);

  if (reply_to != NULL && camel_address_length (CAMEL_ADDRESS (to)) == 0 &&
      camel_internet_address_get (reply_to, 0, &name, &addr))
    camel_internet_address_add (to, name, addr);

  if (camel_address_length (CAMEL_ADDRESS (to)) == 0 && camel_address_length (CAMEL_ADDRESS (cc)) > 0)
    {
      if (camel_internet_address_get (cc, 0, &name, &addr))
        camel_internet_address_add (to, name, addr);
      camel_address_remove (CAMEL_ADDRESS (cc), 0);
    }

  if (camel_address_length (CAMEL_ADDRESS (to)) == 0 &&
      ((to_addrs != NULL && camel_internet_address_get (to_addrs, 0, &name, &addr)) ||
       (cc_addrs != NULL && camel_internet_address_get (cc_addrs, 0, &name, &addr))))
    camel_internet_address_add (to, name, addr);
}

/* Übernommen aus Evolution em-composer-utils.c:3940-3995 (em_composer_utils_get_reply_credits) und
 * 3861-3907 (format_sender) mit der deutschen Zuschreibung aus Evolution po/de.po:20390-20395; Vorgaben
 * composer-reply-credits-(utc-)to-localtime false (gschema.xml.in:310-319): Uhrzeit in der Zeitzone
 * der Mail. */
static char *
reply_credits (CamelMimeMessage *message)
{
  CamelInternetAddress *from = camel_mime_message_get_from (message);
  const char *name = NULL, *addr = NULL;
  g_autofree char *unquoted = NULL;
  g_autoptr (GDateTime) when = NULL;
  g_autofree char *weekday = NULL;
  int tzone = 0;
  gint64 date;

  date = camel_mime_message_get_date (message, &tzone);
  if (date == CAMEL_MESSAGE_DATE_CURRENT)
    date = camel_mime_message_get_date_received (message, &tzone);
  if (date == CAMEL_MESSAGE_DATE_CURRENT)
    {
      date = g_get_real_time () / G_USEC_PER_SEC;
      tzone = 0;
    }
  date += (tzone / 100) * 60 * 60 + (tzone % 100) * 60;
  when = g_date_time_new_from_unix_utc (date);
  weekday = g_date_time_format (when, "%A");

  if (from != NULL && camel_internet_address_get (from, 0, &name, &addr))
    {
      gsize len = name != NULL ? strlen (name) : 0;

      if (len >= 2 && name[0] == '"' && name[len - 1] == '"')
        name = unquoted = g_strndup (name + 1, len - 2);
    }
  if (name == NULL || *name == '\0' || g_strcmp0 (name, "\"") == 0)
    name = addr != NULL ? addr : _("einem unbekannten Absender");

  /* Translators: Zuschreibung über dem Zitat wie Evolution (po/de.po:20394-20395). */
  return g_strdup_printf (_("Am %s, dem %02d.%02d.%04d um %02d:%02d %+05d schrieb %s:"), weekday,
                          g_date_time_get_day_of_month (when), g_date_time_get_month (when),
                          g_date_time_get_year (when), g_date_time_get_hour (when),
                          g_date_time_get_minute (when), tzone, name);
}

typedef struct
{
  ESourceRegistry *registry;
  CamelSession    *session;
  RevolutionComposerType type;
  char            *account_uid;
  char            *store_uid;
  char            *folder_name;
  char            *message_uid;
  /* Ergebnis */
  char            *from_name;
  char            *from_address;
  char            *to;
  char            *cc;
  char            *subject;
  char            *in_reply_to;
  char            *references;
  char            *body;
  GError          *error;
} Prepare;

static void
prepare_free (Prepare *prepare)
{
  g_clear_object (&prepare->registry);
  g_clear_object (&prepare->session);
  g_free (prepare->account_uid);
  g_free (prepare->store_uid);
  g_free (prepare->folder_name);
  g_free (prepare->message_uid);
  g_free (prepare->from_name);
  g_free (prepare->from_address);
  g_free (prepare->to);
  g_free (prepare->cc);
  g_free (prepare->subject);
  g_free (prepare->in_reply_to);
  g_free (prepare->references);
  g_free (prepare->body);
  g_clear_error (&prepare->error);
  g_free (prepare);
}

/* Identität des Kontos (Mail Account → identity-uid), sonst die Standard-Identität. Auswahl der
 * Identität folgt mit #360 (stamp-composer-from.c). */
static ESource *
ref_identity (ESourceRegistry *registry,
              const char      *account_uid)
{
  g_autoptr (ESource) account = account_uid != NULL ? e_source_registry_ref_source (registry, account_uid) : NULL;

  if (account != NULL && e_source_has_extension (account, E_SOURCE_EXTENSION_MAIL_ACCOUNT))
    {
      g_autofree char *identity_uid = e_source_mail_account_dup_identity_uid (
        e_source_get_extension (account, E_SOURCE_EXTENSION_MAIL_ACCOUNT));
      g_autoptr (ESource) identity = identity_uid != NULL ? e_source_registry_ref_source (registry, identity_uid) : NULL;

      if (identity != NULL && e_source_has_extension (identity, E_SOURCE_EXTENSION_MAIL_IDENTITY))
        return g_steal_pointer (&identity);
    }

  return e_source_registry_ref_default_mail_identity (registry);
}

/* Antwort wie Evolution em_utils_reply_to_message (em-composer-utils.c:5063-5216): Empfänger
 * (em_utils_get_reply_recipients 5009-5040), Betreff und Kopfzeilen (reply_setup_composer 3326-3391),
 * Zuschreibung und Zitat (composer_set_body 4144-4158, Stil „quoted“). */
static void
prepare_reply (Prepare          *prepare,
               CamelMimeMessage *message,
               GCancellable     *cancellable)
{
  g_autoptr (CamelInternetAddress) to = camel_internet_address_new ();
  g_autoptr (CamelInternetAddress) cc = camel_internet_address_new ();
  g_autofree char *message_id = NULL, *references = NULL, *credits = NULL, *quote = NULL, *escaped = NULL;
  const char *sender = NULL;

  /* Antwort auf eine eigene Nachricht geht an alle wie Evolution (em-composer-utils.c:5127-5131,
   * em_utils_sender_is_user e-mail-utils.c:1011-1029: Absender ist Adresse oder Alias einer eingeschalteten
   * Mail-Identität); Gesendet-Ordner (5149-5151) prüft mail-view.c open_reply. */
  if (prepare->type == REVOLUTION_COMPOSER_REPLY && camel_mime_message_get_from (message) != NULL &&
      camel_internet_address_get (camel_mime_message_get_from (message), 0, NULL, &sender) && sender != NULL)
    {
      g_autoptr (GHashTable) own = own_addresses (prepare->registry);

      if (g_hash_table_contains (own, sender))
        {
          g_debug ("Composer: Absender %s ist eigene Adresse, Allen antworten", sender);
          prepare->type = REVOLUTION_COMPOSER_REPLY_ALL;
        }
    }

  if (prepare->type == REVOLUTION_COMPOSER_REPLY_ALL)
    reply_all (prepare->registry, message, to, cc);
  else
    reply_sender (message, to);
  prepare->to = camel_address_format (CAMEL_ADDRESS (to));
  prepare->cc = camel_address_format (CAMEL_ADDRESS (cc));
  prepare->subject = reply_subject (camel_mime_message_get_subject (message));

  /* In-Reply-To und References: em-composer-utils.c:3363-3391. */
  message_id = camel_header_unfold (camel_medium_get_header (CAMEL_MEDIUM (message), "Message-ID"));
  references = camel_header_unfold (camel_medium_get_header (CAMEL_MEDIUM (message), "References"));
  if (message_id != NULL)
    {
      prepare->in_reply_to = g_strdup (message_id);
      prepare->references = references != NULL ? g_strdup_printf ("%s %s", references, message_id)
                                                : g_strdup (message_id);
    }
  else
    prepare->references = g_steal_pointer (&references);

  credits = reply_credits (message);
  escaped = g_markup_escape_text (credits, -1);
  quote = revolution_mail_reader_message_html (message, cancellable);
  prepare->body = g_strdup_printf ("<div><br></div><div>%s</div><blockquote type=\"cite\" style=\"" CITE_STYLE "\">%s"
                                   "</blockquote>", escaped, quote);
}

static void
prepare_thread (GTask        *task,
                gpointer      source_object G_GNUC_UNUSED,
                gpointer      task_data,
                GCancellable *cancellable)
{
  Prepare *prepare = task_data;
  g_autoptr (ESource) identity = NULL;
  g_autoptr (CamelService) service = NULL;
  g_autoptr (CamelFolder) folder = NULL;
  g_autoptr (CamelMimeMessage) message = NULL;

  identity = prepare->registry != NULL ? ref_identity (prepare->registry, prepare->account_uid) : NULL;
  if (identity != NULL)
    {
      ESourceMailIdentity *extension = e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_IDENTITY);

      prepare->from_name = e_source_mail_identity_dup_name (extension);
      prepare->from_address = e_source_mail_identity_dup_address (extension);
    }

  if (prepare->type != REVOLUTION_COMPOSER_NEW)
    {
      service = camel_session_ref_service (prepare->session, prepare->store_uid);
      if (!CAMEL_IS_STORE (service))
        g_set_error (&prepare->error, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE,
                     _("Konto %s nicht verfügbar"), prepare->store_uid);
      else
        folder = camel_store_get_folder_sync (CAMEL_STORE (service), prepare->folder_name, 0, cancellable,
                                              &prepare->error);
      if (folder != NULL)
        message = camel_folder_get_message_sync (folder, prepare->message_uid, cancellable, &prepare->error);
      if (message != NULL)
        prepare_reply (prepare, message, cancellable);
    }

  g_task_return_boolean (task, TRUE);
}

/* ------------------------------------------------------------------ Editor */

static void
run_js (RevolutionComposer *self,
        const char         *script)
{
  webkit_web_view_evaluate_javascript (self->editor, script, -1, NULL, NULL, NULL, NULL, NULL);
}

/* Schriftfarbe der Karte (style.css .composer-card, Themes #340) ins Dokument. */
static void
update_editor_color (RevolutionComposer *self)
{
  GdkRGBA color;
  g_autofree char *script = NULL;

  if (!self->editor_loaded)
    return;

  gtk_widget_get_color (GTK_WIDGET (self->editor), &color);
  script = g_strdup_printf ("document.documentElement.style.color = 'rgb(%d, %d, %d)';", (int) (color.red * 255),
                            (int) (color.green * 255), (int) (color.blue * 255));
  run_js (self, script);
}

static void
load_editor (RevolutionComposer *self,
             const char         *body)
{
  g_autofree char *label = g_markup_escape_text (_("Nachrichtentext"), -1);
  g_autofree char *head = g_strdup_printf (EDITOR_HEAD, label, label);
  g_autofree char *html = g_strconcat (head, body != NULL ? body : "<div><br></div>", "</body></html>", NULL);

  self->editor_loaded = FALSE;
  webkit_web_view_load_html (self->editor, html, NULL);
}

static void
on_load_changed (RevolutionComposer *self,
                 WebKitLoadEvent     event)
{
  if (event != WEBKIT_LOAD_FINISHED)
    return;

  self->editor_loaded = TRUE;
  run_js (self, STATE_JS);
  update_editor_color (self);
  if (self->focus_editor)
    {
      run_js (self, CARET_START_JS);
      gtk_widget_grab_focus (GTK_WIDGET (self->editor));
    }
}

/* Nur das erste Laden (load_editor); danach keine Navigation (Links in zitierten Mails, Meta-Refresh). */
static gboolean
on_decide_policy (RevolutionComposer      *self,
                  WebKitPolicyDecision    *decision,
                  WebKitPolicyDecisionType type)
{
  if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE || !self->editor_loaded)
    return FALSE;

  webkit_policy_decision_ignore (decision);
  return TRUE;
}

static void
on_state_message (RevolutionComposer *self,
                  JSCValue           *value)
{
  g_autofree char *text = jsc_value_to_string (value);
  g_auto (GStrv) on = g_strsplit (text != NULL ? text : "", " ", -1);

  self->updating_state = TRUE;
  for (gsize i = 0; i < G_N_ELEMENTS (FORMATS); i++)
    g_simple_action_set_state (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self->actions), FORMATS[i].action)),
                               g_variant_new_boolean (g_strv_contains ((const char * const *) on, FORMATS[i].state)));
  /* Absatzformat: Überschrift 1–3, alles andere (p, div, leer) ist „Text“. */
  for (gsize i = 0; on[i] != NULL; i++)
    if (g_str_has_prefix (on[i], "block:"))
      {
        guint block = 0;

        for (guint b = 1; b < G_N_ELEMENTS (BLOCKS); b++)
          if (g_strcmp0 (on[i] + strlen ("block:"), BLOCKS[b]) == 0)
            block = b;
        gtk_drop_down_set_selected (self->paragraph_dropdown, block);
      }
  self->updating_state = FALSE;
}

static void
change_format (GSimpleAction *action,
               GVariant      *value,
               gpointer       user_data)
{
  RevolutionComposer *self = user_data;

  g_simple_action_set_state (action, value);
  if (self->updating_state)
    return;

  for (gsize i = 0; i < G_N_ELEMENTS (FORMATS); i++)
    if (g_strcmp0 (g_action_get_name (G_ACTION (action)), FORMATS[i].action) == 0)
      webkit_web_view_execute_editing_command (self->editor, FORMATS[i].command);
  run_js (self, "window.revolutionState && window.revolutionState ();");
}

static void
on_paragraph_selected (RevolutionComposer *self)
{
  guint selected = gtk_drop_down_get_selected (self->paragraph_dropdown);

  if (self->updating_state || selected >= G_N_ELEMENTS (BLOCKS))
    return;

  webkit_web_view_execute_editing_command_with_argument (self->editor, "FormatBlock", BLOCKS[selected]);
  gtk_widget_grab_focus (GTK_WIDGET (self->editor));
}

/* ------------------------------------------------------------------ Aktionen */

/* #402: Text-Mail blendet die Formatierungsleiste aus (Knopf „Formatierung anzeigen“
 * aus und gesperrt), HTML-Mail zeigt sie wieder. #359: Text-Mail sendet nur text/plain. */
static void
change_mail_format (GSimpleAction *action,
                    GVariant      *value,
                    gpointer       user_data)
{
  RevolutionComposer *self = user_data;
  gboolean html = g_strcmp0 (g_variant_get_string (value, NULL), "html") == 0;

  g_simple_action_set_state (action, value);
  gtk_widget_set_sensitive (self->format_button, html);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->format_button), html);
}

/* #402: Kopie/Blindkopie getrennt ein-/ausblenden (Hauptmenü, Haken = sichtbar);
 * Beschriftung und Trennlinie folgen per Bindung (composer.blp). */
static void
change_show_cc (GSimpleAction *action,
                GVariant      *value,
                gpointer       user_data)
{
  RevolutionComposer *self = user_data;

  g_simple_action_set_state (action, value);
  gtk_widget_set_visible (self->cc_entry, g_variant_get_boolean (value));
}

static void
change_show_bcc (GSimpleAction *action,
                 GVariant      *value,
                 gpointer       user_data)
{
  RevolutionComposer *self = user_data;

  g_simple_action_set_state (action, value);
  gtk_widget_set_visible (self->bcc_entry, g_variant_get_boolean (value));
}

/* #402: Emoji-Auswahl (GtkEmojiChooser) an der Schreibmarke wie Stamp show_emoji_chooser
 * (stamp-composer.c:2226-2300), Strg+. (composer.blp). */
static void
on_emoji_picked (RevolutionComposer *self,
                 const char         *text)
{
  webkit_web_view_execute_editing_command_with_argument (self->editor, "InsertText", text);
  gtk_widget_grab_focus (GTK_WIDGET (self->editor));
}

static void
on_caret_rect (GObject      *source,
               GAsyncResult *result,
               gpointer      user_data)
{
  g_autoptr (RevolutionComposer) self = user_data;
  g_autoptr (JSCValue) value = webkit_web_view_evaluate_javascript_finish (WEBKIT_WEB_VIEW (source), result, NULL);
  GdkRectangle rect = { 0, 0, 1, 1 };

  if (g_cancellable_is_cancelled (self->cancellable))
    return;
  if (value != NULL && jsc_value_is_array (value))
    {
      g_autoptr (JSCValue) x = jsc_value_object_get_property_at_index (value, 0);
      g_autoptr (JSCValue) y = jsc_value_object_get_property_at_index (value, 1);

      rect.x = jsc_value_to_int32 (x);
      rect.y = jsc_value_to_int32 (y);
    }
  gtk_popover_set_pointing_to (GTK_POPOVER (self->emoji_chooser), &rect);
  gtk_popover_popup (GTK_POPOVER (self->emoji_chooser));
}

static void
insert_emoji (GSimpleAction *action G_GNUC_UNUSED,
              GVariant      *parameter G_GNUC_UNUSED,
              gpointer       user_data)
{
  RevolutionComposer *self = user_data;

  /* Erst bei Bedarf (Strg+., Hauptmenü). */
  if (self->emoji_chooser == NULL)
    {
      self->emoji_chooser = gtk_emoji_chooser_new ();
      gtk_widget_set_parent (self->emoji_chooser, GTK_WIDGET (self->editor));
      g_signal_connect_object (self->emoji_chooser, "emoji-picked", G_CALLBACK (on_emoji_picked), self,
                               G_CONNECT_SWAPPED);
    }
  webkit_web_view_evaluate_javascript (self->editor, CARET_RECT_JS, -1, NULL, NULL, self->cancellable, on_caret_rect,
                                       g_object_ref (self));
}

typedef struct
{
  char     *from_name;
  char     *from_address;
  char     *to;
  char     *cc;
  char     *bcc;
  char     *subject;
  char     *in_reply_to;
  char     *references;
  char     *html;
  char     *plain;
  char     *path;
  gboolean  is_html;
} Build;

static void
build_free (Build *build)
{
  g_free (build->from_name);
  g_free (build->from_address);
  g_free (build->to);
  g_free (build->cc);
  g_free (build->bcc);
  g_free (build->subject);
  g_free (build->in_reply_to);
  g_free (build->references);
  g_free (build->html);
  g_free (build->plain);
  g_free (build->path);
  g_free (build);
}

static CamelMimePart *
text_part (const char *text,
           const char *type)
{
  CamelMimePart *part = camel_mime_part_new ();

  camel_mime_part_set_content (part, text, strlen (text), type);
  camel_mime_part_set_encoding (part, CAMEL_TRANSFER_ENCODING_QUOTEDPRINTABLE);

  return part;
}

/* Nachricht wie Stamp build_message (stamp-composer.c:243-349) und on_get_body_html (754-770, Message-ID
 * mit der Domain des Absenders): multipart/alternative aus text/plain und text/html, bei Text-Mail nur
 * text/plain. Im Test in die Datei statt Senden (#362). */
static void
build_thread (GTask        *task,
              gpointer      source_object G_GNUC_UNUSED,
              gpointer      task_data,
              GCancellable *cancellable)
{
  Build *build = task_data;
  g_autoptr (CamelMimeMessage) message = camel_mime_message_new ();
  g_autoptr (CamelInternetAddress) from = camel_internet_address_new ();
  g_autoptr (GFile) file = g_file_new_for_path (build->path);
  g_autoptr (GFileOutputStream) stream = NULL;
  g_autofree char *message_id = NULL;
  const char *at = strchr (build->from_address, '@');
  const struct { const char *type; const char *text; } recipients[] = {
    { CAMEL_RECIPIENT_TYPE_TO, build->to },
    { CAMEL_RECIPIENT_TYPE_CC, build->cc },
    { CAMEL_RECIPIENT_TYPE_BCC, build->bcc },
  };
  GError *error = NULL;

  camel_internet_address_add (from, build->from_name, build->from_address);
  camel_mime_message_set_from (message, from);
  for (gsize i = 0; i < G_N_ELEMENTS (recipients); i++)
    {
      g_autoptr (CamelInternetAddress) address = camel_internet_address_new ();

      if (camel_address_unformat (CAMEL_ADDRESS (address), recipients[i].text) > 0)
        camel_mime_message_set_recipients (message, recipients[i].type, address);
    }
  camel_mime_message_set_subject (message, build->subject);
  camel_mime_message_set_date (message, CAMEL_MESSAGE_DATE_CURRENT, 0);
  message_id = camel_header_msgid_generate (at != NULL && at[1] != '\0' ? at + 1 : "localhost");
  camel_mime_message_set_message_id (message, message_id);
  if (build->in_reply_to != NULL)
    camel_medium_set_header (CAMEL_MEDIUM (message), "In-Reply-To", build->in_reply_to);
  if (build->references != NULL)
    camel_medium_set_header (CAMEL_MEDIUM (message), "References", build->references);

  if (build->is_html)
    {
      g_autoptr (CamelMultipart) body = camel_multipart_new ();
      g_autoptr (CamelMimePart) plain = text_part (build->plain, "text/plain; charset=utf-8");
      g_autoptr (CamelMimePart) html = text_part (build->html, "text/html; charset=utf-8");

      camel_data_wrapper_set_mime_type (CAMEL_DATA_WRAPPER (body), "multipart/alternative");
      camel_multipart_set_boundary (body, NULL);
      camel_multipart_add_part (body, plain);
      camel_multipart_add_part (body, html);
      camel_medium_set_content (CAMEL_MEDIUM (message), CAMEL_DATA_WRAPPER (body));
    }
  else
    {
      camel_mime_part_set_content (CAMEL_MIME_PART (message), build->plain, strlen (build->plain),
                                   "text/plain; charset=utf-8");
      camel_mime_part_set_encoding (CAMEL_MIME_PART (message), CAMEL_TRANSFER_ENCODING_QUOTEDPRINTABLE);
    }

  stream = g_file_replace (file, NULL, FALSE, G_FILE_CREATE_NONE, cancellable, &error);
  if (stream == NULL ||
      camel_data_wrapper_write_to_output_stream_sync (CAMEL_DATA_WRAPPER (message), G_OUTPUT_STREAM (stream),
                                                      cancellable, &error) < 0 ||
      !g_output_stream_close (G_OUTPUT_STREAM (stream), cancellable, &error))
    {
      g_task_return_error (task, error);
      return;
    }

  g_task_return_boolean (task, TRUE);
}

static void
on_built (GObject      *source_object,
          GAsyncResult *result,
          gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionComposer *self = REVOLUTION_COMPOSER (source_object);
  Build *build = g_task_get_task_data (G_TASK (result));
  g_autoptr (GError) error = NULL;

  if (!g_task_propagate_boolean (G_TASK (result), &error))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;
      g_warning ("Composer: MIME nicht geschrieben: %s", error->message);
      show_toast (self, _("Nachricht konnte nicht erstellt werden"));
      g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self->actions), "send")), TRUE);
      return;
    }

  g_debug ("Composer: MIME geschrieben %s", build->path);
  gtk_window_destroy (GTK_WINDOW (self));
}

static void
on_content (GObject      *source,
            GAsyncResult *result,
            gpointer      user_data)
{
  g_autoptr (RevolutionComposer) self = user_data;
  g_autoptr (GError) error = NULL;
  g_autoptr (JSCValue) value = webkit_web_view_evaluate_javascript_finish (WEBKIT_WEB_VIEW (source), result, &error);
  g_autoptr (JSCValue) html = NULL, plain = NULL;
  g_autoptr (GVariant) format = NULL;
  g_autoptr (GTask) task = NULL;
  g_autofree char *body = NULL;
  Build *build;

  if (value == NULL || !jsc_value_is_array (value))
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          g_warning ("Composer: Inhalt nicht lesbar: %s", error != NULL ? error->message : "kein Feld");
          show_toast (self, _("Nachricht konnte nicht erstellt werden"));
          g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self->actions), "send")),
                                       TRUE);
        }
      return;
    }

  html = jsc_value_object_get_property_at_index (value, 0);
  plain = jsc_value_object_get_property_at_index (value, 1);
  body = jsc_value_to_string (html);
  format = g_action_group_get_action_state (G_ACTION_GROUP (self->actions), "mail-format");

  build = g_new0 (Build, 1);
  build->from_name = g_strdup (self->from_name);
  build->from_address = g_strdup (self->from_address);
  build->to = g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->to_entry)));
  build->cc = g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->cc_entry)));
  build->bcc = g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->bcc_entry)));
  build->subject = g_strdup (gtk_editable_get_text (GTK_EDITABLE (self->subject_entry)));
  build->in_reply_to = g_strdup (self->in_reply_to);
  build->references = g_strdup (self->references);
  build->html = g_strconcat ("<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body>", body, "</body></html>",
                             NULL);
  build->plain = jsc_value_to_string (plain);
  build->path = g_strdup (g_getenv ("REVOLUTION_TEST_MIME"));
  build->is_html = g_strcmp0 (g_variant_get_string (format, NULL), "html") == 0;

  task = g_task_new (self, self->cancellable, on_built, NULL);
  g_task_set_source_tag (task, on_content);
  g_task_set_task_data (task, build, (GDestroyNotify) build_free);
  g_task_run_in_thread (task, build_thread);
}

/* Senden folgt mit #362; bis dahin nur im Test (send_enabled): Nachricht in die Datei. */
static void
send_message (GSimpleAction *action,
              GVariant      *parameter G_GNUC_UNUSED,
              gpointer       user_data)
{
  RevolutionComposer *self = user_data;

  if (!self->editor_loaded)
    return;

  /* Nur einmal: aus bis on_built (bei Fehler wieder an, sonst schließt das Fenster). */
  g_simple_action_set_enabled (action, FALSE);
  webkit_web_view_evaluate_javascript (self->editor, GET_CONTENT_JS, -1, NULL, NULL, self->cancellable, on_content,
                                       g_object_ref (self));
}

static const GActionEntry composer_actions[] = {
  { "show-cc", NULL, NULL, "true", change_show_cc, { 0 } },
  { "show-bcc", NULL, NULL, "true", change_show_bcc, { 0 } },
  { "mail-format", NULL, "s", "'html'", change_mail_format, { 0 } },
  { "insert-emoji", insert_emoji, NULL, NULL, NULL, { 0 } },
  { "send", send_message, NULL, NULL, NULL, { 0 } },
  { "bold", NULL, NULL, "false", change_format, { 0 } },
  { "italic", NULL, NULL, "false", change_format, { 0 } },
  { "underline", NULL, NULL, "false", change_format, { 0 } },
  { "strikethrough", NULL, NULL, "false", change_format, { 0 } },
  { "bullet-list", NULL, NULL, "false", change_format, { 0 } },
  { "numbered-list", NULL, NULL, "false", change_format, { 0 } },
};

/* ------------------------------------------------------------------ Fenster */

/* Briefing 1.3: Titel = Betreff bzw. „Neue E-Mail“ (stamp-composer.c:1846-1860). */
static void
on_subject_changed (RevolutionComposer *self)
{
  const char *subject = gtk_editable_get_text (GTK_EDITABLE (self->subject_entry));
  const char *title = *subject != '\0' ? subject : _("Neue E-Mail");

  adw_window_title_set_title (self->window_title, title);
  gtk_window_set_title (GTK_WINDOW (self), title);
}

static void
on_prepared (GObject      *source_object,
             GAsyncResult *result,
             gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionComposer *self = REVOLUTION_COMPOSER (source_object);
  Prepare *prepare = g_task_get_task_data (G_TASK (result));
  gboolean test_send;

  if (g_cancellable_is_cancelled (self->cancellable))
    return;

  if (prepare->error != NULL)
    {
      g_warning ("Composer: Original nicht geladen: %s", prepare->error->message);
      show_toast (self, _("Ursprüngliche Nachricht konnte nicht geladen werden"));
    }

  self->from_name = g_steal_pointer (&prepare->from_name);
  self->from_address = g_steal_pointer (&prepare->from_address);
  self->in_reply_to = g_steal_pointer (&prepare->in_reply_to);
  self->references = g_steal_pointer (&prepare->references);
  if (self->from_address != NULL)
    {
      g_autofree char *from = self->from_name != NULL && *self->from_name != '\0'
                              /* Translators: Absender im Composer wie der Prototyp „Name – Adresse“. */
                              ? g_strdup_printf (_("%s – %s"), self->from_name, self->from_address)
                              : g_strdup (self->from_address);

      gtk_label_set_label (self->from_value, from);
    }
  if (prepare->to != NULL)
    gtk_editable_set_text (GTK_EDITABLE (self->to_entry), prepare->to);
  if (prepare->cc != NULL)
    gtk_editable_set_text (GTK_EDITABLE (self->cc_entry), prepare->cc);
  if (prepare->subject != NULL)
    gtk_editable_set_text (GTK_EDITABLE (self->subject_entry), prepare->subject);

  self->focus_editor = prepare->type != REVOLUTION_COMPOSER_NEW && prepare->error == NULL;
  if (!self->focus_editor)
    gtk_widget_grab_focus (self->to_entry);
  load_editor (self, prepare->body);

  /* Nur Test (#359 bis #362): Senden schreibt die Nachricht in REVOLUTION_TEST_MIME. */
  test_send = g_strcmp0 (g_getenv ("REVOLUTION_TEST_HOOKS"), "1") == 0 && g_getenv ("REVOLUTION_TEST_MIME") != NULL;
  g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self->actions), "send")),
                               test_send && self->from_address != NULL);
  g_debug ("Composer: %s vorbereitet, Von %s", prepare->type == REVOLUTION_COMPOSER_NEW ? "Neu" :
           prepare->type == REVOLUTION_COMPOSER_REPLY ? "Antworten" : "Allen antworten",
           self->from_address != NULL ? self->from_address : "(keine Identität)");
}

/* #402: Klasse .dark für Regeln nur im dunklen Design (style.css, Abschnitt
 * „Composer (#402)“), wie mail-view.c on_dark_changed (#401 Punkt 1). */
static void
on_dark_changed (AdwStyleManager    *manager,
                 GParamSpec         *pspec G_GNUC_UNUSED,
                 RevolutionComposer *self)
{
  if (adw_style_manager_get_dark (manager))
    gtk_widget_add_css_class (GTK_WIDGET (self), "dark");
  else
    gtk_widget_remove_css_class (GTK_WIDGET (self), "dark");
  update_editor_color (self);
}

static WebKitWebView *
create_editor (RevolutionComposer *self)
{
  static WebKitNetworkSession *network_session;
  g_autofree char *font_name = NULL;
  g_autoptr (WebKitSettings) settings = NULL;
  PangoFontDescription *font;
  WebKitUserContentManager *manager;
  WebKitWebView *editor;
  GdkRGBA transparent = { 0, 0, 0, 0 };
  guint32 size = 15;

  /* Flüchtig wie der Lesebereich (mail-reader.c network_session): nichts auf der Platte (Architektur 4). */
  if (network_session == NULL)
    network_session = webkit_network_session_new_ephemeral ();

  /* Schrift aus GTK wie der Lesebereich (mail-reader.c create_settings, Evolution e-web-view.c:3440-3476). */
  g_object_get (gtk_settings_get_default (), "gtk-font-name", &font_name, NULL);
  font = pango_font_description_from_string (font_name != NULL ? font_name : "Sans 11");
  if (pango_font_description_get_size (font) > 0)
    size = webkit_settings_font_size_to_pixels ((guint32) (pango_font_description_get_size (font) / PANGO_SCALE));
  settings = webkit_settings_new_with_settings ("enable-javascript-markup", FALSE,
                                                "enable-html5-database", FALSE,
                                                "enable-html5-local-storage", FALSE,
                                                "enable-page-cache", FALSE,
                                                "enable-media", FALSE,
                                                "enable-webgl", FALSE,
                                                "enable-webrtc", FALSE,
                                                "allow-modal-dialogs", FALSE,
                                                "default-font-family", pango_font_description_get_family (font),
                                                "default-font-size", size,
                                                NULL);
  pango_font_description_free (font);
  /* Keine Vorab-Verbindungen aus zitiertem HTML (<link rel=preconnect …>): die CSP greift dort nicht (Prüfer #359
   * R1, build-pruef/probe-netz: 1 TCP-Verbindung); wie der Lesebereich. */
  revolution_mail_reader_disable_prefetch (settings);

  editor = g_object_new (WEBKIT_TYPE_WEB_VIEW, "network-session", network_session, "settings", settings, NULL);
  webkit_web_view_set_editable (editor, TRUE);
  webkit_web_view_set_background_color (editor, &transparent);
  gtk_widget_set_vexpand (GTK_WIDGET (editor), TRUE);

  manager = webkit_web_view_get_user_content_manager (editor);
  webkit_user_content_manager_register_script_message_handler (manager, "state", NULL);
  g_signal_connect_object (manager, "script-message-received::state", G_CALLBACK (on_state_message), self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (editor, "load-changed", G_CALLBACK (on_load_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (editor, "decide-policy", G_CALLBACK (on_decide_policy), self, G_CONNECT_SWAPPED);

  return editor;
}

static void
revolution_composer_dispose (GObject *object)
{
  RevolutionComposer *self = REVOLUTION_COMPOSER (object);

  g_cancellable_cancel (self->cancellable);
  /* Webprozess zuerst beenden wie der Lesebereich (mail-reader.c revolution_mail_reader_dispose): sonst meldet
   * er beim Schließen „Error releasing name …Sandboxed.WebProcess…“ (gemessen, build/m5-359-probe). */
  if (self->editor != NULL)
    webkit_web_view_terminate_web_process (self->editor);
  g_clear_pointer (&self->emoji_chooser, gtk_widget_unparent);
  gtk_widget_dispose_template (GTK_WIDGET (object), REVOLUTION_TYPE_COMPOSER);
  /* Die WebView gehörte editor_bin (Vorlage) und ist jetzt freigegeben. */
  self->editor = NULL;

  G_OBJECT_CLASS (revolution_composer_parent_class)->dispose (object);
}

static void
revolution_composer_finalize (GObject *object)
{
  RevolutionComposer *self = REVOLUTION_COMPOSER (object);

  g_clear_object (&self->cancellable);
  g_clear_object (&self->actions);
  g_free (self->from_name);
  g_free (self->from_address);
  g_free (self->in_reply_to);
  g_free (self->references);

  G_OBJECT_CLASS (revolution_composer_parent_class)->finalize (object);
}

static void
revolution_composer_class_init (RevolutionComposerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = revolution_composer_dispose;
  object_class->finalize = revolution_composer_finalize;
  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/composer.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionComposer, toast_overlay);
  gtk_widget_class_bind_template_child (widget_class, RevolutionComposer, window_title);
  gtk_widget_class_bind_template_child (widget_class, RevolutionComposer, format_button);
  gtk_widget_class_bind_template_child (widget_class, RevolutionComposer, to_entry);
  gtk_widget_class_bind_template_child (widget_class, RevolutionComposer, cc_entry);
  gtk_widget_class_bind_template_child (widget_class, RevolutionComposer, bcc_entry);
  gtk_widget_class_bind_template_child (widget_class, RevolutionComposer, subject_entry);
  gtk_widget_class_bind_template_child (widget_class, RevolutionComposer, from_value);
  gtk_widget_class_bind_template_child (widget_class, RevolutionComposer, editor_bin);
  gtk_widget_class_bind_template_child (widget_class, RevolutionComposer, paragraph_dropdown);
}

static void
revolution_composer_init (RevolutionComposer *self)
{
  AdwStyleManager *style_manager = adw_style_manager_get_default ();

  gtk_widget_init_template (GTK_WIDGET (self));
  self->cancellable = g_cancellable_new ();

  self->actions = g_simple_action_group_new ();
  g_action_map_add_action_entries (G_ACTION_MAP (self->actions), composer_actions, G_N_ELEMENTS (composer_actions), self);
  g_simple_action_set_enabled (G_SIMPLE_ACTION (g_action_map_lookup_action (G_ACTION_MAP (self->actions), "send")), FALSE);
  gtk_widget_insert_action_group (GTK_WIDGET (self), "composer", G_ACTION_GROUP (self->actions));

  self->editor = create_editor (self);
  adw_bin_set_child (self->editor_bin, GTK_WIDGET (self->editor));

  g_signal_connect_object (self->subject_entry, "changed", G_CALLBACK (on_subject_changed), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (self->paragraph_dropdown, "notify::selected", G_CALLBACK (on_paragraph_selected), self,
                           G_CONNECT_SWAPPED);
  /* Theme „Dunkel“ setzt .content-dark am Fenster (main.c apply_content_class): Schriftfarbe nachziehen. */
  g_signal_connect_object (self, "notify::css-classes", G_CALLBACK (update_editor_color), self, G_CONNECT_SWAPPED);
  g_signal_connect_object (style_manager, "notify::dark", G_CALLBACK (on_dark_changed), self, G_CONNECT_DEFAULT);
  on_dark_changed (style_manager, NULL, self);
}

/* Neue Mail (type NEW, Absender = Identität von account_uid) oder Antwort auf die Nachricht message_uid im
 * Ordner folder_name des Stores store_uid (bei Favoriten der Suchordner); account_uid = Konto des echten
 * Ordners. registry darf NULL sein (noch nicht geladen): dann ohne Absender und ohne Senden. */
GtkWindow *
revolution_composer_new (GtkApplication         *application,
                         ESourceRegistry        *registry,
                         CamelSession           *session,
                         RevolutionComposerType  type,
                         const char             *account_uid,
                         const char             *store_uid,
                         const char             *folder_name,
                         const char             *message_uid)
{
  RevolutionComposer *self;
  g_autoptr (GTask) task = NULL;
  Prepare *prepare;

  g_return_val_if_fail (type == REVOLUTION_COMPOSER_NEW || (CAMEL_IS_SESSION (session) && message_uid != NULL), NULL);

  self = g_object_new (REVOLUTION_TYPE_COMPOSER, "application", application, NULL);

  prepare = g_new0 (Prepare, 1);
  prepare->registry = registry != NULL ? g_object_ref (registry) : NULL;
  prepare->session = session != NULL ? g_object_ref (session) : NULL;
  prepare->type = type;
  prepare->account_uid = g_strdup (account_uid);
  prepare->store_uid = g_strdup (store_uid);
  prepare->folder_name = g_strdup (folder_name);
  prepare->message_uid = g_strdup (message_uid);

  task = g_task_new (self, self->cancellable, on_prepared, NULL);
  g_task_set_source_tag (task, revolution_composer_new);
  g_task_set_task_data (task, prepare, (GDestroyNotify) prepare_free);
  g_task_run_in_thread (task, prepare_thread);

  return GTK_WINDOW (self);
}
