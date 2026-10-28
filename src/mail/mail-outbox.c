/*
 * e-mail-session.c, e-mail-session-utils.c, mail-ops.c, mail-tools.c
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 *
 * Authors:
 *   Jonathon Jongsma <jonathon.jongsma@collabora.co.uk>
 *   Dan Winship <danw@ximian.com>
 *   Jeffrey Stedfast <fejj@ximian.com>
 *   Peter Williams <peterw@ximian.com>
 *   Michael Zucchi <notzed@ximian.com>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 * Copyright (C) 2009 Intel Corporation
 *
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
 * (Kopf ohne Versionsangabe; dafür gilt Evolution 3.56.2 COPYING:1-4, wie mail-session.c.)
 */

/* mail-outbox.c – Postausgang und Senden (#362)
 *
 * Übernommen aus Evolution 3.56.2 src/libemail-engine, gekürzt, ohne GTK:
 *   Postausgang im lokalen Store         e-mail-session.c:824-894 (mail_session_configure_local_store)
 *   Ablegen im Postausgang               e-mail-session-utils.c:123-232 (append_to_local_folder),
 *                                        Flags wie src/mail/em-composer-utils.c:1555-1580
 *   Versanddienst zur Nachricht          e-mail-session-utils.c:1440-1644 (ref_transport…), nur über
 *                                        X-Evolution-Identity
 *   Senden einer Nachricht               mail-ops.c:572-920 (mail_send_message)
 *   Senden des Postausgangs              mail-ops.c:965-1126 (send_queue_exec)
 *   X-Evolution-Köpfe entfernen/zurück   mail-tools.c:217-273
 *   #432 Entwurf löschen, Flags am       e-mail-session-utils.c:257-301, 363-445 (e_mail_session_handle_
 *        Original (Antwort, Weiterl.)    draft_headers_sync, _source_headers_sync), Ordner aus „folder://“
 *                                        e-mail-session.c:2742-2775, e-mail-folder-utils.c:1862-1898;
 *                                        Aufruf nach dem Senden wie mail-ops.c:842-866
 * Geändert gegenüber der Vorlage:
 *   - Nur der Ordner „Outbox“ im lokalen Store (dort Inbox, Drafts, Outbox, Sent, Templates). Store
 *     mit eigener UID REVOLUTION_MAIL_LOCAL_UID ohne ESource unter ~/.local/share/revolution/mail/local
 *     (Architektur 4), nicht Evolutions eingebaute Quelle „local“.
 *   - Jede Nachricht geht erst in den Postausgang, dann sofort raus: Evolution mit
 *     „composer-use-outbox“ und „composer-delay-outbox-flush“ 0 (em-composer-utils.c:1024-1035,
 *     1482-1497). Ohne Rückfrage-Dialog bei Fehler (dort 940-961, Architektur 3): die Nachricht
 *     bleibt im Postausgang, der Fehler kommt ins Sammel-Banner (main.c).
 *   - Kopie nach Gesendet: Ordner nach der Regel der Seitenleiste (#350,
 *     revolution_mail_folder_mark_special) statt e_mail_session_get_fcc_for_message_sync
 *     (e-mail-session-utils.c:1217-1298), siehe outbox_ref_sent_folder. Ohne lokalen Ordner „Sent“
 *     als Rückfall (dort 1290-1295, mail-ops.c:799-836): scheitert die Kopie, bleibt die gesendete
 *     Nachricht mit der Marke „revolution-sent“ im Postausgang (Banner „Gesendet, Kopie in Gesendet
 *     folgt“); der nächste Lauf (F5, Netzwechsel, Store verbunden) legt nur noch die Kopie an. Evolution
 *     legt sie stattdessen in den lokalen „Sent“ und nimmt die Nachricht aus dem Postausgang.
 *   - Versanddienst nur aus X-Evolution-Identity (Revolution setzt nur diesen Kopf); ohne Rückfall auf
 *     X-Evolution-Transport und die Standard-Identität (e-mail-session-utils.c:1579-1641).
 *   - Ohne Filter für ausgehende Nachrichten (Revolution hat keine, mail-session.c
 *     get_filter_driver), ohne X-Evolution-PostTo, ohne e_mail_session_mark_service_used_sync (in
 *     Revolution sendet nur ein Lauf zur Zeit, main.c), ohne Gruppen-Adressen (em_utils_expand_groups),
 *     ohne Statusmeldungen.
 *   - #362 „höchstens einmal“: Vor dem Senden bekommt die Nachricht im Postausgang die Marke
 *     „revolution-sending“, nach dem Senden „revolution-sent“, jeweils sofort gespeichert. Endet das
 *     Programm dazwischen (Absturz, SIGKILL), ist unklar, ob der Server sie angenommen hat. Ein
 *     automatischer Lauf sendet sie dann nicht erneut (Fehler „Versand unterbrochen“ im Banner), nur
 *     ein ausdrücklicher (F5, all = TRUE). Mit „revolution-sent“ wird nur noch nachbereitet. Evolution
 *     hat diese Marken nicht und sendet nach einem Absturz erneut (Kommentar mail-ops.c:874-876).
 *   - Meldungen deutsch wie alle msgids des Projekts; Log „Senden: …“, „Postausgang: …“. */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>
#include <libedataserver/libedataserver.h>

#include "eds-compat.h"
#include "mail-folder.h"
#include "mail-outbox.h"
#include "mail-session.h"

#define OUTBOX_NAME "Outbox"
#define OUTBOX_KEY "revolution-outbox"
#define FLAG_SENDING "revolution-sending"
#define FLAG_SENT "revolution-sent"
#define USER_AGENT ("Revolution " PACKAGE_VERSION)

/* Im Worker (main.c, load_session_thread) wie mail_session_configure_local_store
 * (e-mail-session.c:824-894), dort aus constructed: Store anlegen (Evolution über die eingebaute
 * Quelle „local“, Backend maildir), Pfad setzen, Postausgang anlegen bzw. öffnen und halten. */
