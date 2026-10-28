/* mail-folder.c – Mail-Ordner als Eintrag der Seitenleiste (#348)
 *
 * Kopie der Felder aus CamelFolderInfo (camel_store_get_folder_info_sync, main.c) für den
 * Ordnerbaum (GtkTreeListModel, CR #397). Die Blueprint-Zeilenvorlage (mail-view.blp) bindet
 * an „display-name“ und „icon-name“, der Abschnittskopf an „account“ (Kontoname).
 * Unterordner liegen in „children“ (create_func in mail-view.c), NULL ohne Unterordner.
 * #350: Spezialordner aus der ESource (revolution_mail_folder_mark_special, im Worker) und
 * Ungelesen-Zähler („unread-label“, live über CamelFolder::changed, revolution_mail_folder_watch).
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>

#include "eds-compat.h"
#include "mail-folder.h"

struct _RevolutionMailFolder
{
  GObject     parent_instance;
  char       *full_name;
  char       *display_name;
  const char *icon_name;
  guint32     flags;
  GListStore *children;
  /* Schwach: das Konto hält über den Ordnerbaum (mail-view.c) seine Ordner. */
  GWeakRef    account;
  /* #350: Ungelesen, -1 = unbekannt (imapx ohne geöffneten Ordner, camel-imapx-store.c:338-339). */
  int         unread;
  char       *unread_label;
  char       *accessible_label;
  /* #350: beobachteter CamelFolder (schwach wie Evolution mail-folder-cache.c:2349-2363),
   * beim Posteingang zusätzlich gehalten (held). */
  GWeakRef    camel_folder;
  CamelFolder *held;
  gulong      changed_id;
  gboolean    counting;
  gboolean    count_again;
};

G_DEFINE_FINAL_TYPE (RevolutionMailFolder, revolution_mail_folder, G_TYPE_OBJECT)

enum { PROP_0, PROP_FULL_NAME, PROP_DISPLAY_NAME, PROP_ICON_NAME, PROP_ACCOUNT, PROP_UNREAD_LABEL,
       PROP_ACCESSIBLE_LABEL, N_PROPS };

static GParamSpec *properties[N_PROPS];

