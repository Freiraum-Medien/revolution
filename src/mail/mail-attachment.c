/* mail-attachment.c – Anhänge einer Nachricht: Eintrag und Anhangleiste (#356)
 *
 * RevolutionMailAttachment: ein Anhang (CamelMimePart) mit Anzeigename, Inhaltstyp, Symbol und
 * Größe. Angelegt im Worker des Lesebereichs (mail-reader.c, Zerlegen der MIME-Teile), weil die
 * Größe den Teil dekodiert (camel_data_wrapper_calculate_decoded_size_sync).
 *
 * RevolutionMailAttachmentBar: die Anhangleiste unter dem Nachrichtenkopf, Anhänge als Pillen
 * (Dokument #5 „Anhänge als Pillen“, Design/Prototyp/composer-hell.png) in einer waagrechten
 * GtkListView (Architektur 2). Öffnen (Doppelklick, Eingabe), Speichern unter …, Alle speichern …
 * über Kontextmenü (Rechtsklick, Menü-Taste, Umschalt+F10) und Knopf. Dekodieren und Schreiben
 * im Worker (Architektur 1); Öffnen über GtkFileLauncher aus einer Datei unter
 * ~/.cache/revolution/attachments, Speichern über GtkFileDialog.
 *
 * Vorlage: Stamp views/mail/message-list/stamp-attachment-button.c:132-400 (Öffnen über eine
 * Datei und GtkFileLauncher, Speichern über GtkFileDialog, Aktionen open/save-as). Abweichungen:
 * kein Vertrauensdialog vor dem Öffnen (Architektur 3 „keine Dialog-Kaskaden“; Evolution öffnet
 * ohne Rückfrage, e-attachment.c:2716-2862), Schreiben im Worker statt im UI-Thread. Verhalten:
 * Evolution e-util/e-attachment.c (Anzeigename, Inhaltstyp, Größe 2455-2552; Öffnen über eine
 * Datei in einem eigenen temporären Ordner 213-241, 2800-2862; „Alle speichern“ in einen Ordner
 * mit Namenszusatz „ (1)“ bei vorhandenen Dateien 3020-3075, 3460-3500).
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <errno.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include "mail-attachment.h"
#include "mail-view.h"

/* ------------------------------------------------------------------ Eintrag */

struct _RevolutionMailAttachment
{
  GObject        parent_instance;
  CamelMimePart *part;
  char          *name;
  char          *file_name;
  char          *content_type;
  GIcon         *icon;
  goffset        size;
};