gboolean
revolution_mail_outbox_setup (CamelSession  *session,
                              GError       **error)
{
	CamelLocalSettings *local_settings;
	CamelSettings *settings;
	CamelService *service;
	CamelFolder *folder;
	const gchar *data_dir;
	gchar *path;

	service = camel_session_add_service (session, REVOLUTION_MAIL_LOCAL_UID, "maildir",
	                                     CAMEL_PROVIDER_STORE, error);
	if (service == NULL)
		return FALSE;

	settings = camel_service_ref_settings (service);

	data_dir = camel_session_get_user_data_dir (session);
	path = g_build_filename (data_dir, "local", NULL);
	g_mkdir_with_parents (path, 0700);

	local_settings = CAMEL_LOCAL_SETTINGS (settings);
	camel_local_settings_set_path (local_settings, path);

	g_free (path);

	g_object_unref (settings);

	/* Shouldn't need to worry about other mail applications
	 * altering files in our local mail store. */
	g_object_set (service, "need-summary-check", FALSE, NULL);

	folder = camel_store_get_folder_sync (
		CAMEL_STORE (service), OUTBOX_NAME,
		CAMEL_STORE_FOLDER_CREATE, NULL, error);

	if (folder != NULL) {
		g_object_set_data_full (G_OBJECT (service), OUTBOX_KEY, folder, g_object_unref);
		g_debug ("Postausgang: bereit, %d Nachrichten", camel_folder_get_message_count (folder) -
			 camel_folder_get_deleted_message_count (folder));
	}

	g_object_unref (service);

	return folder != NULL;
}

/* Wie e_mail_session_get_local_folder (e-mail-session.c:2453-2470) für E_MAIL_LOCAL_FOLDER_OUTBOX. */
CamelFolder *
revolution_mail_outbox_ref_folder (CamelSession *session)
{
	CamelService *service;
	CamelFolder *folder = NULL;

	service = camel_session_ref_service (session, REVOLUTION_MAIL_LOCAL_UID);
	if (service != NULL) {
		folder = g_object_get_data (G_OBJECT (service), OUTBOX_KEY);
		if (folder != NULL)
			g_object_ref (folder);
		g_object_unref (service);
	}

	return folder;
}

/* mail-tools.c:217-248 */
static CamelNameValueArray *
mail_tool_remove_xevolution_headers (CamelMimeMessage *message)
{
	CamelNameValueArray *orig_headers, *removed_headers = NULL;
	CamelMedium *medium;
	guint ii, len;

	g_return_val_if_fail (CAMEL_IS_MIME_MESSAGE (message), NULL);

	medium = CAMEL_MEDIUM (message);
	orig_headers = camel_medium_dup_headers (medium);
	len = camel_name_value_array_get_length (orig_headers);

	for (ii = 0; ii < len; ii++) {
		const gchar *header_name = NULL, *header_value = NULL;

		if (!camel_name_value_array_get (orig_headers, ii, &header_name, &header_value) || !header_name)
			continue;

		if (g_ascii_strncasecmp (header_name, "X-Evolution", 11) == 0) {
			if (!removed_headers)
				removed_headers = camel_name_value_array_new ();

			camel_name_value_array_append (removed_headers, header_name, header_value);

			camel_medium_remove_header (medium, header_name);
		}
	}

	camel_name_value_array_free (orig_headers);

	return removed_headers;
}

/* mail-tools.c:250-273 */
static void
mail_tool_restore_xevolution_headers (CamelMimeMessage *message,
                                      CamelNameValueArray *headers)
{
	CamelMedium *medium;
	guint ii, len;

	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));

	if (!headers)
		return;

	medium = CAMEL_MEDIUM (message);
	len = camel_name_value_array_get_length (headers);

	for (ii = 0; ii < len; ii++) {
		const gchar *header_name = NULL, *header_value = NULL;

		if (!camel_name_value_array_get (headers, ii, &header_name, &header_value) || !header_name)
			continue;

		camel_medium_add_header (medium, header_name, header_value);
	}
}

/* #432: Ordner aus „folder://STORE-UID/PFAD“ (e-mail-session.c:2742-2775, e-mail-folder-utils.c:1862-1898) und Flags
 * an der Nachricht setzen (e-mail-session-utils.c:290-298, 431-439). Abweichungen wie zuvor composer.c (#360):
 * camel_folder_synchronize_sync statt _message_sync, damit der Server die Änderung gleich erhält; fehlt die Nachricht
 * in der Summary, erst abgleichen (revolution_eds_folder_refresh_info_sync, Echo-Schutz #415); Setzen und Speichern
 * unter revolution_eds_folder_lock (#409) wie mail-view.c set_flags_thread. Kein Expunge (Entwurf: IMAP \Deleted wie
 * Evolution, microsoft365 löscht beim Abgleich, camel-m365-folder.c:1376-1407). Im Worker. */