static void
revolution_mail_folder_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  RevolutionMailFolder *self = REVOLUTION_MAIL_FOLDER (object);

  switch (prop_id)
    {
    case PROP_FULL_NAME:    g_value_set_string (value, self->full_name); break;
    case PROP_DISPLAY_NAME: g_value_set_string (value, self->display_name); break;
    case PROP_ICON_NAME:    g_value_set_static_string (value, self->icon_name); break;
    case PROP_ACCOUNT:      g_value_take_object (value, g_weak_ref_get (&self->account)); break;
    case PROP_UNREAD_LABEL: g_value_set_string (value, self->unread_label); break;
    case PROP_ACCESSIBLE_LABEL: g_value_set_string (value, self->accessible_label); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
revolution_mail_folder_finalize (GObject *object)
{
  RevolutionMailFolder *self = REVOLUTION_MAIL_FOLDER (object);
  g_autoptr (CamelFolder) folder = g_weak_ref_get (&self->camel_folder);

  if (folder != NULL && self->changed_id != 0)
    g_signal_handler_disconnect (folder, self->changed_id);
  g_weak_ref_clear (&self->camel_folder);
  g_clear_object (&self->held);
  g_free (self->full_name);
  g_free (self->display_name);
  g_free (self->unread_label);
  g_free (self->accessible_label);
  g_clear_object (&self->children);
  g_weak_ref_clear (&self->account);

  G_OBJECT_CLASS (revolution_mail_folder_parent_class)->finalize (object);
}

static void
revolution_mail_folder_class_init (RevolutionMailFolderClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamFlags flags = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;

  object_class->get_property = revolution_mail_folder_get_property;
  object_class->finalize = revolution_mail_folder_finalize;

  properties[PROP_FULL_NAME] = g_param_spec_string ("full-name", NULL, NULL, NULL, flags);
  properties[PROP_DISPLAY_NAME] = g_param_spec_string ("display-name", NULL, NULL, NULL, flags);
  properties[PROP_ICON_NAME] = g_param_spec_string ("icon-name", NULL, NULL, NULL, flags);
  properties[PROP_ACCOUNT] = g_param_spec_object ("account", NULL, NULL, REVOLUTION_TYPE_MAIL_ACCOUNT, flags);
  properties[PROP_UNREAD_LABEL] = g_param_spec_string ("unread-label", NULL, NULL, NULL,
                                                       flags | G_PARAM_EXPLICIT_NOTIFY);
  properties[PROP_ACCESSIBLE_LABEL] = g_param_spec_string ("accessible-label", NULL, NULL, NULL,
                                                           flags | G_PARAM_EXPLICIT_NOTIFY);
  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
revolution_mail_folder_init (RevolutionMailFolder *self)
{
  g_weak_ref_init (&self->account, NULL);
  g_weak_ref_init (&self->camel_folder, NULL);
  self->unread = -1;
}

/* Icons nach Ordnertyp wie Stamp (stamp-folder-item.c:56-89). inbox, drafts, sent und
 * archive liegen als Stamp-Icons in src/icons; Spam wie im Prototyp (Tag m2-prototyp,
 * mail-sample.c), Postausgang mit dem Adwaita-Icon mail-send-symbolic (Stamps
 * outbox-symbolic ist nicht übernommen). */
static const char *
icon_for_type (guint32 flags)
{
  switch (flags & CAMEL_FOLDER_TYPE_MASK)
    {
    case CAMEL_FOLDER_TYPE_INBOX:   return "inbox-symbolic";
    case CAMEL_FOLDER_TYPE_OUTBOX:  return "mail-send-symbolic";
    case CAMEL_FOLDER_TYPE_DRAFTS:  return "drafts-symbolic";
    case CAMEL_FOLDER_TYPE_SENT:    return "sent-symbolic";
    case CAMEL_FOLDER_TYPE_JUNK:    return "mail-mark-junk-symbolic";
    case CAMEL_FOLDER_TYPE_TRASH:   return "user-trash-symbolic";
    case CAMEL_FOLDER_TYPE_ARCHIVE: return "archive-symbolic";
    default:                        return "folder-symbolic";
    }
}

/* Namen der Spezialordner wie Evolution (em-folder-tree-model.c:1557-1575): nur bei
 * passendem Typ und englischem Namen vom Store, z. B. imapx „Inbox“ (EDS
 * camel-imapx-store.c:348) und die virtuellen „Trash“/„Junk“ (camel-store.c:1729-1758).
 * Deren Übersetzung fehlt in EDS (evolution-data-server.mo hat 7 Einträge); Evolution
 * übersetzt mit eigenen msgids. Deutsche Namen wie der Prototyp (Tag m2-prototyp,
 * mail-sample.c: Eingang, Entwürfe, Gesendet, Spam, Papierkorb). */
static const struct
{
  guint32 type;
  const char *bare_name;
  const char *name;
} special_folders[] = {
  { CAMEL_FOLDER_TYPE_INBOX,  "Inbox",  N_("Eingang") },
  { CAMEL_FOLDER_TYPE_TRASH,  "Trash",  N_("Papierkorb") },
  { CAMEL_FOLDER_TYPE_JUNK,   "Junk",   N_("Spam") },
  { CAMEL_FOLDER_TYPE_SENT,   "Sent",   N_("Gesendet") },
  { CAMEL_FOLDER_TYPE_DRAFTS, "Drafts", N_("Entwürfe") },
  /* #350: nicht in Evolutions Tabelle; Archiv-Ordner aus der Ersteinrichtung von imapx heißen
   * „Archive“ (camel-imapx-store.c:2814-2816), Name wie der Prototyp. */
  { CAMEL_FOLDER_TYPE_ARCHIVE, "Archive", N_("Archiv") },
};

static const char *
display_name_for (CamelFolderInfo *fi)
{
  const char *name = fi->display_name != NULL ? fi->display_name : fi->full_name;

  for (gsize i = 0; i < G_N_ELEMENTS (special_folders); i++)
    if ((fi->flags & CAMEL_FOLDER_TYPE_MASK) == special_folders[i].type &&
        g_strcmp0 (name, special_folders[i].bare_name) == 0)
      return _(special_folders[i].name);

  return name;
}

/* Reihenfolge wie der abgenommene Prototyp (Dokument #5, Tag m2-prototyp mail-sample.c):
 * Eingang, Entwürfe, Gesendet, Spam, Papierkorb, Archiv, dann alle übrigen. Evolution
 * stellt nur den Posteingang nach vorn (em-folder-tree-model.c:464-470). */
static guint
rank_for_type (guint32 flags)
{
  switch (flags & CAMEL_FOLDER_TYPE_MASK)
    {
    case CAMEL_FOLDER_TYPE_INBOX:   return 0;
    case CAMEL_FOLDER_TYPE_DRAFTS:  return 1;
    case CAMEL_FOLDER_TYPE_SENT:    return 2;
    case CAMEL_FOLDER_TYPE_JUNK:    return 3;
    case CAMEL_FOLDER_TYPE_TRASH:   return 4;
    case CAMEL_FOLDER_TYPE_ARCHIVE: return 5;
    default:                        return 6;
    }
}

/* Gleicher Rang alphabetisch wie Evolution (em-folder-tree-model.c:474-483). */
static int
compare_folders (gconstpointer a,
                 gconstpointer b,
                 gpointer      user_data G_GNUC_UNUSED)
{
  const RevolutionMailFolder *fa = a, *fb = b;
  guint ra = rank_for_type (fa->flags), rb = rank_for_type (fb->flags);

  if (ra != rb)
    return ra < rb ? -1 : 1;

  return g_utf8_collate (fa->display_name, fb->display_name);
}

const char *
revolution_mail_folder_get_full_name (RevolutionMailFolder *self)
{
  g_return_val_if_fail (REVOLUTION_IS_MAIL_FOLDER (self), NULL);

  return self->full_name;
}

/* #352: CamelFolderInfo-Flags, z. B. CAMEL_FOLDER_NOSELECT. */
guint32
revolution_mail_folder_get_flags (RevolutionMailFolder *self)
{
  g_return_val_if_fail (REVOLUTION_IS_MAIL_FOLDER (self), 0);

  return self->flags;
}

GListModel *
revolution_mail_folder_get_children (RevolutionMailFolder *self)
{
  g_return_val_if_fail (REVOLUTION_IS_MAIL_FOLDER (self), NULL);

  return self->children != NULL ? G_LIST_MODEL (self->children) : NULL;
}

/* #350: Ungelesen-Zähler rechts in der Ordnerzeile wie der Prototyp (Tag m2-prototyp,
 * mail-view.blp:176-189), nur ungelesen (Task #350); 0 und unbekannt (-1) ohne Zahl.
 * Zugänglicher Name der Zeile mit Zahl (Architektur 8). Nur im UI-Thread. */
static void
set_unread (RevolutionMailFolder *self,
            int                   unread)
{
  g_autoptr (RevolutionMailAccount) account = g_weak_ref_get (&self->account);
  g_autofree char *label = NULL;
  g_autofree char *accessible = NULL;

  if (self->unread == unread && self->accessible_label != NULL)
    return;
  self->unread = unread;

  if (unread > 0)
    {
      label = g_strdup_printf ("%d", unread);
      /* Translators: zugänglicher Name einer Ordnerzeile, z. B. „Eingang, 4 ungelesen“. */
      accessible = g_strdup_printf (ngettext ("%s, %d ungelesen", "%s, %d ungelesen", unread),
                                    self->display_name, unread);
    }

  g_free (self->unread_label);
  self->unread_label = g_steal_pointer (&label);
  g_free (self->accessible_label);
  self->accessible_label = accessible != NULL ? g_steal_pointer (&accessible) : g_strdup (self->display_name);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_UNREAD_LABEL]);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ACCESSIBLE_LABEL]);

  if (unread >= 0 && account != NULL)
    g_debug ("Ungelesen: %s %s %d", revolution_mail_account_get_uid (account), self->full_name, unread);
}

