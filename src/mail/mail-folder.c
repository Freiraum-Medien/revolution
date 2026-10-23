/* mail-folder.c – Mail-Ordner als Eintrag der Seitenleiste (#348)
 *
 * Kopie der Felder aus CamelFolderInfo (camel_store_get_folder_info_sync, main.c) für den
 * Ordnerbaum (GtkTreeListModel, CR #397). Die Blueprint-Zeilenvorlage (mail-view.blp) bindet
 * an „display-name“ und „icon-name“, der Abschnittskopf an „account“ (Kontoname).
 * Unterordner liegen in „children“ (create_func in mail-view.c), NULL ohne Unterordner.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>

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
};

G_DEFINE_FINAL_TYPE (RevolutionMailFolder, revolution_mail_folder, G_TYPE_OBJECT)

enum { PROP_0, PROP_FULL_NAME, PROP_DISPLAY_NAME, PROP_ICON_NAME, PROP_ACCOUNT, N_PROPS };

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
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
revolution_mail_folder_finalize (GObject *object)
{
  RevolutionMailFolder *self = REVOLUTION_MAIL_FOLDER (object);

  g_free (self->full_name);
  g_free (self->display_name);
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
  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
revolution_mail_folder_init (RevolutionMailFolder *self)
{
  g_weak_ref_init (&self->account, NULL);
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
      g_autoptr (RevolutionMailFolder) folder = g_object_new (REVOLUTION_TYPE_MAIL_FOLDER, NULL);

      folder->full_name = g_strdup (fi->full_name);
      folder->display_name = g_strdup (display_name_for (fi));
      folder->flags = fi->flags;
      folder->icon_name = icon_for_type (fi->flags);
      g_weak_ref_set (&folder->account, account);
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