gboolean
revolution_mail_folder_uri_set_flags_sync (CamelSession *session,
                                           const gchar *folder_uri,
                                           const gchar *uid,
                                           guint32 flags,
                                           GCancellable *cancellable,
                                           GError **error)
{
	CamelURL *url;
	CamelService *service = NULL;
	CamelFolder *folder = NULL;
	CamelMessageInfo *info;
	gboolean success = FALSE;

	url = camel_url_new (folder_uri, error);
	if (url == NULL)
		return FALSE;
	if (g_strcmp0 (url->protocol, "folder") == 0 && url->host != NULL) {
		gchar *store_uid = url->user != NULL && *url->user != '\0' ?
			g_strconcat (url->user, "@", url->host, NULL) : g_strdup (url->host);

		service = camel_session_ref_service (session, store_uid);
		g_free (store_uid);
	}
	if (CAMEL_IS_STORE (service) && url->path != NULL && *url->path == '/') {
		gchar *folder_name = camel_url_decode_path (url->path + 1);

		folder = camel_store_get_folder_sync (CAMEL_STORE (service), folder_name, 0, cancellable, error);
		g_free (folder_name);
	} else {
		g_set_error (error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID, _("Folder %s unavailable"),
		             folder_uri);
	}
	g_clear_object (&service);
	camel_url_free (url);
	if (folder == NULL)
		return FALSE;

	revolution_eds_folder_lock (folder);
	info = camel_folder_get_message_info (folder, uid);
	if (info == NULL && revolution_eds_folder_refresh_info_sync (folder, cancellable, NULL))
		info = camel_folder_get_message_info (folder, uid);
	if (info == NULL) {
		g_set_error (error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID_UID, _("Message %s not in %s"),
		             uid, folder_uri);
	} else {
		camel_folder_set_message_flags (folder, uid, flags, flags);
		success = camel_folder_synchronize_sync (folder, FALSE, cancellable, error);
		g_debug ("Flags: %s %s 0x%x gesetzt, jetzt 0x%x (%s)", folder_uri, uid, flags,
			 camel_message_info_get_flags (info), success ? "abgeglichen" : "Fehler");
		g_object_unref (info);
	}
	revolution_eds_folder_unlock (folder);
	g_object_unref (folder);

	return success;
}

/* e-mail-session-utils.c:257-301 (e_mail_session_handle_draft_headers_sync) */
static gboolean
outbox_handle_draft_headers_sync (CamelSession *session,
                                  CamelMimeMessage *message,
                                  GCancellable *cancellable,
                                  GError **error)
{
	const gchar *folder_uri = camel_medium_get_header (CAMEL_MEDIUM (message), "X-Evolution-Draft-Folder");
	const gchar *message_uid = camel_medium_get_header (CAMEL_MEDIUM (message), "X-Evolution-Draft-Message");

	/* Don't report errors about missing X-Evolution-Draft
	 * headers.  These headers are optional, so their absence
	 * is handled by doing nothing. */
	if (folder_uri == NULL || message_uid == NULL)
		return TRUE;

	if (!revolution_mail_folder_uri_set_flags_sync (session, folder_uri, message_uid,
	                                                CAMEL_MESSAGE_DELETED | CAMEL_MESSAGE_SEEN, cancellable, error))
		return FALSE;
	g_debug ("Entwurf: %s %s als gelöscht markiert (abgeglichen)", folder_uri, message_uid);

	return TRUE;
}

/* e-mail-session-utils.c:363-445 (e_mail_session_handle_source_headers_sync) */
static gboolean
outbox_handle_source_headers_sync (CamelSession *session,
                                   CamelMimeMessage *message,
                                   GCancellable *cancellable,
                                   GError **error)
{
	CamelMedium *medium = CAMEL_MEDIUM (message);
	CamelMessageFlags flags = 0;
	const gchar *folder_uri = camel_medium_get_header (medium, "X-Evolution-Source-Folder");
	const gchar *message_uid = camel_medium_get_header (medium, "X-Evolution-Source-Message");
	const gchar *flag_string = camel_medium_get_header (medium, "X-Evolution-Source-Flags");
	gchar **tokens;
	gchar *string;
	gboolean success;

	/* Don't report errors about missing X-Evolution-Source
	 * headers.  These headers are optional, so their absence
	 * is handled by doing nothing. */
	if (folder_uri == NULL || message_uid == NULL || flag_string == NULL)
		return TRUE;

	/* Convert the flag string to CamelMessageFlags. */
	string = g_strstrip (g_strdup (flag_string));
	tokens = g_strsplit (string, " ", 0);

	for (guint ii = 0; tokens != NULL && tokens[ii] != NULL; ii++) {
		if (g_strcmp0 (tokens[ii], "ANSWERED") == 0)
			flags |= CAMEL_MESSAGE_ANSWERED;
		else if (g_strcmp0 (tokens[ii], "ANSWERED_ALL") == 0)
			flags |= CAMEL_MESSAGE_ANSWERED_ALL;
		else if (g_strcmp0 (tokens[ii], "FORWARDED") == 0)
			flags |= CAMEL_MESSAGE_FORWARDED;
		else if (g_strcmp0 (tokens[ii], "SEEN") == 0)
			flags |= CAMEL_MESSAGE_SEEN;
		else if (*tokens[ii] != '\0')
			g_warning ("Unknown flag '%s' in X-Evolution-Source-Flags", tokens[ii]);
	}
	g_strfreev (tokens);

	success = revolution_mail_folder_uri_set_flags_sync (session, folder_uri, message_uid, flags, cancellable, error);
	if (success)
		g_debug ("Senden: Original %s %s: %s gesetzt", folder_uri, message_uid, string);
	g_free (string);

	return success;
}

static ESourceRegistry *
outbox_ref_registry (CamelSession *session)
{
	ESourceRegistry *registry = NULL;

	g_object_get (session, "registry", &registry, NULL);

	return registry;
}

/* e-mail-session-utils.c:1440-1474 (e_mail_session_ref_transport) */
static CamelService *
outbox_ref_transport (CamelSession *session,
                      ESourceRegistry *registry,
                      const gchar *transport_uid)
{
	ESource *source = NULL;
	CamelService *transport = NULL;
	const gchar *extension_name;

	extension_name = E_SOURCE_EXTENSION_MAIL_TRANSPORT;

	source = e_source_registry_ref_source (registry, transport_uid);

	if (source == NULL)
		goto exit;

	if (!e_source_registry_check_enabled (registry, source))
		goto exit;

	if (!e_source_has_extension (source, extension_name))
		goto exit;

	transport = camel_session_ref_service (session, transport_uid);

	/* Sanity check. */
	if (transport != NULL)
		g_warn_if_fail (CAMEL_IS_TRANSPORT (transport));

exit:
	g_clear_object (&source);

	return transport;
}

