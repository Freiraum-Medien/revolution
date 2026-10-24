/* eds-compat.c
 *
 * Copyright 2026 Arnd Messer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Die einzige Stelle für Aufrufe, die von der EDS-/Camel-Version abhängen
 * (CLAUDE.md Architektur-Festlegung 6, README „Architektur“).
 * EDS_CHECK_VERSION: /usr/include/evolution-data-server/libedataserver/eds-version.h:63. */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <libedataserver/libedataserver.h>

#include "eds-compat.h"

#if !EDS_CHECK_VERSION (3, 56, 0)
#error "Revolution braucht Evolution-Data-Server 3.56 oder neuer"
#endif

/* folder_flagged der Nachricht selbst: camel_folder_summary_get_info_flags führt es bei Änderungen
 * der Kategorien (camel_message_info_set_user_flag) nicht nach (gemessen, #415). */
static CamelMessageInfo *
ref_if_folder_flagged (CamelFolderSummary *summary,
                       const char         *uid)
{
  CamelMessageInfo *info = camel_folder_summary_get (summary, uid);

  if (info != NULL && !camel_message_info_get_folder_flagged (info))
    g_clear_object (&info);

  return info;
}

/* #415: camel_folder_refresh_info_sync, bei microsoft365 ohne Rückschreiben dessen, was der Server
 * gerade geliefert hat. evolution-ews 3.56.2 übernimmt Server-Änderungen an vorhandenen Nachrichten
 * mit camel_message_info_set_flags (camel-m365-folder.c:902-904, 666-674), das folder_flagged setzt
 * (camel-message-info.c:1856-1862); der nächste Sync (Camel-Zeitgeber store-changes-interval,
 * camel-folder.c:309-313 und 236-249, oder eine eigene Bedienung im selben Ordner, mail-view.c
 * set_flags_thread) schickt sie dann als PATCH zurück (camel-m365-folder.c:1341, 1385-1388), aus
 * Teilobjekten des Deltas mit „categories“:[] und „importance“ normal. EWS und imapx setzen
 * folder_flagged nach dem Server-Abgleich zurück (evolution-ews camel-ews-utils.c:1004-1019, EDS
 * camel-imapx-utils.c:394); dasselbe hier: was vorher nicht lokal geändert war, gilt danach als
 * gespeichert. Restrisiko: Eine eigene Flag-Änderung an einer vorher unveränderten Nachricht, die
 * während des Abgleichs gesetzt und noch nicht gespeichert ist, wird mit zurückgesetzt und bleibt
 * dann nur lokal. Nur über öffentliche Camel-API (die Server-Flags des Providers sind nicht
 * öffentlich). Im Worker. */
gboolean
revolution_eds_folder_refresh_info_sync (CamelFolder   *folder,
                                         GCancellable  *cancellable,
                                         GError       **error)
{
  CamelStore *store = camel_folder_get_parent_store (folder);
  CamelProvider *provider = store != NULL ? camel_service_get_provider (CAMEL_SERVICE (store)) : NULL;
  CamelFolderSummary *summary = camel_folder_get_folder_summary (folder);
  g_autoptr (GHashTable) before = NULL;
  GPtrArray *changed;
  gboolean ok;
  guint reset = 0;

  if (provider == NULL || summary == NULL || g_strcmp0 (provider->protocol, "microsoft365") != 0)
    return camel_folder_refresh_info_sync (folder, cancellable, error);

  before = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  changed = camel_folder_summary_get_changed (summary);
  for (guint i = 0; i < changed->len; i++)
    {
      g_autoptr (CamelMessageInfo) info = ref_if_folder_flagged (summary, changed->pdata[i]);

      if (info != NULL)
        g_hash_table_add (before, g_strdup (changed->pdata[i]));
    }
  camel_folder_summary_free_array (changed);

  ok = camel_folder_refresh_info_sync (folder, cancellable, error);

  changed = camel_folder_summary_get_changed (summary);
  for (guint i = 0; i < changed->len; i++)
    {
      const char *uid = changed->pdata[i];
      g_autoptr (CamelMessageInfo) info = NULL;

      if (g_hash_table_contains (before, uid))
        continue;
      info = ref_if_folder_flagged (summary, uid);
      if (info != NULL && camel_message_info_set_folder_flagged (info, FALSE))
        reset++;
    }
  camel_folder_summary_free_array (changed);

  if (reset > 0)
    g_debug ("Abgleich: %s %s: %u Server-Änderungen nicht zurückgeschrieben",
             camel_service_get_uid (CAMEL_SERVICE (store)), camel_folder_get_full_name (folder), reset);

  return ok;
}