/* Eintrag zu fi ohne Unterordner. */
static RevolutionMailFolder *
folder_from_info (CamelFolderInfo       *fi,
                  RevolutionMailAccount *account)
{
  RevolutionMailFolder *folder = g_object_new (REVOLUTION_TYPE_MAIL_FOLDER, NULL);

  folder->full_name = g_strdup (fi->full_name);
  folder->display_name = g_strdup (display_name_for (fi));
  folder->flags = fi->flags;
  folder->icon_name = icon_for_type (fi->flags);
  g_weak_ref_set (&folder->account, account);
  /* #350: Zahl aus der Ordnerliste, sofern der Store sie kennt (imapx mit LIST-STATUS oder
   * geöffnetem Ordner, camel-imapx-store.c:1362-1400; microsoft365 aus Graph). Wie
   * Evolution setup_folder (mail-folder-cache.c:1260). */
  set_unread (folder, (fi->flags & CAMEL_FOLDER_NOSELECT) != 0 ? -1 : fi->unread);

  return folder;
}

/* Hängt first und seine Geschwister (mit Unterordnern) sortiert an store an; nur im
 * UI-Thread (README „Datenmodell“). Gibt die Zahl aller Ordner zurück. */
guint
revolution_mail_folder_append_tree (GListStore            *store,
                                    CamelFolderInfo       *first,
                                    RevolutionMailAccount *account)
{
  guint count = 0;

  for (CamelFolderInfo *fi = first; fi != NULL; fi = fi->next)
    {
      g_autoptr (RevolutionMailFolder) folder = folder_from_info (fi, account);

      g_debug ("Ordner: %s %s", revolution_mail_account_get_uid (account), fi->full_name);
      if (fi->child != NULL)
        {
          folder->children = g_list_store_new (REVOLUTION_TYPE_MAIL_FOLDER);
          count += revolution_mail_folder_append_tree (folder->children, fi->child, account);
        }

      g_list_store_append (store, folder);
      count++;
    }

  g_list_store_sort (store, compare_folders, NULL);

  return count;
}

/* #408: Ordnerbaum nach „Netz zurück“ mit der neuen Ordnerliste abgleichen, statt ihn zu
 * ersetzen (revolution_mail_view_update_folders): bestehende Einträge bleiben dieselben Objekte,
 * damit Auswahl und Aufklappzustand des GtkTreeListModel bleiben; neue Ordner kommen sortiert
 * dazu, fehlende fallen weg. Wie Evolution, das den Baum nur je Ordner ändert (folder-created/
 * -deleted, em-folder-tree-model.c). Zähler: nur, wenn die Liste einen kennt (sonst -1, imapx,
 * forget_unread in main.c), sonst bleibt der gezählte Stand. Nur im UI-Thread. added/removed
 * zählen mit. */