/* e-mail-session-utils.c:1478-1510 (mail_session_ref_transport_for_identity) */
static CamelService *
outbox_ref_transport_for_identity (CamelSession *session,
                                   ESourceRegistry *registry,
                                   ESource *source)
{
	ESourceMailSubmission *extension;
	CamelService *transport = NULL;
	const gchar *extension_name;
	gchar *uid;

	extension_name = E_SOURCE_EXTENSION_MAIL_SUBMISSION;

	if (source == NULL)
		return NULL;

	if (!e_source_registry_check_enabled (registry, source))
		return NULL;

	if (!e_source_has_extension (source, extension_name))
		return NULL;

	extension = e_source_get_extension (source, extension_name);
	uid = e_source_mail_submission_dup_transport_uid (extension);

	if (uid != NULL) {
		transport = outbox_ref_transport (session, registry, uid);
		g_free (uid);
	}

	return transport;
}

/* e-mail-session-utils.c:1547-1577 (mail_session_ref_transport_from_x_identity). */
static CamelService *
outbox_ref_transport_for_message (CamelSession *session,
                                  ESourceRegistry *registry,
                                  CamelMimeMessage *message)
{
	CamelService *transport = NULL;
	const gchar *header_value;
	ESource *source;
	gchar *uid;

	/* Check for "X-Evolution-Identity" header. */
	header_value = camel_medium_get_header (CAMEL_MEDIUM (message), "X-Evolution-Identity");
	if (header_value != NULL) {
		uid = g_strstrip (g_strdup (header_value));
		source = e_source_registry_ref_source (registry, uid);
		transport = outbox_ref_transport_for_identity (session, registry, source);
		g_clear_object (&source);
		g_free (uid);
	}

	return transport;
}

/* #362: Gmail legt gesendete Nachrichten selbst in Gesendet ab; EDS prüft das am Host
 * (camel_imapx_store_is_gmail_server, camel-imapx-store.c:2562-2586, ohne installierten Header,
 * nur im Modul libcamelimapx), hier dieselbe Prüfung. */
static gboolean
outbox_is_gmail (CamelService *store)
{
	CamelSettings *settings;
	gboolean is_gmail = FALSE;

	settings = camel_service_ref_settings (store);
	if (CAMEL_IS_NETWORK_SETTINGS (settings)) {
		gchar *host;

		host = camel_network_settings_dup_host (CAMEL_NETWORK_SETTINGS (settings));

		is_gmail = host && (
			camel_strstrcase (host, ".gmail.com") != NULL ||
			camel_strstrcase (host, ".googlemail.com") != NULL);

		g_free (host);
	}

	g_clear_object (&settings);

	return is_gmail;
}

static CamelFolderInfo *
find_sent (CamelFolderInfo *fi)
{
	for (; fi != NULL; fi = fi->next) {
		CamelFolderInfo *found;

		if ((fi->flags & CAMEL_FOLDER_TYPE_MASK) == CAMEL_FOLDER_TYPE_SENT)
			return fi;
		found = find_sent (fi->child);
		if (found != NULL)
			return found;
	}

	return NULL;
}

/* #362, statt e_mail_session_get_fcc_for_message_sync (e-mail-session-utils.c:1217-1298): Gesendet des
 * Kontos, dessen Identität die Nachricht trägt („X-Evolution-Identity“), nach derselben Regel wie die
 * Seitenleiste (#350, revolution_mail_folder_mark_special): ESource „sent-folder“, wenn sie in den Store
 * des Kontos zeigt, sonst die Ersteinrichtung des Stores (Evolution schreibt sie beim ersten Verbinden in
 * die ESource, mail-folder-cache.c:1971-2028; bei den GOA-Konten steht dort sonst folder://local/Sent).
 * „use-sent-folder“ aus wie dort 1066-1067: keine Kopie. NULL ohne Fehler: keine Kopie, auch für Gmail
 * (*out_server_saves): Gmail legt gesendete Nachrichten selbst ab, EDS lässt Gesendet deshalb in der
 * Ersteinrichtung aus (camel-imapx-store.c:2874-2881); imapx setzt den Typ aber aus \Sent
 * (camel-imapx-store.c:540-542), daher die Prüfung outbox_is_gmail vor der Ordnersuche.
 * *out_store: Store des Ordners. */
static CamelFolder *
outbox_ref_sent_folder (CamelSession *session,
                        ESourceRegistry *registry,
                        const gchar *identity_uid,
                        CamelStore **out_store,
                        gboolean *out_server_saves,
                        GCancellable *cancellable,
                        GError **error)
{
	GList *accounts, *link;
	ESource *identity;
	CamelService *store = NULL;
	CamelFolderInfo *info, *sent;
	CamelFolder *folder = NULL;

	*out_store = NULL;
	*out_server_saves = FALSE;

	identity = identity_uid != NULL ? e_source_registry_ref_source (registry, identity_uid) : NULL;
	if (identity == NULL || !e_source_has_extension (identity, E_SOURCE_EXTENSION_MAIL_SUBMISSION) ||
	    !e_source_mail_submission_get_use_sent_folder (
		e_source_get_extension (identity, E_SOURCE_EXTENSION_MAIL_SUBMISSION))) {
		g_clear_object (&identity);
		return NULL;
	}
	g_object_unref (identity);

	accounts = e_source_registry_list_sources (registry, E_SOURCE_EXTENSION_MAIL_ACCOUNT);
	for (link = accounts; link != NULL && store == NULL; link = g_list_next (link)) {
		ESourceMailAccount *extension = e_source_get_extension (link->data, E_SOURCE_EXTENSION_MAIL_ACCOUNT);
		gchar *uid = e_source_mail_account_dup_identity_uid (extension);

		if (g_strcmp0 (uid, identity_uid) == 0)
			store = camel_session_ref_service (session, e_source_get_uid (link->data));
		g_free (uid);
	}
	g_list_free_full (accounts, g_object_unref);

	if (!CAMEL_IS_STORE (store)) {
		g_clear_object (&store);
		return NULL;
	}

	if (outbox_is_gmail (store)) {
		*out_server_saves = TRUE;
		g_object_unref (store);
		return NULL;
	}

	info = camel_store_get_folder_info_sync (CAMEL_STORE (store), NULL,
	                                         CAMEL_STORE_FOLDER_INFO_RECURSIVE |
	                                         CAMEL_STORE_FOLDER_INFO_SUBSCRIBED,
	                                         cancellable, error);
	if (info == NULL) {
		g_object_unref (store);
		return NULL;
	}

	revolution_mail_folder_mark_special (CAMEL_STORE (store), registry, info, cancellable);
	sent = find_sent (info);
	if (sent != NULL)
		folder = camel_store_get_folder_sync (CAMEL_STORE (store), sent->full_name, 0, cancellable, error);
	camel_folder_info_free (info);

	if (folder != NULL)
		*out_store = CAMEL_STORE (store);
	else
		g_object_unref (store);

	return folder;
}