enum
{
  PROP_0,
  PROP_NAME,
  PROP_SHORT_NAME,
  PROP_ICON,
  PROP_SIZE_TEXT,
  PROP_ACCESSIBLE_LABEL,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

G_DEFINE_FINAL_TYPE (RevolutionMailAttachment, revolution_mail_attachment, G_TYPE_OBJECT)

static char *
size_text (RevolutionMailAttachment *self)
{
  return self->size >= 0 ? g_format_size ((guint64) self->size) : g_strdup ("");
}

/* Name in der Pille: die waagrechte GtkListView gibt jeder Pille ihre Mindestbreite, ein Label mit
 * Auslassung zeigte dann nur „…“. Daher volle Breite und lange Namen hier in der Mitte gekürzt
 * (Anfang und Endung bleiben); der volle Name steht im Tooltip und im zugänglichen Namen. */
static char *
short_name (const char *name)
{
  glong length = g_utf8_strlen (name, -1);

  if (length <= 40)
    return g_strdup (name);

  {
    g_autofree char *start = g_utf8_substring (name, 0, 26);
    g_autofree char *end = g_utf8_substring (name, length - 12, length);

    return g_strconcat (start, "…", end, NULL);
  }
}

static void
revolution_mail_attachment_get_property (GObject    *object,
                                         guint       prop_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
  RevolutionMailAttachment *self = REVOLUTION_MAIL_ATTACHMENT (object);

  switch (prop_id)
    {
    case PROP_NAME:
      g_value_set_string (value, self->name);
      break;
    case PROP_SHORT_NAME:
      g_value_take_string (value, short_name (self->name));
      break;
    case PROP_ICON:
      g_value_set_object (value, self->icon);
      break;
    case PROP_SIZE_TEXT:
      g_value_take_string (value, size_text (self));
      break;
    case PROP_ACCESSIBLE_LABEL:
      {
        g_autofree char *type = g_content_type_get_description (self->content_type);
        g_autofree char *size = size_text (self);

        /* Translators: zugänglicher Name eines Anhangs: Dateiname, Typ, Größe. */
        g_value_take_string (value, g_strdup_printf (_("%s, %s, %s"), self->name, type, size));
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
revolution_mail_attachment_finalize (GObject *object)
{
  RevolutionMailAttachment *self = REVOLUTION_MAIL_ATTACHMENT (object);

  g_clear_object (&self->part);
  g_free (self->name);
  g_free (self->file_name);
  g_free (self->content_type);
  g_clear_object (&self->icon);

  G_OBJECT_CLASS (revolution_mail_attachment_parent_class)->finalize (object);
}

static void
revolution_mail_attachment_class_init (RevolutionMailAttachmentClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = revolution_mail_attachment_get_property;
  object_class->finalize = revolution_mail_attachment_finalize;

  properties[PROP_NAME] = g_param_spec_string ("name", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_SHORT_NAME] = g_param_spec_string ("short-name", NULL, NULL, NULL,
                                                     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_ICON] = g_param_spec_object ("icon", NULL, NULL, G_TYPE_ICON, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_SIZE_TEXT] = g_param_spec_string ("size-text", NULL, NULL, NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  properties[PROP_ACCESSIBLE_LABEL] = g_param_spec_string ("accessible-label", NULL, NULL, NULL,
                                                           G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
revolution_mail_attachment_init (RevolutionMailAttachment *self)
{
  self->size = -1;
}

/* Übernommen aus Evolution e-util/e-misc-utils.c:4425-4454 (e_util_make_safe_filename).
 * Lizenz: Dateikopf e-misc-utils.c „GNU Lesser General Public License“, Evolution COPYING:1-4
 * „LGPLv2 or (at your choice) LGPLv3“; LGPL erlaubt die Weitergabe unter GPL-3.0-or-later.
 * Nicht druckbare Zeichen, „/“ und „\“ sowie die Zeichen der Vorgabe von „filename-illegal-chars“
 * (org.gnome.evolution.shell.gschema.xml.in:150-151) werden „_“. Fest statt aus GSettings. */
static void
make_safe_filename (char *filename)
{
  const char *unsafe_chars = "/\\";
  const char *illegal_chars = "'\"&`();|<>$%{}!";
  char *pp = filename, *ts;
  gunichar cc;

  while (pp && *pp)
    {
      cc = g_utf8_get_char (pp);
      ts = pp;
      pp = g_utf8_next_char (pp);

      if (!g_unichar_isprint (cc) ||
          (cc < 0xff && (strchr (unsafe_chars, cc & 0xff) || strchr (illegal_chars, cc & 0xff))))
        {
          while (ts < pp)
            *ts++ = '_';
        }
    }
}

/* Übernommen aus Evolution e-util/e-attachment.c:3020-3044 (get_new_name_with_count), Lizenz wie
 * oben (Dateikopf e-attachment.c „GNU Lesser General Public License“). „name.ext“ → „name (2).ext“;
 * die Endung beginnt wie dort am ersten Punkt („a.tar.gz“ → „a (2).tar.gz“). */
static char *
get_new_name_with_count (const char *initial_name,
                         int         count)
{
  GString *string;
  const char *ext;
  gsize length;

  if (count == 0)
    return g_strdup (initial_name);

  string = g_string_sized_new (strlen (initial_name));
  ext = g_utf8_strchr (initial_name, -1, '.');

  if (ext != NULL)
    length = ext - initial_name;
  else
    length = strlen (initial_name);

  g_string_append_len (string, initial_name, length);
  g_string_append_printf (string, " (%d)", count);
  g_string_append (string, ext ? ext : "");

  return g_string_free (string, FALSE);
}

/* Steuer- und Formatzeichen (Unicode-Kategorien Cc, Cf, u. a. U+202E RIGHT-TO-LEFT OVERRIDE) werden
 * „_“, auch im angezeigten Namen (Pille, Tooltip, zugänglicher Name): „rechnung<U+202E>fdp.exe“
 * erschiene sonst als „rechnungexe.pdf“. Ungültiges UTF-8 wird vorher ersetzt. */
static char *
visible_name (const char *name)
{
  g_autofree char *valid = g_utf8_make_valid (name, -1);
  GString *out = g_string_sized_new (strlen (valid));

  for (const char *p = valid; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);
      GUnicodeType type = g_unichar_type (c);

      if (type == G_UNICODE_CONTROL || type == G_UNICODE_FORMAT)
        g_string_append_c (out, '_');
      else
        g_string_append_unichar (out, c);
    }

  return g_string_free (out, FALSE);
}

/* Höchstens max Bytes (Dateisysteme: 255 Byte je Name, NAME_MAX), UTF-8-sicher; die Endung ab dem
 * letzten Punkt bleibt, wenn sie kurz ist (bis 32 Byte), gekürzt wird davor. */
static char *
fit_name (const char *name,
          gsize       max)
{
  gsize length = strlen (name);
  const char *ext = strrchr (name, '.');
  gsize ext_length, cut;

  if (length <= max)
    return g_strdup (name);

  ext_length = ext != NULL && ext != name && strlen (ext) <= 32 ? strlen (ext) : 0;
  cut = max - ext_length;
  /* name[cut] ist das erste weggelassene Byte; ist es ein Folgebyte, fällt das ganze Zeichen weg. */
  while (cut > 0 && ((guchar) name[cut] & 0xC0) == 0x80)
    cut--;

  {
    GString *out = g_string_new_len (name, (gssize) cut);

    if (ext_length > 0)
      g_string_append (out, ext);
    return g_string_free (out, FALSE);
  }
}

/* Anzeigename wie Evolution (e-attachment.c:2494-2531): Dateiname des Teils (Kodierung nach
 * RFC 2047 aufgelöst) ohne Pfadanteil, bei message/rfc822 ohne Dateiname der Betreff, sonst
 * „anhang.dat“. Zusätzlich: „.“, „..“ und leere Namen gelten als kein Name; Steuer- und
 * Formatzeichen werden „_“ (visible_name). */
static char *
display_name (CamelMimePart    *part,
              CamelContentType *type)
{
  const char *filename = camel_mime_part_get_filename (part);
  g_autofree char *base = NULL;

  if (filename != NULL)
    {
      g_autofree char *decoded = camel_header_decode_string (filename, "UTF-8");

      base = g_path_get_basename (decoded != NULL && *decoded != '\0' ? decoded : filename);
    }
  else if (camel_content_type_is (type, "message", "rfc822"))
    {
      CamelDataWrapper *content = camel_medium_get_content (CAMEL_MEDIUM (part));
      const char *subject = CAMEL_IS_MIME_MESSAGE (content) ? camel_mime_message_get_subject (CAMEL_MIME_MESSAGE (content)) : NULL;

      if (subject != NULL)
        base = g_strdup (subject);
    }

  if (base != NULL)
    {
      char *visible = visible_name (base);

      g_free (base);
      base = visible;
    }

  if (base == NULL || *g_strstrip (base) == '\0' || g_str_equal (base, ".") || g_str_equal (base, "..") ||
      g_str_equal (base, G_DIR_SEPARATOR_S))
    {
      g_free (base);
      /* Translators: Dateiname eines Anhangs ohne eigenen Namen (Evolution „attachment.dat“). */
      base = g_strdup (_("anhang.dat"));
    }

  return g_steal_pointer (&base);
}

/* Im Worker (mail-reader.c): Name, Typ und Größe. Inhaltstyp wie Evolution (e-attachment.c:2457-2486):
 * MIME-Typ in Kleinbuchstaben, unbekannt → nach dem Dateinamen geraten. */
RevolutionMailAttachment *
revolution_mail_attachment_new (CamelMimePart *part,
                                GCancellable  *cancellable)
{
  RevolutionMailAttachment *self;
  CamelContentType *type;
  CamelDataWrapper *content;
  g_autofree char *mime_type = NULL;
  gsize size;

  g_return_val_if_fail (CAMEL_IS_MIME_PART (part), NULL);

  self = g_object_new (REVOLUTION_TYPE_MAIL_ATTACHMENT, NULL);
  self->part = g_object_ref (part);
  type = camel_mime_part_get_content_type (part);
  content = camel_medium_get_content (CAMEL_MEDIUM (part));

  self->name = display_name (part, type);
  self->file_name = fit_name (self->name, 255);
  make_safe_filename (self->file_name);
  /* Abweichung von Evolution: kein führender Punkt, sonst legte „Alle speichern“ versteckte Dateien
   * an (etwa „.xprofile“ in den persönlichen Ordner). */
  if (self->file_name[0] == '.')
    self->file_name[0] = '_';

  mime_type = type != NULL ? camel_content_type_simple (type) : NULL;
  if (mime_type != NULL)
    {
      g_autofree char *lower = g_ascii_strdown (mime_type, -1);

      self->content_type = g_content_type_from_mime_type (lower);
    }
  if (self->content_type == NULL || g_content_type_is_unknown (self->content_type))
    {
      g_free (self->content_type);
      self->content_type = g_content_type_guess (self->name, NULL, 0, NULL);
    }
  self->icon = g_content_type_get_symbolic_icon (self->content_type);

  size = content != NULL ? camel_data_wrapper_calculate_decoded_size_sync (content, cancellable, NULL) : (gsize) -1;
  self->size = size != (gsize) -1 ? (goffset) size : -1;

  return self;
}

/* #360: der Teil für den Composer (Anhänge der neuen Nachricht). */
CamelMimePart *
revolution_mail_attachment_get_part (RevolutionMailAttachment *self)
{
  return self->part;
}

/* Derselbe Teil kann gleichzeitig geöffnet und gespeichert werden (zwei Worker). Für gewöhnliche Teile
 * sperrt Camel 3.56 selbst (stream_lock um das Schreiben, camel-data-wrapper.c:261-276; Filter je
 * Aufruf, 282-351). Eine angehängte Nachricht (message/rfc822) ergänzt beim Schreiben aber ohne
 * Sperre ihre Kopfzeilen (mime_message_ensure_required_headers, camel-mime-message.c:281-295), und
 * Evolution nennt CamelDataWrapper nicht reentrant (e-attachment.c:3392-3394); daher nacheinander. */
static GMutex decode_lock;

/* Im Worker: Inhalt des Teils dekodiert (Transfer-Encoding) in den Strom, dann schließen. */
static gboolean
write_attachment (RevolutionMailAttachment *self,
                  GOutputStream            *stream,
                  GCancellable             *cancellable,
                  GError                  **error)
{
  CamelDataWrapper *content = camel_medium_get_content (CAMEL_MEDIUM (self->part));
  gssize written;

  if (content == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("Anhang ohne Inhalt"));
      return FALSE;
    }

  g_mutex_lock (&decode_lock);
  written = camel_data_wrapper_decode_to_output_stream_sync (content, stream, cancellable, error);
  g_mutex_unlock (&decode_lock);

  if (written < 0)
    return FALSE;

  g_debug ("Anhang: %s %" G_GSSIZE_FORMAT " Bytes geschrieben", self->name, written);
  return g_output_stream_close (stream, cancellable, error);
}

/* In eine neue Datei (nie überschreiben); bei Fehler die angefangene Datei entfernen. */
static gboolean
create_file (RevolutionMailAttachment *self,
             GFile                    *file,
             GFileCreateFlags          flags,
             GCancellable             *cancellable,
             GError                  **error)
{
  g_autoptr (GFileOutputStream) stream = g_file_create (file, flags, cancellable, error);

  if (stream == NULL)
    return FALSE;

  if (!write_attachment (self, G_OUTPUT_STREAM (stream), cancellable, error))
    {
      g_file_delete (file, NULL, NULL);
      return FALSE;
    }

  return TRUE;
}

/* ------------------------------------------------------------------ Anhangleiste */

struct _RevolutionMailAttachmentBar
{
  AdwBin              parent_instance;
  GtkListView        *list;
  GtkSingleSelection *selection;
  GMenuModel         *menu;
  GtkWidget          *popover;
  GListStore         *store;
};

G_DEFINE_FINAL_TYPE (RevolutionMailAttachmentBar, revolution_mail_attachment_bar, ADW_TYPE_BIN)

static GtkWindow *
window_for (RevolutionMailAttachmentBar *self)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));

  return GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL;
}

/* Fehler ins Sammel-Banner der Mail-Ansicht (Architektur 3), Einzelheiten im Tooltip und im Log. */
static void
show_error (RevolutionMailAttachmentBar *self,
            const char                  *title,
            const char                  *name,
            const GError                *error)
{
  GtkWidget *view = gtk_widget_get_ancestor (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_VIEW);
  g_autofree char *detail = g_strdup_printf ("%s: %s", name, error->message);

  g_warning ("%s: %s", title, detail);
  if (view != NULL)
    revolution_mail_view_show_error (REVOLUTION_MAIL_VIEW (view), title, detail, FALSE);
}

static RevolutionMailAttachment *
selected_attachment (RevolutionMailAttachmentBar *self)
{
  return gtk_single_selection_get_selected_item (self->selection);
}

/* ---- Öffnen */

static char *
temp_base (void)
{
  return g_build_filename (g_get_user_cache_dir (), "revolution", "attachments", NULL);
}

/* Aufräumen wie Evolution e-mktemp.c:47, 54-90 (TEMP_EXPIRE 2 h, Zugriffszeit, Zeile 79): beim Start im
 * Worker Dateien in ~/.cache/revolution/attachments/XXXXXX entfernen, die seit mehr als 2 h weder
 * gelesen noch geändert wurden, danach leere Ordner, die selbst älter als 2 h sind. Nur reguläre
 * Dateien und Ordner dieser einen Ebene (lstat, keine Verweise verfolgen). */
#define TEMP_EXPIRE (60 * 60 * 2)

static void
cleanup_thread (GTask        *task,
                gpointer      source_object G_GNUC_UNUSED,
                gpointer      task_data G_GNUC_UNUSED,
                GCancellable *cancellable G_GNUC_UNUSED)
{
  g_autofree char *base = temp_base ();
  g_autoptr (GDir) dir = g_dir_open (base, 0, NULL);
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  const char *name;
  guint files = 0, dirs = 0;

  while (dir != NULL && (name = g_dir_read_name (dir)) != NULL)
    {
      g_autofree char *sub = g_build_filename (base, name, NULL);
      g_autoptr (GDir) inner = NULL;
      GStatBuf st;
      const char *file;

      if (g_lstat (sub, &st) != 0 || !S_ISDIR (st.st_mode) || (inner = g_dir_open (sub, 0, NULL)) == NULL)
        continue;

      while ((file = g_dir_read_name (inner)) != NULL)
        {
          g_autofree char *path = g_build_filename (sub, file, NULL);
          GStatBuf fst;

          if (g_lstat (path, &fst) == 0 && S_ISREG (fst.st_mode) && MAX (fst.st_atime, fst.st_mtime) + TEMP_EXPIRE < now &&
              g_unlink (path) == 0)
            files++;
        }

      if (MAX (st.st_atime, st.st_mtime) + TEMP_EXPIRE < now && g_rmdir (sub) == 0)
        dirs++;
    }

  g_debug ("Anhang: temporäre Dateien aufgeräumt (%u Dateien, %u Ordner älter als 2 h)", files, dirs);
  g_task_return_boolean (task, TRUE);
}

/* Im Worker: eigener Ordner je Öffnen unter ~/.cache/revolution/attachments (Rechte 0700, wie
 * Evolution „We use a directory so the files can retain their basenames“, e-attachment.c:220-224;
 * Architektur 4: eigene Verzeichnisse), darin der bereinigte Dateiname mit Rechten 0600. */
static void
open_thread (GTask        *task,
             gpointer      source_object G_GNUC_UNUSED,
             gpointer      task_data,
             GCancellable *cancellable)
{
  RevolutionMailAttachment *attachment = task_data;
  g_autofree char *base = temp_base ();
  g_autofree char *template = g_build_filename (base, "XXXXXX", NULL);
  g_autoptr (GFile) file = NULL;
  GError *error = NULL;

  if (g_mkdir_with_parents (base, 0700) != 0 || g_mkdtemp_full (template, 0700) == NULL)
    {
      int saved = errno;

      g_task_return_new_error (task, G_IO_ERROR, g_io_error_from_errno (saved), "%s: %s", base, g_strerror (saved));
      return;
    }

  file = g_file_new_build_filename (template, attachment->file_name, NULL);
  if (!create_file (attachment, file, G_FILE_CREATE_PRIVATE, cancellable, &error))
    {
      g_rmdir (template);
      g_task_return_error (task, error);
      return;
    }

  g_task_return_pointer (task, g_steal_pointer (&file), g_object_unref);
}

typedef struct
{
  RevolutionMailAttachmentBar *bar;
  RevolutionMailAttachment    *attachment;
  GFile                       *file;
} Launch;

static void
launch_free (Launch *launch)
{
  g_object_unref (launch->bar);
  g_object_unref (launch->attachment);
  g_object_unref (launch->file);
  g_free (launch);
}

static void
on_launched (GObject      *source,
             GAsyncResult *result,
             gpointer      user_data)
{
  Launch *launch = user_data;
  g_autoptr (GError) error = NULL;
  g_autofree char *path = g_file_get_path (launch->file);

  if (gtk_file_launcher_launch_finish (GTK_FILE_LAUNCHER (source), result, &error))
    g_debug ("Anhang: %s geöffnet aus %s", launch->attachment->name, path);
  else if (!g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
    show_error (launch->bar, _("Anhang nicht geöffnet"), launch->attachment->name, error);

  launch_free (launch);
}

static void
on_open_written (GObject      *source_object,
                 GAsyncResult *result,
                 gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionMailAttachmentBar *self = REVOLUTION_MAIL_ATTACHMENT_BAR (source_object);
  RevolutionMailAttachment *attachment = g_task_get_task_data (G_TASK (result));
  g_autoptr (GError) error = NULL;
  g_autoptr (GtkFileLauncher) launcher = NULL;
  GFile *file = g_task_propagate_pointer (G_TASK (result), &error);
  Launch *launch;

  if (file == NULL)
    {
      show_error (self, _("Anhang nicht geöffnet"), attachment->name, error);
      return;
    }

  launch = g_new0 (Launch, 1);
  launch->bar = g_object_ref (self);
  launch->attachment = g_object_ref (attachment);
  launch->file = file;
  launcher = gtk_file_launcher_new (file);
  gtk_file_launcher_launch (launcher, window_for (self), NULL, on_launched, launch);
}

static void
open_attachment (RevolutionMailAttachmentBar *self,
                 RevolutionMailAttachment    *attachment)
{
  g_autoptr (GTask) task = NULL;

  if (attachment == NULL)
    return;

  g_debug ("Anhang: %s öffnen", attachment->name);
  task = g_task_new (self, NULL, on_open_written, NULL);
  g_task_set_source_tag (task, open_attachment);
  g_task_set_task_data (task, g_object_ref (attachment), g_object_unref);
  g_task_run_in_thread (task, open_thread);
}

/* ---- Speichern */

typedef struct
{
  GPtrArray *attachments;
  GFile     *target;
  gboolean   folder;
  guint      saved;
} Save;

static void
save_free (Save *save)
{
  g_ptr_array_unref (save->attachments);
  g_object_unref (save->target);
  g_free (save);
}

/* Im Worker. Speichern unter: die Datei aus dem Dialog; eine vorhandene ersetzt nur, wer im Dialog
 * „Ersetzen“ bestätigt hat (GtkFileChooser fragt im Speichern-Modus immer). Alle speichern: je
 * Anhang eine neue Datei im gewählten Ordner, bei vorhandenem Namen „Name (1).ext“, „(2)“ … wie
 * Evolution (attachment_save_create_cb, e-attachment.c:3479-3497); nie überschreiben. */
static void
save_thread (GTask        *task,
             gpointer      source_object G_GNUC_UNUSED,
             gpointer      task_data,
             GCancellable *cancellable)
{
  Save *save = task_data;
  GString *failures = NULL;
  GError *error = NULL;

  if (!save->folder)
    {
      RevolutionMailAttachment *attachment = g_ptr_array_index (save->attachments, 0);
      /* g_file_replace wie Evolution (e-attachment.c:3636-3639), aber ohne
       * G_FILE_CREATE_REPLACE_DESTINATION: Rechte der vorhandenen Datei und Verweise bleiben, das
       * Ziel eines Verweises wird beschrieben (PM #356 Runde 2). */
      g_autoptr (GFileOutputStream) stream = g_file_replace (save->target, NULL, FALSE, G_FILE_CREATE_NONE,
                                                             cancellable, &error);

      if (stream == NULL || !write_attachment (attachment, G_OUTPUT_STREAM (stream), cancellable, &error))
        {
          g_prefix_error (&error, "%s: ", attachment->name);
          g_task_return_error (task, error);
          return;
        }
      save->saved = 1;
      g_task_return_boolean (task, TRUE);
      return;
    }

  /* Ein Fehler bricht nicht ab: die übrigen Anhänge werden gespeichert, die Fehler gesammelt gemeldet. */
  for (guint i = 0; i < save->attachments->len; i++)
    {
      RevolutionMailAttachment *attachment = g_ptr_array_index (save->attachments, i);
      gboolean done = FALSE;

      for (int count = 0; !done && error == NULL && count < 1000; count++)
        {
          /* Mit Zusatz „ (N)“ höchstens 255 Byte (fit_name kürzt vor der Endung). */
          g_autofree char *suffix = g_strdup_printf (" (%d)", count);
          g_autofree char *base = fit_name (attachment->file_name, 255 - (count > 0 ? strlen (suffix) : 0));
          g_autofree char *name = get_new_name_with_count (base, count);
          g_autoptr (GFile) file = g_file_get_child (save->target, name);

          if (create_file (attachment, file, G_FILE_CREATE_NONE, cancellable, &error))
            {
              g_autofree char *path = g_file_get_path (file);

              g_debug ("Anhang: %s gespeichert unter %s", attachment->name, path);
              done = TRUE;
            }
          else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
            g_clear_error (&error);
        }
      if (done)
        {
          save->saved++;
          continue;
        }

      if (failures == NULL)
        failures = g_string_new (NULL);
      else
        g_string_append (failures, "; ");
      g_string_append_printf (failures, "%s: %s", attachment->name, error != NULL ? error->message : _("Kein freier Dateiname"));
      g_clear_error (&error);
    }

  if (failures != NULL)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               /* Translators: Alle speichern mit Fehlern, z. B. „2 von 5 nicht gespeichert: …“. */
                               _("%u von %u nicht gespeichert: %s"), save->attachments->len - save->saved,
                               save->attachments->len, failures->str);
      g_string_free (failures, TRUE);
      return;
    }

  g_task_return_boolean (task, TRUE);
}

static void
on_saved (GObject      *source_object,
          GAsyncResult *result,
          gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionMailAttachmentBar *self = REVOLUTION_MAIL_ATTACHMENT_BAR (source_object);
  Save *save = g_task_get_task_data (G_TASK (result));
  g_autoptr (GError) error = NULL;
  g_autofree char *path = g_file_get_path (save->target);

  if (!g_task_propagate_boolean (G_TASK (result), &error))
    show_error (self, _("Anhang nicht gespeichert"), path, error);

  g_debug ("Anhänge: %u gespeichert %s %s", save->saved, save->folder ? "im Ordner" : "als", path);
}

static void
start_save (RevolutionMailAttachmentBar *self,
            GPtrArray                   *attachments,
            GFile                       *target,
            gboolean                     folder)
{
  g_autoptr (GTask) task = g_task_new (self, NULL, on_saved, NULL);
  Save *save = g_new0 (Save, 1);

  save->attachments = g_ptr_array_ref (attachments);
  save->target = g_object_ref (target);
  save->folder = folder;
  g_task_set_source_tag (task, start_save);
  g_task_set_task_data (task, save, (GDestroyNotify) save_free);
  g_task_run_in_thread (task, save_thread);
}

static void
on_save_chosen (GObject      *source,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr (RevolutionMailAttachmentBar) self = user_data;
  GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
  GPtrArray *attachments = g_object_get_data (source, "revolution-attachments");
  gboolean folder = g_object_get_data (source, "revolution-folder") != NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) target = NULL;