void
revolution_mail_folder_merge_tree (GListStore            *store,
                                   CamelFolderInfo       *first,
                                   RevolutionMailAccount *account,
                                   guint                 *added,
                                   guint                 *removed)
{
  const char *uid = revolution_mail_account_get_uid (account);

  for (guint i = g_list_model_get_n_items (G_LIST_MODEL (store)); i > 0; i--)
    {
      g_autoptr (RevolutionMailFolder) folder = g_list_model_get_item (G_LIST_MODEL (store), i - 1);
      CamelFolderInfo *fi = first;

      while (fi != NULL && g_strcmp0 (fi->full_name, folder->full_name) != 0)
        fi = fi->next;
      if (fi == NULL)
        {
          g_debug ("Ordner: %s entfernt %s", uid, folder->full_name);
          g_list_store_remove (store, i - 1);
          (*removed)++;
        }
    }

  for (CamelFolderInfo *fi = first; fi != NULL; fi = fi->next)
    {
      g_autoptr (RevolutionMailFolder) folder = NULL;
      guint position;

      for (position = 0; position < g_list_model_get_n_items (G_LIST_MODEL (store)); position++)
        {
          g_autoptr (RevolutionMailFolder) item = g_list_model_get_item (G_LIST_MODEL (store), position);

          if (g_strcmp0 (item->full_name, fi->full_name) == 0)
            {
              folder = g_steal_pointer (&item);
              break;
            }
        }

      if (folder == NULL)
        {
          folder = folder_from_info (fi, account);
          g_debug ("Ordner: %s neu %s", uid, fi->full_name);
          if (fi->child != NULL)
            {
              folder->children = g_list_store_new (REVOLUTION_TYPE_MAIL_FOLDER);
              *added += revolution_mail_folder_append_tree (folder->children, fi->child, account);
            }
          g_list_store_insert_sorted (store, folder, compare_folders, NULL);
          (*added)++;
          continue;
        }

      if (fi->unread >= 0 && (fi->flags & CAMEL_FOLDER_NOSELECT) == 0)
        set_unread (folder, fi->unread);

      if ((fi->child != NULL) != (folder->children != NULL))
        {
          /* Erst jetzt Unterordner bzw. keine mehr. #364: Das GtkTreeListModel erfragt die Kinder
           * einer Zeile nur beim Anlegen der Zeile (folder_children, mail-view.c), daher den Eintrag
           * an derselben Stelle neu einsetzen; die Auswahl dieser Zeile geht dabei verloren. */
          for (guint i = 0; folder->children != NULL && i < g_list_model_get_n_items (G_LIST_MODEL (folder->children)); i++)
            {
              g_autoptr (RevolutionMailFolder) child = g_list_model_get_item (G_LIST_MODEL (folder->children), i);

              g_debug ("Ordner: %s entfernt %s", uid, child->full_name);
              (*removed)++;
            }
          g_clear_object (&folder->children);
          if (fi->child != NULL)
            {
              folder->children = g_list_store_new (REVOLUTION_TYPE_MAIL_FOLDER);
              *added += revolution_mail_folder_append_tree (folder->children, fi->child, account);
            }
          g_list_store_splice (store, position, 1, (gpointer *) &folder, 1);
        }
      else if (folder->children != NULL)
        revolution_mail_folder_merge_tree (folder->children, fi->child, account, added, removed);
    }
}

/* #351: Eintrag ohne CamelFolderInfo für die Favoriten (mail-favorites.c): Name und Symbol fest,
 * ohne Ordnertyp, damit die Erstauswahl (mail-view.c) ihn nicht für einen Posteingang hält.
 * icon_name: statische Zeichenkette. */
RevolutionMailFolder *
revolution_mail_folder_new (const char            *full_name,
                            const char            *display_name,
                            const char            *icon_name,
                            RevolutionMailAccount *account)
{
  RevolutionMailFolder *folder = g_object_new (REVOLUTION_TYPE_MAIL_FOLDER, NULL);

  folder->full_name = g_strdup (full_name);
  folder->display_name = g_strdup (display_name);
  folder->icon_name = icon_name;
  g_weak_ref_set (&folder->account, account);
  set_unread (folder, -1);

  return folder;
}

/* #350: Ordner mit diesem vollen Namen in model und seinen Unterordnern (neue Referenz) oder
 * NULL. */
RevolutionMailFolder *
revolution_mail_folder_find (GListModel *model,
                             const char *full_name)
{
  for (guint i = 0; model != NULL && i < g_list_model_get_n_items (model); i++)
    {
      g_autoptr (RevolutionMailFolder) folder = g_list_model_get_item (model, i);
      RevolutionMailFolder *found;

      if (g_strcmp0 (folder->full_name, full_name) == 0)
        return g_steal_pointer (&folder);

      found = revolution_mail_folder_find (revolution_mail_folder_get_children (folder), full_name);
      if (found != NULL)
        return found;
    }

  return NULL;
}