typedef enum {
	SEND_DONE,
	SEND_FAILED,
	SEND_OFFLINE,
	SEND_UNCLEAR,
	SEND_NO_COPY
} SendResult;

static void
outbox_sync (CamelFolder *queue)
{
	GError *local_error = NULL;

	if (!camel_folder_synchronize_sync (queue, FALSE, NULL, &local_error)) {
		g_warning ("Postausgang: nicht gespeichert: %s", local_error->message);
		g_clear_error (&local_error);
	}
}

/* mail-ops.c:572-920 (mail_send_message), gekürzt (Kopf dieser Datei). out_transport_uid: Versanddienst
 * bzw. Identität für das Banner, out_name: Anzeigename. */
static SendResult
mail_send_message (CamelSession *session,
                   ESourceRegistry *registry,
                   CamelFolder *queue,
                   const gchar *uid,
                   gboolean all,
                   gchar **out_transport_uid,
                   gchar **out_name,
                   GCancellable *cancellable,
                   GError **error)
{
	CamelService *service;
	const CamelInternetAddress *iaddr;
	CamelAddress *from, *recipients;
	CamelMessageInfo *info = NULL;
	CamelMessageInfo *queue_info;
	CamelProvider *provider = NULL;
	const gchar *resent_from;
	const gchar *message_id;
	CamelFolder *folder = NULL;
	CamelStore *sent_store = NULL;
	CamelNameValueArray *xev_headers = NULL;
	CamelMimeMessage *message;
	gchar *identity_uid = NULL;
	gint i;
	gsize msg_size;
	GError *local_error = NULL;
	gboolean did_connect = FALSE;
	gboolean sent_message_saved = FALSE;
	gboolean already_sent;
	gboolean request_dsn;
	gint64 started;
	SendResult result = SEND_FAILED;
	static const gchar *normal_recipients[] = {
		CAMEL_RECIPIENT_TYPE_TO,
		CAMEL_RECIPIENT_TYPE_CC,
		CAMEL_RECIPIENT_TYPE_BCC
	};
	static const gchar *resent_recipients[] = {
		CAMEL_RECIPIENT_TYPE_RESENT_TO,
		CAMEL_RECIPIENT_TYPE_RESENT_CC,
		CAMEL_RECIPIENT_TYPE_RESENT_BCC
	};

	message = camel_folder_get_message_sync (
		queue, uid, cancellable, error);
	if (!message) {
		/* #362: Banner auch ohne lesbare Nachricht (main.c). */
		*out_transport_uid = g_strdup (REVOLUTION_MAIL_LOCAL_UID);
		*out_name = g_strdup (_("Outbox"));
		return SEND_FAILED;
	}

	message_id = camel_mime_message_get_message_id (message);
	queue_info = camel_folder_get_message_info (queue, uid);
	already_sent = queue_info != NULL && camel_message_info_get_user_flag (queue_info, FLAG_SENT);

	if (!camel_medium_get_header (CAMEL_MEDIUM (message), "X-Evolution-Is-Redirect"))
		camel_medium_set_header (CAMEL_MEDIUM (message), "User-Agent", USER_AGENT);

	request_dsn = g_strcmp0 (camel_medium_get_header (CAMEL_MEDIUM (message), "X-Evolution-Request-DSN"), "1") == 0;

	identity_uid = g_strdup (camel_medium_get_header (CAMEL_MEDIUM (message), "X-Evolution-Identity"));
	if (identity_uid != NULL)
		g_strstrip (identity_uid);

	/* Do this before removing "X-Evolution" headers. */
	service = outbox_ref_transport_for_message (session, registry, message);
	if (service != NULL) {
		provider = camel_service_get_provider (service);
		*out_transport_uid = g_strdup (camel_service_get_uid (service));
		*out_name = camel_service_dup_display_name (service);
	} else {
		*out_transport_uid = g_strdup (identity_uid != NULL ? identity_uid : REVOLUTION_MAIL_LOCAL_UID);
		*out_name = g_strdup (identity_uid != NULL ? identity_uid : _("Outbox"));
	}

	xev_headers = mail_tool_remove_xevolution_headers (message);

	/* Check for email sending */
	from = (CamelAddress *) camel_internet_address_new ();
	resent_from = camel_medium_get_header (
		CAMEL_MEDIUM (message), "Resent-From");
	if (resent_from != NULL) {
		camel_address_decode (from, resent_from);
	} else {
		iaddr = camel_mime_message_get_from (message);
		camel_address_copy (from, CAMEL_ADDRESS (iaddr));
	}

	recipients = (CamelAddress *) camel_internet_address_new ();
	for (i = 0; i < 3; i++) {
		const gchar *type;

		if (resent_from != NULL)
			type = resent_recipients[i];
		else
			type = normal_recipients[i];
		iaddr = camel_mime_message_get_recipients (message, type);
		camel_address_cat (recipients, CAMEL_ADDRESS (iaddr));
	}

	if (camel_address_length (recipients) > 0 && !already_sent) {
		if (!CAMEL_IS_TRANSPORT (service)) {
			/* e-mail-session-utils.c:504-511 */
			g_set_error (
				&local_error, CAMEL_SERVICE_ERROR,
				CAMEL_SERVICE_ERROR_UNAVAILABLE,
				_("No mail transport for identity %s"),
				identity_uid != NULL ? identity_uid : "?");
			goto exit;
		}
		if (provider && (provider->flags & CAMEL_PROVIDER_IS_REMOTE) != 0 &&
		    !camel_session_get_online (session)) {
			/* silently ignore (mail-ops.c:656-660) */
			g_debug ("Senden: %s offline, bleibt im Postausgang", message_id);
			result = SEND_OFFLINE;
			goto exit;
		}
		/* #362: Abbruch beim letzten Versuch (Kopf dieser Datei). */
		if (queue_info != NULL && camel_message_info_get_user_flag (queue_info, FLAG_SENDING) && !all) {
			g_debug ("Senden: %s Zustellung unklar (Abbruch beim letzten Versuch), bleibt im Postausgang",
				 message_id);
			result = SEND_UNCLEAR;
			goto exit;
		}
		if (camel_service_get_connection_status (service) != CAMEL_SERVICE_CONNECTED) {
			if (!camel_service_connect_sync (service, cancellable, &local_error))
				goto exit;

			did_connect = TRUE;
		}

		camel_transport_set_request_dsn (CAMEL_TRANSPORT (service), request_dsn);

		/* #362: Marke vor dem Senden, gespeichert (Kopf dieser Datei). */
		camel_folder_set_message_user_flag (queue, uid, FLAG_SENDING, TRUE);
		outbox_sync (queue);

		g_debug ("Senden: %s über %s (%s) beginnt", message_id, camel_service_get_uid (service),
			 provider != NULL ? provider->protocol : "?");
		started = g_get_monotonic_time ();
		if (!camel_transport_send_to_sync (
			CAMEL_TRANSPORT (service), message,
			from, recipients, &sent_message_saved, cancellable, &local_error)) {
			/* Der Server hat abgelehnt bzw. die Verbindung ist gescheitert: erneut senden wie
			 * Evolution. */
			camel_folder_set_message_user_flag (queue, uid, FLAG_SENDING, FALSE);
			outbox_sync (queue);
			goto exit;
		}

		camel_folder_set_message_user_flag (queue, uid, FLAG_SENT, TRUE);
		outbox_sync (queue);
		g_debug ("Senden: %s gesendet nach %.0f ms", message_id, (g_get_monotonic_time () - started) / 1000.0);
	} else if (already_sent) {
		g_debug ("Senden: %s schon gesendet, nur Nachbereitung", message_id);
	}

	/* Now check for posting, failures are ignored */
	info = camel_message_info_new_from_headers (NULL, camel_medium_get_headers (CAMEL_MEDIUM (message)));
	msg_size = camel_data_wrapper_calculate_size_sync (CAMEL_DATA_WRAPPER (message), cancellable, NULL);
	if (msg_size != ((gsize) -1))
		camel_message_info_set_size (info, msg_size);
	camel_message_info_set_flags (info, CAMEL_MESSAGE_SEEN |
		(camel_mime_message_has_attachment (message) ? CAMEL_MESSAGE_ATTACHMENTS : 0), ~0);

	/* post process */
	mail_tool_restore_xevolution_headers (message, xev_headers);

	if (sent_message_saved) {
		g_debug ("Senden: %s Kopie in Gesendet legt der Server an", message_id);
	} else if (provider != NULL && g_strcmp0 (provider->protocol, "microsoft365") == 0) {
		/* #362: microsoft365 legt die Nachricht selbst in Gesendet ab, wenn der Gesendet-Ordner der
		 * Identität der des Stores ist (evolution-ews 3.56.2 camel-m365-transport.c:32-115, 320-323:
		 * dann sent_message_saved). Revolution schreibt ihn nicht in die ESource (#350), nimmt aber
		 * genau diesen Ordner als Gesendet (Seitenleiste, Ordnertyp vom Server); daher keine Kopie. */
		g_debug ("Senden: %s Kopie in Gesendet legt der Server an (microsoft365)", message_id);
	} else if (provider == NULL || !(provider->flags & CAMEL_PROVIDER_DISABLE_SENT_FOLDER)) {
		gboolean server_saves;

		folder = outbox_ref_sent_folder (session, registry, identity_uid, &sent_store, &server_saves,
		                                 cancellable, &local_error);

		if (folder != NULL) {
			camel_folder_append_message_sync (
				folder, message, info, NULL,
				cancellable, &local_error);
			if (local_error == NULL)
				g_debug ("Senden: %s Kopie in %s/%s", message_id,
					 camel_service_get_uid (CAMEL_SERVICE (sent_store)),
					 camel_folder_get_full_name (folder));
		} else if (server_saves) {
			g_debug ("Senden: %s Kopie in Gesendet legt der Server an (Gmail)", message_id);
		} else if (local_error == NULL) {
			g_debug ("Senden: %s ohne Gesendet-Ordner, keine Kopie", message_id);
		}

		if (g_error_matches (
			local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			goto exit;

		/* #362: gesendet, Kopie fehlt: mit „revolution-sent“ im Postausgang lassen (Kopf dieser Datei). */
		if (local_error != NULL) {
			g_debug ("Senden: %s Kopie in Gesendet fehlt, bleibt im Postausgang: %s", message_id,
				 local_error->message);
			result = SEND_NO_COPY;
			goto exit;
		}
	}

	/* #432, mail-ops.c:842-866 (cleanup), nach Versand und Kopie; die X-Evolution-Köpfe sind seit „post process“
	 * wieder da. Fehler nur als Warnung, die Nachricht ist gesendet. */
	/* Mark the draft message for deletion, if present. */
	if (!outbox_handle_draft_headers_sync (session, message, cancellable, &local_error)) {
		g_warning ("Senden: %s Entwurf nicht gelöscht: %s", message_id, local_error->message);
		g_clear_error (&local_error);
	}

	/* Set flags on the original source message, if present.
	 * Source message refers to the message being forwarded
	 * or replied to. */
	if (!outbox_handle_source_headers_sync (session, message, cancellable, &local_error) &&
	    !g_error_matches (local_error, CAMEL_FOLDER_ERROR, CAMEL_FOLDER_ERROR_INVALID_UID) &&
	    !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		g_warning ("Senden: %s Flags am Original nicht gesetzt: %s", message_id, local_error->message);
	g_clear_error (&local_error);

	camel_folder_set_message_flags (
		queue, uid, CAMEL_MESSAGE_DELETED |
		CAMEL_MESSAGE_SEEN, ~0);
	/* Sync it to disk, since if it crashes in between,
	 * we keep sending it again on next start. */
	outbox_sync (queue);
	g_debug ("Senden: %s aus dem Postausgang entfernt", message_id);
	result = SEND_DONE;

exit:
	if (did_connect) {
		/* Disconnect regardless of error or cancellation,
		 * but be mindful of these conditions when calling
		 * camel_service_disconnect_sync(). */
		if (g_cancellable_is_cancelled (cancellable)) {
			camel_service_disconnect_sync (service, FALSE, NULL, NULL);
		} else if (local_error != NULL) {
			camel_service_disconnect_sync (service, FALSE, cancellable, NULL);
		} else {
			camel_service_disconnect_sync (service, TRUE, cancellable, NULL);
		}
	}

	if (local_error != NULL) {
		if (result != SEND_NO_COPY)
			g_debug ("Senden: %s fehlgeschlagen: %s", message_id, local_error->message);
		g_propagate_error (error, local_error);
	}

	if (folder != NULL) {
		camel_folder_synchronize_sync (folder, FALSE, NULL, NULL);
		g_object_unref (folder);
	}

	g_clear_object (&sent_store);
	g_clear_object (&queue_info);
	g_clear_object (&info);
	g_clear_object (&service);
	g_free (identity_uid);

	g_object_unref (recipients);
	g_object_unref (from);
	camel_name_value_array_free (xev_headers);
	g_object_unref (message);

	return result;
}

void
revolution_mail_outbox_flush_free (RevolutionMailOutboxFlush *flush)
{
	g_ptr_array_unref (flush->failed_uids);
	g_ptr_array_unref (flush->failed_titles);
	g_ptr_array_unref (flush->failed_details);
	g_ptr_array_unref (flush->sent_uids);
	g_free (flush);
}

/* mail-ops.c:965-1126 (send_queue_exec), im Worker, gekürzt: ohne Verzögerung und „wird bearbeitet“
 * (composer-delay-outbox-flush, MAIL_USER_KEY_EDITING), ohne Statusmeldungen. all: auch Nachrichten
 * mit unklarer Zustellung (Kopf dieser Datei). */
RevolutionMailOutboxFlush *
revolution_mail_outbox_flush_sync (CamelSession *session,
                                   gboolean all,
                                   GCancellable *cancellable)
{
	RevolutionMailOutboxFlush *flush = g_new0 (RevolutionMailOutboxFlush, 1);
	ESourceRegistry *registry = outbox_ref_registry (session);
	CamelFolder *queue = revolution_mail_outbox_ref_folder (session);
	GPtrArray *uids, *send_uids;
	guint i, failed = 0;

	flush->failed_uids = g_ptr_array_new_with_free_func (g_free);
	flush->failed_titles = g_ptr_array_new ();
	flush->failed_details = g_ptr_array_new_with_free_func (g_free);
	flush->sent_uids = g_ptr_array_new_with_free_func (g_free);

	if (queue == NULL || registry == NULL) {
		g_clear_object (&queue);
		g_clear_object (&registry);
		return flush;
	}

	uids = camel_folder_get_uids (queue);
	send_uids = g_ptr_array_new ();
	for (i = 0; uids != NULL && i < uids->len; i++) {
		CamelMessageInfo *info;

		info = camel_folder_get_message_info (queue, uids->pdata[i]);
		if (info) {
			if (!(camel_message_info_get_flags (info) & CAMEL_MESSAGE_DELETED))
				g_ptr_array_add (send_uids, uids->pdata[i]);

			g_clear_object (&info);
		}
	}

	if (send_uids->len > 0)
		g_debug ("Postausgang: %u Nachrichten senden%s", send_uids->len, all ? " (auch unklare)" : "");

	for (i = 0; i < send_uids->len && !g_cancellable_is_cancelled (cancellable); i++) {
		gchar *transport_uid = NULL, *name = NULL;
		GError *local_error = NULL;
		SendResult result;

		result = mail_send_message (session, registry, queue, send_uids->pdata[i], all,
		                            &transport_uid, &name, cancellable, &local_error);

		switch (result) {
			case SEND_DONE:
				flush->sent++;
				g_ptr_array_add (flush->sent_uids, g_steal_pointer (&transport_uid));
				break;
			case SEND_NO_COPY:
				/* Gesendet, die Kopie folgt im nächsten Lauf (bleibt im Postausgang). */
				failed++;
				g_ptr_array_add (flush->failed_uids, g_steal_pointer (&transport_uid));
				g_ptr_array_add (flush->failed_titles, (gpointer) _("Sent, Copy to Sent Pending"));
				g_ptr_array_add (flush->failed_details, g_strdup_printf ("%s: %s", name, local_error->message));
				break;
			case SEND_OFFLINE:
				break;
			case SEND_UNCLEAR:
				failed++;
				g_ptr_array_add (flush->failed_uids, g_steal_pointer (&transport_uid));
				g_ptr_array_add (flush->failed_titles, (gpointer) _("Sending Interrupted"));
				g_ptr_array_add (flush->failed_details, g_strdup_printf (
					_("%s: The program quit while sending, the message may already have been "
					  "delivered. It stays in the Outbox; F5 sends it again, in which case it may "
					  "arrive twice."), name));
				break;
			case SEND_FAILED:
			default:
				if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
					break;
				failed++;
				g_ptr_array_add (flush->failed_titles, (gpointer) _("Not Sent"));
				g_ptr_array_add (flush->failed_uids, g_steal_pointer (&transport_uid));
				g_ptr_array_add (flush->failed_details, g_strdup_printf ("%s: %s", name,
					local_error != NULL ? local_error->message : _("unknown error")));
				break;
		}

		g_clear_error (&local_error);
		g_free (transport_uid);
		g_free (name);
	}

	/* FIXME Not passing a GCancellable or GError here. */
	if (failed == 0 && !g_cancellable_is_cancelled (cancellable))
		camel_folder_synchronize_sync (queue, TRUE, NULL, NULL);

	flush->left = camel_folder_get_message_count (queue) - MAX (camel_folder_get_deleted_message_count (queue), 0);
	if (send_uids->len > 0)
		g_debug ("Postausgang: %u gesendet, %u übrig", flush->sent, flush->left);

	g_ptr_array_free (send_uids, TRUE);
	camel_folder_free_uids (queue, uids);
	g_object_unref (queue);
	g_object_unref (registry);

	return flush;
}

/* #362: Senden für den Composer (#359). Wie em_utils_composer_save_to_outbox_cb und
 * composer_save_to_outbox_completed (em-composer-utils.c:1500-1580): Nachricht mit Flag „gelesen“ im
 * Worker in den Postausgang (e_mail_session_append_to_local_folder, e-mail-session-utils.c:123-232),
 * danach sofort senden (emcu_manage_flush_outbox mit Verzögerung 0, dort 1482-1497:
 * e_mail_session_flush_outbox, Signal „flush-outbox“). Den Kopf X-Evolution-Identity setzt hier die
 * Identität wie Evolutions Composer (e-msg-composer.c:710-712); X-Evolution-Transport und
 * X-Evolution-Fcc nicht (Gesendet nach #350, outbox_ref_sent_folder). Die Session hält die
 * Anwendung (main.c, Daten „session“). Das Ergebnis meldet nur das Ablegen; Versand und Fehler folgen
 * über den Postausgang (Banner). */

typedef struct {
	CamelSession *session;
	CamelMimeMessage *message;
} AppendData;

static void
append_data_free (AppendData *data)
{
	g_clear_object (&data->session);
	g_clear_object (&data->message);
	g_free (data);
}

static gboolean
emit_flush_outbox (gpointer user_data)
{
	revolution_mail_session_flush_outbox (REVOLUTION_MAIL_SESSION (user_data));

	return G_SOURCE_REMOVE;
}

static void
append_to_outbox_thread (GTask *task,
                         gpointer source_object G_GNUC_UNUSED,
                         gpointer task_data,
                         GCancellable *cancellable)
{
	AppendData *data = task_data;
	CamelFolder *folder = revolution_mail_outbox_ref_folder (data->session);
	CamelMessageInfo *info;
	gchar *appended_uid = NULL;
	GError *error = NULL;

	if (folder == NULL) {
		g_task_return_new_error (task, CAMEL_STORE_ERROR, CAMEL_STORE_ERROR_NO_FOLDER,
		                         _("No outbox"));
		return;
	}

	info = camel_message_info_new (NULL);
	camel_message_info_set_flags (info, CAMEL_MESSAGE_SEEN, ~0);

	if (camel_folder_append_message_sync (folder, data->message, info, &appended_uid, cancellable, &error) &&
	    camel_folder_synchronize_sync (folder, FALSE, cancellable, &error)) {
		g_debug ("Postausgang: %s abgelegt (%s)", camel_mime_message_get_message_id (data->message),
		         appended_uid != NULL ? appended_uid : "?");
		g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT, emit_flush_outbox,
		                            g_object_ref (data->session), g_object_unref);
		g_task_return_boolean (task, TRUE);
	} else {
		g_task_return_error (task, error);
	}

	g_free (appended_uid);
	g_object_unref (info);
	g_object_unref (folder);
}

void
revolution_mail_send_message (CamelMimeMessage *message,
                              const char *identity_uid,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data)
{
	CamelSession *session;
	ESourceRegistry *registry;
	ESource *identity;
	AppendData *data;
	GTask *task;

	g_return_if_fail (CAMEL_IS_MIME_MESSAGE (message));
	g_return_if_fail (identity_uid != NULL);

	session = g_object_get_data (G_OBJECT (g_application_get_default ()), "session");
	task = g_task_new (NULL, cancellable, callback, user_data);
	g_task_set_source_tag (task, revolution_mail_send_message);

	registry = session != NULL ? outbox_ref_registry (session) : NULL;
	identity = registry != NULL ? e_source_registry_ref_source (registry, identity_uid) : NULL;
	if (identity == NULL || !e_source_has_extension (identity, E_SOURCE_EXTENSION_MAIL_SUBMISSION)) {
		g_task_return_new_error (task, CAMEL_SERVICE_ERROR, CAMEL_SERVICE_ERROR_UNAVAILABLE,
		                         _("No identity %s"), identity_uid);
		g_clear_object (&identity);
		g_clear_object (&registry);
		g_object_unref (task);
		return;
	}

	camel_medium_set_header (CAMEL_MEDIUM (message), "X-Evolution-Identity", identity_uid);

	data = g_new0 (AppendData, 1);
	data->session = g_object_ref (session);
	data->message = g_object_ref (message);
	g_task_set_task_data (task, data, (GDestroyNotify) append_data_free);
	g_task_run_in_thread (task, append_to_outbox_thread);

	g_object_unref (identity);
	g_object_unref (registry);
	g_object_unref (task);
}

gboolean
revolution_mail_send_message_finish (GAsyncResult *result,
                                     GError **error)
{
	g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);

	return g_task_propagate_boolean (G_TASK (result), error);
}