  target = folder ? gtk_file_dialog_select_folder_finish (dialog, result, &error)
                  : gtk_file_dialog_save_finish (dialog, result, &error);
  if (target == NULL)
    {
      if (!g_error_matches (error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
        show_error (self, _("Anhang nicht gespeichert"), gtk_file_dialog_get_title (dialog), error);
      return;
    }

  start_save (self, attachments, target, folder);
}

static void
save_attachment (RevolutionMailAttachmentBar *self,
                 RevolutionMailAttachment    *attachment)
{
  g_autoptr (GtkFileDialog) dialog = NULL;
  GPtrArray *attachments;

  if (attachment == NULL)
    return;

  attachments = g_ptr_array_new_with_free_func (g_object_unref);
  g_ptr_array_add (attachments, g_object_ref (attachment));
  dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, _("Anhang speichern"));
  gtk_file_dialog_set_initial_name (dialog, attachment->file_name);
  gtk_file_dialog_set_accept_label (dialog, _("_Speichern"));
  g_object_set_data_full (G_OBJECT (dialog), "revolution-attachments", attachments, (GDestroyNotify) g_ptr_array_unref);
  gtk_file_dialog_save (dialog, window_for (self), NULL, on_save_chosen, g_object_ref (self));
}

static void
save_all (RevolutionMailAttachmentBar *self)
{
  g_autoptr (GtkFileDialog) dialog = NULL;
  GListModel *model = G_LIST_MODEL (self->store);
  GPtrArray *attachments;

  if (g_list_model_get_n_items (model) == 0)
    return;

  attachments = g_ptr_array_new_with_free_func (g_object_unref);
  for (guint i = 0; i < g_list_model_get_n_items (model); i++)
    g_ptr_array_add (attachments, g_list_model_get_item (model, i));

  dialog = gtk_file_dialog_new ();
  gtk_file_dialog_set_title (dialog, _("Alle Anhänge speichern"));
  gtk_file_dialog_set_accept_label (dialog, _("_Speichern"));
  g_object_set_data_full (G_OBJECT (dialog), "revolution-attachments", attachments, (GDestroyNotify) g_ptr_array_unref);
  g_object_set_data (G_OBJECT (dialog), "revolution-folder", GINT_TO_POINTER (TRUE));
  gtk_file_dialog_select_folder (dialog, window_for (self), NULL, on_save_chosen, g_object_ref (self));
}

/* ---- Bedienung */

static void
action_open (GtkWidget  *widget,
             const char *action_name G_GNUC_UNUSED,
             GVariant   *parameter G_GNUC_UNUSED)
{
  RevolutionMailAttachmentBar *self = REVOLUTION_MAIL_ATTACHMENT_BAR (widget);

  open_attachment (self, selected_attachment (self));
}

static void
action_save_as (GtkWidget  *widget,
                const char *action_name G_GNUC_UNUSED,
                GVariant   *parameter G_GNUC_UNUSED)
{
  RevolutionMailAttachmentBar *self = REVOLUTION_MAIL_ATTACHMENT_BAR (widget);

  save_attachment (self, selected_attachment (self));
}

static void
action_save_all (GtkWidget  *widget,
                 const char *action_name G_GNUC_UNUSED,
                 GVariant   *parameter G_GNUC_UNUSED)
{
  save_all (REVOLUTION_MAIL_ATTACHMENT_BAR (widget));
}

static void
show_menu (RevolutionMailAttachmentBar *self,
           GtkWidget                   *widget,
           double                       x,
           double                       y)
{
  graphene_point_t point;

  if (!gtk_widget_compute_point (widget, GTK_WIDGET (self->list), &GRAPHENE_POINT_INIT (x, y), &point))
    return;

  gtk_popover_set_pointing_to (GTK_POPOVER (self->popover), &(GdkRectangle) { (int) point.x, (int) point.y, 1, 1 });
  gtk_popover_popup (GTK_POPOVER (self->popover));
}

/* Menü-Taste, Umschalt+F10: Kontextmenü an der Pille mit dem Fokus (wie die Nachrichtenliste,
 * mail-view.c list.context-menu). */
static void
action_context_menu (GtkWidget  *widget,
                     const char *action_name G_GNUC_UNUSED,
                     GVariant   *parameter G_GNUC_UNUSED)
{
  RevolutionMailAttachmentBar *self = REVOLUTION_MAIL_ATTACHMENT_BAR (widget);
  GtkWidget *focus = gtk_root_get_focus (gtk_widget_get_root (widget));
  graphene_rect_t bounds;

  if (selected_attachment (self) == NULL || focus == NULL ||
      !gtk_widget_is_ancestor (focus, GTK_WIDGET (self->list)) ||
      !gtk_widget_compute_bounds (focus, GTK_WIDGET (self->list), &bounds))
    return;

  show_menu (self, GTK_WIDGET (self->list), bounds.origin.x + 12, bounds.origin.y + bounds.size.height);
}

/* Doppelklick oder Eingabe auf einer Pille: öffnen (Evolution: Doppelklick in der Anhangleiste). */
static void
on_activate (RevolutionMailAttachmentBar *self,
             guint                        position,
             GtkListView                 *list G_GNUC_UNUSED)
{
  g_autoptr (RevolutionMailAttachment) attachment = g_list_model_get_item (G_LIST_MODEL (self->store), position);

  open_attachment (self, attachment);
}

static void
on_secondary_click (GtkListItem     *item,
                    int              n_press G_GNUC_UNUSED,
                    double           x,
                    double           y,
                    GtkGestureClick *gesture)
{
  GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  RevolutionMailAttachmentBar *self =
    REVOLUTION_MAIL_ATTACHMENT_BAR (gtk_widget_get_ancestor (widget, REVOLUTION_TYPE_MAIL_ATTACHMENT_BAR));

  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  gtk_single_selection_set_selected (self->selection, gtk_list_item_get_position (item));
  show_menu (self, widget, x, y);
}

/* Anhänge der angezeigten Nachricht (mail-reader.c, UI-Thread); NULL oder leer blendet die Leiste aus. */
void
revolution_mail_attachment_bar_set (RevolutionMailAttachmentBar *self,
                                    GPtrArray                   *attachments)
{
  guint n = attachments != NULL ? attachments->len : 0;

  g_return_if_fail (REVOLUTION_IS_MAIL_ATTACHMENT_BAR (self));

  gtk_popover_popdown (GTK_POPOVER (self->popover));
  g_list_store_splice (self->store, 0, g_list_model_get_n_items (G_LIST_MODEL (self->store)),
                       n > 0 ? attachments->pdata : NULL, n);
  gtk_single_selection_set_selected (self->selection, GTK_INVALID_LIST_POSITION);
  gtk_widget_set_visible (GTK_WIDGET (self), n > 0);
}

static void
revolution_mail_attachment_bar_dispose (GObject *object)
{
  RevolutionMailAttachmentBar *self = REVOLUTION_MAIL_ATTACHMENT_BAR (object);

  g_clear_pointer (&self->popover, gtk_widget_unparent);
  gtk_widget_dispose_template (GTK_WIDGET (self), REVOLUTION_TYPE_MAIL_ATTACHMENT_BAR);
  g_clear_object (&self->store);

  G_OBJECT_CLASS (revolution_mail_attachment_bar_parent_class)->dispose (object);
}

static void
revolution_mail_attachment_bar_class_init (RevolutionMailAttachmentBarClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->dispose = revolution_mail_attachment_bar_dispose;

  g_type_ensure (REVOLUTION_TYPE_MAIL_ATTACHMENT);
  gtk_widget_class_set_template_from_resource (widget_class, "/de/freiraum_medien/Revolution/mail-attachment.ui");
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailAttachmentBar, list);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailAttachmentBar, selection);
  gtk_widget_class_bind_template_child (widget_class, RevolutionMailAttachmentBar, menu);
  gtk_widget_class_bind_template_callback (widget_class, on_activate);
  gtk_widget_class_bind_template_callback (widget_class, on_secondary_click);

  gtk_widget_class_install_action (widget_class, "attachment.open", NULL, action_open);
  gtk_widget_class_install_action (widget_class, "attachment.save-as", NULL, action_save_as);
  gtk_widget_class_install_action (widget_class, "attachment.save-all", NULL, action_save_all);
  gtk_widget_class_install_action (widget_class, "attachment.context-menu", NULL, action_context_menu);
}

static void
revolution_mail_attachment_bar_init (RevolutionMailAttachmentBar *self)
{
  static gsize cleaned;

  gtk_widget_init_template (GTK_WIDGET (self));

  /* Einmal je Start: alte temporäre Dateien aufräumen (cleanup_thread). */
  if (g_once_init_enter (&cleaned))
    {
      g_autoptr (GTask) task = g_task_new (NULL, NULL, NULL, NULL);

      g_task_set_source_tag (task, cleanup_thread);
      g_task_run_in_thread (task, cleanup_thread);
      g_once_init_leave (&cleaned, 1);
    }

  self->store = g_list_store_new (REVOLUTION_TYPE_MAIL_ATTACHMENT);
  gtk_single_selection_set_model (self->selection, G_LIST_MODEL (self->store));

  /* Wie das Kontextmenü der Nachrichtenliste (mail-view.c): ohne Pfeil an der Klickstelle. */
  self->popover = gtk_popover_menu_new_from_model (self->menu);
  gtk_popover_set_has_arrow (GTK_POPOVER (self->popover), FALSE);
  gtk_widget_set_halign (self->popover, GTK_ALIGN_START);
  gtk_widget_set_parent (self->popover, GTK_WIDGET (self->list));
}