/* #350: Ungelesen zählen wie Evolution update_1folder (mail-folder-cache.c:846-933) mit
 * camel_folder_get_unread_message_count, im Worker wie dort (mail_process_folder_changes).
 * Die Summary zählt Gelöschte und Spam nicht mit (EDS camel-folder-summary.c:418-421).
 * Immer nur eine Zählung je Ordner; Änderungen währenddessen lösen eine weitere aus. */
static void
count_unread_thread (GTask        *task,
                     gpointer      source_object G_GNUC_UNUSED,
                     gpointer      task_data,
                     GCancellable *cancellable G_GNUC_UNUSED)
{
  g_task_return_int (task, camel_folder_get_unread_message_count (task_data));
}

static void count_unread (RevolutionMailFolder *self);

static void
on_unread_counted (GObject      *source_object,
                   GAsyncResult *result,
                   gpointer      user_data G_GNUC_UNUSED)
{
  RevolutionMailFolder *self = REVOLUTION_MAIL_FOLDER (source_object);
  gssize unread = g_task_propagate_int (G_TASK (result), NULL);

  self->counting = FALSE;
  if (unread >= 0)
    set_unread (self, (int) unread);

  if (self->count_again)
    count_unread (self);
}

static void
count_unread (RevolutionMailFolder *self)
{
  g_autoptr (CamelFolder) folder = g_weak_ref_get (&self->camel_folder);
  g_autoptr (GTask) task = NULL;

  if (folder == NULL)
    return;

  if (self->counting)
    {
      self->count_again = TRUE;
      return;
    }

  self->counting = TRUE;
  self->count_again = FALSE;
  task = g_task_new (self, NULL, on_unread_counted, NULL);
  g_task_set_source_tag (task, count_unread);
  g_task_set_task_data (task, g_steal_pointer (&folder), g_object_unref);
  g_task_run_in_thread (task, count_unread_thread);
}

static gboolean
count_unread_idle (gpointer data)
{
  count_unread (data);

  return G_SOURCE_REMOVE;
}

/* CamelFolder::changed kommt über camel_session_idle_add im Hauptkontext, bei eingefrorenem
 * Ordner aber im aufrufenden Thread (EDS camel-folder.c:2860-2899); daher über eine schwache
 * Referenz in den UI-Thread. */
static void
on_camel_folder_changed (CamelFolder           *folder G_GNUC_UNUSED,
                         CamelFolderChangeInfo *changes G_GNUC_UNUSED,
                         GWeakRef              *item)
{
  RevolutionMailFolder *self = g_weak_ref_get (item);

  if (self != NULL)
    g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT_IDLE, count_unread_idle, self, g_object_unref);
}

static void
weak_ref_free (gpointer data,
               GClosure *closure G_GNUC_UNUSED)
{
  g_weak_ref_clear (data);
  g_free (data);
}

/* #350: Zähler dieses Ordners live aus folder („changed“) wie Evolution
 * mail_folder_cache_note_folder (mail-folder-cache.c:2290-2381), nur im UI-Thread. hold: den
 * Ordner zusätzlich halten (Posteingang, main.c); sonst lebt er nur, solange ihn jemand anderes
 * hält (Nachrichtenliste). refreshed: folder ist in dieser Sitzung mit dem Server abgeglichen,
 * jetzt zählen. */
void
revolution_mail_folder_watch (RevolutionMailFolder *self,
                              CamelFolder          *folder,
                              gboolean              hold,
                              gboolean              refreshed)
{
  g_autoptr (CamelFolder) old = NULL;
  GWeakRef *item;

  g_return_if_fail (REVOLUTION_IS_MAIL_FOLDER (self));
  g_return_if_fail (CAMEL_IS_FOLDER (folder));

  if (hold)
    g_set_object (&self->held, folder);

  old = g_weak_ref_get (&self->camel_folder);
  if (old != folder)
    {
      if (old != NULL && self->changed_id != 0)
        g_signal_handler_disconnect (old, self->changed_id);

      item = g_new0 (GWeakRef, 1);
      g_weak_ref_init (item, self);
      g_weak_ref_set (&self->camel_folder, folder);
      self->changed_id = g_signal_connect_data (folder, "changed", G_CALLBACK (on_camel_folder_changed),
                                                item, weak_ref_free, G_CONNECT_DEFAULT);
    }

  /* Ein nur geöffneter Ordner (Signal „folder-opened“) zählt nicht: seine Summary ist der
   * Cache, beim ersten Öffnen leer (gemessen: microsoft365 0 statt 269 aus Graph), und imapx
   * öffnet beim Verbinden jeden Ordner (camel-offline-store.c:317, camel-imapx-store.c:
   * 2904-2925). Gezählt wird nach dem Abgleich (refreshed) und bei jedem „changed“. */
  if (refreshed)
    count_unread (self);
}

