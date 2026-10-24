/* mail-account.c – Mail-Konto als Eintrag der Seitenleiste (#345)
 *
 * Oberste Ebene des Ordnerbaums (GtkTreeListModel, CR #397) wie die Konten in
 * Evolution (em-folder-tree-model.c:1943-1990) und Stamp (stamp-folder-list.c:336-338).
 * UID und Anzeigename der ESource mit der Erweiterung „Mail Account“; die
 * Blueprint-Zeilenvorlage (mail-view.blp) bindet an „display-name“. Die Ordner als
 * Kinder folgen mit #348.
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include "mail-account.h"

struct _RevolutionMailAccount
{
  GObject  parent_instance;
  char    *uid;
  char    *display_name;
};

G_DEFINE_FINAL_TYPE (RevolutionMailAccount, revolution_mail_account, G_TYPE_OBJECT)

enum { PROP_0, PROP_UID, PROP_DISPLAY_NAME, N_PROPS };

static GParamSpec *properties[N_PROPS];

static void
revolution_mail_account_get_property (GObject    *object,
                                      guint       prop_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
  RevolutionMailAccount *self = REVOLUTION_MAIL_ACCOUNT (object);

  switch (prop_id)
    {
    case PROP_UID:          g_value_set_string (value, self->uid); break;
    case PROP_DISPLAY_NAME: g_value_set_string (value, self->display_name); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
revolution_mail_account_finalize (GObject *object)
{
  RevolutionMailAccount *self = REVOLUTION_MAIL_ACCOUNT (object);

  g_free (self->uid);
  g_free (self->display_name);

  G_OBJECT_CLASS (revolution_mail_account_parent_class)->finalize (object);
}

static void
revolution_mail_account_class_init (RevolutionMailAccountClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GParamFlags flags = G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;

  object_class->get_property = revolution_mail_account_get_property;
  object_class->finalize = revolution_mail_account_finalize;

  properties[PROP_UID] = g_param_spec_string ("uid", NULL, NULL, NULL, flags);
  properties[PROP_DISPLAY_NAME] = g_param_spec_string ("display-name", NULL, NULL, NULL, flags);
  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
revolution_mail_account_init (G_GNUC_UNUSED RevolutionMailAccount *self)
{
}

RevolutionMailAccount *
revolution_mail_account_new (const char *uid,
                             const char *display_name)
{
  RevolutionMailAccount *self = g_object_new (REVOLUTION_TYPE_MAIL_ACCOUNT, NULL);

  self->uid = g_strdup (uid);
  self->display_name = g_strdup (display_name);

  return self;
}

const char *
revolution_mail_account_get_uid (RevolutionMailAccount *self)
{
  g_return_val_if_fail (REVOLUTION_IS_MAIL_ACCOUNT (self), NULL);

  return self->uid;
}

/* #345: Eingebaute Speicher, die Evolution neben den Konten im Ordnerbaum zeigt:
 * „Auf diesem Rechner“ (local) und „Suchordner“ (vfolder), Evolution e-mail-session.h:58-59
 * (E_MAIL_SESSION_LOCAL_UID, E_MAIL_SESSION_VFOLDER_UID), sowie „Nachrichten und Blogs“
 * (rss, Evolution-Modul rss, e-rss-folder-tree-model-extension.c:66). Revolution hat für
 * keinen davon Funktionen; sie erscheinen erst mit einem eigenen Task. */
static const char * const builtin_uids[] = { "local", "vfolder", "rss", NULL };

/* #345: Welche Mail-Konten Evolution 3.56.2 im Ordnerbaum zeigt. NULL = angezeigt,
 * sonst der Grund fürs Log.
 * 1. Konto unter GNOME/Ubuntu Online Accounts, dessen Mail-Teil aus ist (die Quelle selbst
 *    „disabled“): erscheint nirgends (e-mail-session.c:515-555, 595-600).
 * 2. Sonst entscheidet „enabled“ der umgebenden Collection, ohne Collection die Quelle
 *    selbst (e-mail-account-store.c:1220-1255); der Ordnerbaum zeigt nur aktivierte
 *    (em-folder-tree-model.c:559-574).
 * Die Registry-Aufrufe lesen nur die Quellen im Speicher (EDS e-source-registry.c:459-472,
 * 2576-2604), kein D-Bus, unter Mutex; daher zulässig im UI-Thread (Seitenleiste, README
 * „Worker“) wie im Worker (Session).
 * #346: Dieselbe Regel entscheidet, für welche Konten die Session einen Store registriert
 * (mail-session.c); daher hier statt in mail-view.c. */
const char *
revolution_mail_account_hidden_reason (ESourceRegistry *registry,
                                       ESource         *source)
{
  g_autoptr (ESource) goa = NULL;
  g_autoptr (ESource) uoa = NULL;
  g_autoptr (ESource) collection = NULL;

  if (g_strv_contains (builtin_uids, e_source_get_uid (source)))
    return "eingebauter Speicher";

  /* #415: Nur für Tests (REVOLUTION_TEST_HOOKS=1): Konten aus REVOLUTION_SKIP_ACCOUNTS (UIDs, durch
   * Komma getrennt) gar nicht laden, damit Testläufe ein nur lesend freigegebenes Konto nicht
   * berühren. Wirkt vor dem Anlegen des Stores (mail-session.c, mail_session_sync_stores). */
  if (g_strcmp0 (g_getenv ("REVOLUTION_TEST_HOOKS"), "1") == 0 && g_getenv ("REVOLUTION_SKIP_ACCOUNTS") != NULL)
    {
      g_auto (GStrv) skip = g_strsplit (g_getenv ("REVOLUTION_SKIP_ACCOUNTS"), ",", -1);

      if (g_strv_contains ((const char * const *) skip, e_source_get_uid (source)))
        return "Test: ausgeschlossen";
    }

  goa = e_source_registry_find_extension (registry, source, E_SOURCE_EXTENSION_GOA);
  uoa = e_source_registry_find_extension (registry, source, E_SOURCE_EXTENSION_UOA);
  if ((goa != NULL || uoa != NULL) && !e_source_get_enabled (source))
    return "Online-Konto, Mail aus";

  collection = e_source_registry_find_extension (registry, source, E_SOURCE_EXTENSION_COLLECTION);
  if (!e_source_get_enabled (collection != NULL ? collection : source))
    return collection != NULL ? "Collection deaktiviert" : "deaktiviert";

  return NULL;
}

/* Umbenennen in den Online-Konten bzw. in Evolution (Registry-Signal „source-changed“). */
void
revolution_mail_account_set_display_name (RevolutionMailAccount *self,
                                          const char            *display_name)
{
  g_return_if_fail (REVOLUTION_IS_MAIL_ACCOUNT (self));

  if (g_set_str (&self->display_name, display_name))
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_DISPLAY_NAME]);
}