/* #350: Nach Vorlage Evolution 3.56.2 libemail-engine/e-mail-folder-utils.c:1862-1900
 * (e_mail_folder_uri_parse, Zweig „folder://“), nachgebildet statt übernommen (#346-Entscheidung,
 * die Datei wird nicht übernommen). folder://UID/PFAD (Format e_mail_folder_uri_build, dort
 * 1808-1835) → voller Name, wenn die URI in den Store mit store_uid zeigt, sonst NULL. Die alten
 * Formate „email://“ und Store-URIs aus Evolution 2.x (dort 1901-1970) nicht. Vergleich der
 * Namen wie e_mail_folder_uri_equal (dort 2026-2070) über CamelStoreClass::equal_folder_name
 * (apply_special). */
static char *
folder_uri_to_name (const char *uri,
                    const char *store_uid)
{
  g_autoptr (CamelURL) url = NULL;
  g_autofree char *uid = NULL;

  if (uri == NULL || *uri == '\0')
    return NULL;

  url = camel_url_new (uri, NULL);
  if (url == NULL || g_strcmp0 (url->protocol, "folder") != 0 || url->host == NULL ||
      url->path == NULL || *url->path != '/')
    return NULL;

  uid = url->user != NULL && *url->user != '\0' ? g_strconcat (url->user, "@", url->host, NULL)
                                                : g_strdup (url->host);

  return g_strcmp0 (uid, store_uid) == 0 ? camel_url_decode_path (url->path + 1) : NULL;
}

typedef struct
{
  guint32     type;
  const char *setup_key;
  const char *label;
  char       *name;
  const char *source;
} SpecialFolder;

static gboolean
tree_has_type (CamelFolderInfo *fi,
               guint32          type)
{
  for (; fi != NULL; fi = fi->next)
    if ((fi->flags & CAMEL_FOLDER_TYPE_MASK) == type || tree_has_type (fi->child, type))
      return TRUE;

  return FALSE;
}

static void
apply_special (CamelStore      *store,
               CamelFolderInfo *fi,
               SpecialFolder   *special,
               gsize            n_special)
{
  GEqualFunc equal = CAMEL_STORE_GET_CLASS (store)->equal_folder_name;

  for (; fi != NULL; fi = fi->next)
    {
      /* Nur Ordner ohne Typ, wie Evolution (em-folder-tree-model.c:747): Typen vom Server
       * (Posteingang, microsoft365) haben Vorrang. Reihenfolge Entwürfe, Gesendet, Archiv wie
       * dort 749-776. */
      for (gsize i = 0; i < n_special && (fi->flags & CAMEL_FOLDER_TYPE_MASK) == 0; i++)
        if (special[i].name != NULL && equal (fi->full_name, special[i].name))
          {
            fi->flags |= special[i].type;
            g_debug ("Spezialordner: %s %s %s (%s)", camel_service_get_uid (CAMEL_SERVICE (store)),
                     special[i].label, fi->full_name, special[i].source);
          }

      apply_special (store, fi->child, special, n_special);
    }
}

/* #350: Gesendet, Entwürfe und Archiv als Ordnertyp in info eintragen, im Worker. Wie Evolution
 * (em-folder-tree-model.c:655-791, folder_tree_model_get_special_folders_uri und
 * e_mail_session_is_archive_folder, e-mail-session.c:3139-3165) aus der Identität des Kontos
 * (em_utils_ref_mail_identity_for_store, e-mail-utils.c:789-820): Mail Submission
 * „sent-folder“, Mail Composition „drafts-folder“, Mail Account „archive-folder“.
 * Zeigt die ESource nicht in diesen Store (bei Konten, die Evolution nie eingerichtet hat,
 * steht dort folder://local/…), gilt das Ergebnis der Ersteinrichtung des Stores
 * (camel_store_initial_setup_sync), das Evolution beim ersten Verbinden in die ESource schreibt
 * (mail-folder-cache.c:1971-2028). Revolution schreibt es nie (#348), es gilt nur im Speicher.
 * imapx liest dafür nur (LIST, SPECIAL-USE, Namen, camel-imapx-store.c:2795-2890). */
void
revolution_mail_folder_mark_special (CamelStore      *store,
                                     ESourceRegistry *registry,
                                     CamelFolderInfo *info,
                                     GCancellable    *cancellable)
{
  const char *uid = camel_service_get_uid (CAMEL_SERVICE (store));
  SpecialFolder special[] = {
    { CAMEL_FOLDER_TYPE_DRAFTS,  CAMEL_STORE_SETUP_DRAFTS_FOLDER,  "Entwürfe", NULL, "ESource" },
    { CAMEL_FOLDER_TYPE_SENT,    CAMEL_STORE_SETUP_SENT_FOLDER,    "Gesendet", NULL, "ESource" },
    { CAMEL_FOLDER_TYPE_ARCHIVE, CAMEL_STORE_SETUP_ARCHIVE_FOLDER, "Archiv",   NULL, "ESource" },
  };
  g_autoptr (ESource) account = e_source_registry_ref_source (registry, uid);
  g_autoptr (ESource) identity = NULL;
  g_autoptr (GHashTable) setup = NULL;
  g_autoptr (GError) error = NULL;
  gboolean need_setup = FALSE;

  if (account != NULL && e_source_has_extension (account, E_SOURCE_EXTENSION_MAIL_ACCOUNT))
    {
      ESourceMailAccount *extension = e_source_get_extension (account, E_SOURCE_EXTENSION_MAIL_ACCOUNT);
      g_autofree char *identity_uid = e_source_mail_account_dup_identity_uid (extension);
      g_autofree char *archive = e_source_mail_account_dup_archive_folder (extension);

      special[2].name = folder_uri_to_name (archive, uid);
      if (identity_uid != NULL)
        identity = e_source_registry_ref_source (registry, identity_uid);
    }

  if (identity != NULL && e_source_has_extension (identity, E_SOURCE_EXTENSION_MAIL_COMPOSITION))
    {
      g_autofree char *drafts = e_source_mail_composition_dup_drafts_folder (
        e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_COMPOSITION));

      special[0].name = folder_uri_to_name (drafts, uid);
    }

  if (identity != NULL && e_source_has_extension (identity, E_SOURCE_EXTENSION_MAIL_SUBMISSION))
    {
      g_autofree char *sent = e_source_mail_submission_dup_sent_folder (
        e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_SUBMISSION));

      special[1].name = folder_uri_to_name (sent, uid);
    }

  /* Ersteinrichtung nur, wenn ein Typ weder in der ESource noch vom Server (microsoft365,
   * camel-m365-store.c:157-169) kommt; imapx fragt dafür die Ordnerliste ein zweites Mal ab. */
  for (gsize i = 0; i < G_N_ELEMENTS (special); i++)
    need_setup |= special[i].name == NULL && !tree_has_type (info, special[i].type);

  if (need_setup && (camel_store_get_flags (store) & CAMEL_STORE_SUPPORTS_INITIAL_SETUP) != 0)
    {
      gint64 started = g_get_monotonic_time ();

      if (!camel_store_initial_setup_sync (store, &setup, cancellable, &error))
        g_debug ("Spezialordner: %s Ersteinrichtung: %s", uid, error->message);
      else
        g_debug ("Spezialordner: %s Ersteinrichtung nach %.0f ms", uid,
                 (g_get_monotonic_time () - started) / 1000.0);

      for (gsize i = 0; setup != NULL && i < G_N_ELEMENTS (special); i++)
        if (special[i].name == NULL && g_hash_table_contains (setup, special[i].setup_key))
          {
            special[i].name = g_strdup (g_hash_table_lookup (setup, special[i].setup_key));
            special[i].source = "Ersteinrichtung";
          }
    }

  apply_special (store, info, special, G_N_ELEMENTS (special));

  for (gsize i = 0; i < G_N_ELEMENTS (special); i++)
    g_free (special[i].name);
}

/* #364: Ordner anlegen wie Evolution e_mail_store_create_folder_sync (libemail-engine/
 * e-mail-store-utils.c:28-68): camel_store_create_folder_sync, danach abonnieren, wenn der Store
 * Abos kennt (imapx; microsoft365 nimmt das Abo ohne Serveraufruf an, camel-m365-store.c:1547-1553).
 * Abweichung: ein gescheitertes Abo ist kein Fehler. imapx legt auf oberster Ebene im persönlichen
 * Namensraum an (camel-imapx-store.c:2365-2389, geops@: „INBOX.“), das Abo unter dem übergebenen Namen
 * findet den Ordner dann nicht (gemessen: „No such folder“), obwohl er angelegt ist; ohne Abos
 * (UseSubscriptions=false, Vorgabe camel-imapx-settings.c:856) ist er trotzdem sichtbar.
 * parent NULL: oberste Ebene. Im Worker. */
gboolean
revolution_mail_folder_create_sync (CamelStore    *store,
                                    const char    *parent,
                                    const char    *name,
                                    GCancellable  *cancellable,
                                    GError       **error)
{
  g_autofree char *full_name = parent != NULL ? g_strconcat (parent, "/", name, NULL) : g_strdup (name);
  CamelFolderInfo *fi = camel_store_create_folder_sync (store, parent != NULL ? parent : "", name, cancellable, error);
  g_autoptr (GError) local_error = NULL;

  if (fi == NULL)
    return FALSE;
  camel_folder_info_free (fi);

  if (CAMEL_IS_SUBSCRIBABLE (store) &&
      !camel_subscribable_subscribe_folder_sync (CAMEL_SUBSCRIBABLE (store), full_name, cancellable, &local_error))
    g_debug ("Ordner: %s %s nicht abonniert: %s", camel_service_get_uid (CAMEL_SERVICE (store)), full_name,
             local_error->message);

  return TRUE;
}

/* #364: Umbenennen wie Evolution folder_tree_cell_edited_cb (mail/em-folder-tree.c:626-722): neuer
 * voller Name aus dem alten mit neuem letzten Teil (dort 683-689), vorher prüfen, ob es ihn schon gibt
 * (dort 691-701). Abos stellt imapx selbst um (camel-imapx-store.c:2512-2547). Im Worker. new_full_name:
 * Rückgabe, frei mit g_free. */
gboolean
revolution_mail_folder_rename_sync (CamelStore    *store,
                                    const char    *full_name,
                                    const char    *name,
                                    char         **new_full_name,
                                    GCancellable  *cancellable,
                                    GError       **error)
{
  const char *slash = strrchr (full_name, '/');
  CamelFolderInfo *exists;

  *new_full_name = slash != NULL ? g_strdup_printf ("%.*s/%s", (int) (slash - full_name), full_name, name)
                                 : g_strdup (name);
  exists = camel_store_get_folder_info_sync (store, *new_full_name, CAMEL_STORE_FOLDER_INFO_FAST, cancellable, NULL);
  if (exists != NULL)
    {
      camel_folder_info_free (exists);
      g_set_error (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_INVALID,
                   _("Ein Ordner „%s“ existiert bereits."), name);
      return FALSE;
    }

  return revolution_eds_store_rename_folder_sync (store, full_name, *new_full_name, cancellable, error);
}

/* #364: Übernommen aus Evolution 3.56.2 libemail-engine/e-mail-folder-utils.c:997-1066
 * (mail_folder_remove_recursive), gekürzt: Unterordner zuerst (von unten nach oben), je Ordner alle
 * Nachrichten löschen und bereinigen, Abo beenden, Ordner löschen. */
static gboolean
remove_recursive (CamelStore       *store,
                  CamelFolderInfo  *fi,
                  GCancellable     *cancellable,
                  GError          **error)
{
  for (; fi != NULL; fi = fi->next)
    {
      g_autoptr (CamelFolder) folder = NULL;
      GPtrArray *uids;
      gboolean success;

      if (fi->child != NULL && !remove_recursive (store, fi->child, cancellable, error))
        return FALSE;

      folder = camel_store_get_folder_sync (store, fi->full_name, 0, cancellable, error);
      if (folder == NULL)
        return FALSE;

      camel_folder_freeze (folder);
      uids = camel_folder_get_uids (folder);
      for (guint i = 0; i < uids->len; i++)
        camel_folder_delete_message (folder, uids->pdata[i]);
      camel_folder_free_uids (folder, uids);
      success = camel_folder_synchronize_sync (folder, TRUE, cancellable, error);
      camel_folder_thaw (folder);

      if (!success ||
          (CAMEL_IS_SUBSCRIBABLE (store) &&
           !camel_subscribable_unsubscribe_folder_sync (CAMEL_SUBSCRIBABLE (store), fi->full_name, cancellable, error)) ||
          !camel_store_delete_folder_sync (store, fi->full_name, cancellable, error))
        return FALSE;
    }

  return TRUE;
}

/* #364: Löschen mit allen Unterordnern wie Evolution e_mail_folder_remove_sync
 * (libemail-engine/e-mail-folder-utils.c:1080-1171): Stores, die Unterordner selbst mitlöschen
 * (CAMEL_STORE_CAN_DELETE_FOLDERS_AT_ONCE, microsoft365 camel-m365-store.c:109), in einem Aufruf;
 * microsoft365 nimmt dabei den ganzen Teilbaum aus seinem Ordner-Cache (camel-m365-store.c:914), ohne
 * geöffnete Ordner abzugleichen (kein Echo, #416); die Summaries löscht eds-compat.c. Sonst über die Ordnerliste von unten nach oben
 * (remove_recursive), nur der Ordner selbst, nicht seine Geschwister (dort 1131-1150). Im Worker. */
gboolean
revolution_mail_folder_delete_sync (CamelStore    *store,
                                    const char    *full_name,
                                    GCancellable  *cancellable,
                                    GError       **error)
{
  CamelFolderInfo *info, *fi, *next;
  gboolean success;

  if ((camel_store_get_flags (store) & CAMEL_STORE_CAN_DELETE_FOLDERS_AT_ONCE) != 0)
    return revolution_eds_store_delete_folder_sync (store, full_name, cancellable, error);

  info = camel_store_get_folder_info_sync (store, full_name,
                                           CAMEL_STORE_FOLDER_INFO_RECURSIVE | CAMEL_STORE_FOLDER_INFO_SUBSCRIBED,
                                           cancellable, error);
  if (info == NULL)
    return FALSE;

  for (fi = info; fi != NULL && g_strcmp0 (fi->full_name, full_name) != 0; fi = fi->next)
    ;
  if (fi == NULL)
    {
      camel_folder_info_free (info);
      g_set_error (error, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_NO_FOLDER, _("Ordner „%s“ nicht gefunden"), full_name);
      return FALSE;
    }

  next = fi->next;
  fi->next = NULL;
  success = remove_recursive (store, fi, cancellable, error);
  fi->next = next;
  camel_folder_info_free (info);

  return success;
}
