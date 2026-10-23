/*
 * e-mail-session.c
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
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 * Copyright (C) 2009 Intel Corporation
 *
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only
 * (Kopf ohne Versionsangabe; dafür gilt Evolution 3.56.2 COPYING:1-4, „LGPLv2 or (at
 * your choice) LGPLv3“, COPYING.LGPL2 ist Version 2.1.)
 */

/* mail-session.c – CamelSession der Mail (#346, Anmeldung #347)
 *
 * Übernommen aus Evolution 3.56.2 (src/libemail-engine/e-mail-session.c), gekürzt.
 * Übernommen (Zeilen der Vorlage):
 *   Proxy-Resolver je Dienst                 166-276
 *   mail_session_add_from_source (Stores)    558-652
 *   Eigenschaft „registry“, dispose          1018-1187
 *   constructed (Konten, Registry-Signale)   1212-1271
 *   add_service                              1370-1423
 *   get_oauth2_access_token_sync (#347)      1927-1979
 *   class_init, new, Datenordner             2139-2160, 2411-2428, 2891-2911
 * Abschnitt „Anmeldung“ (#347): authenticate_sync und trust_prompt übernommen aus
 * Evolution 3.56.2 (src/mail/e-mail-ui-session.c), gekürzt; Einzelheiten dort.
 * Weggelassen: Archiv-Ordner, lokaler und Suchordner-Store, Junk-Filter, Auto-Refresh,
 * Outbox-Flush, forward_to, Transporte, Ordner-Cache; sie kommen mit den Folgetasks
 * (#348 Ordner, #362 Postausgang) oder haben in Revolution keinen Nutzer. Damit entfallen alle e-util-Aufrufe
 * (e_passwords_*, e_util_ref_settings, e_signal_connect_notify, e_util_is_running_flatpak)
 * und alle übrigen libemail-engine-Dateien.
 * Geändert gegenüber der Vorlage:
 *   - Welche Konten einen Store bekommen, entscheidet dieselbe Regel wie die Seitenleiste
 *     (revolution_mail_account_hidden_reason, #345) statt nur der GOA/UOA-Prüfung
 *     (e-mail-session.c:516-556, 592-600). Anders als in Evolution bekommen damit auch
 *     deaktivierte Nicht-GOA-Konten und die eingebauten Speicher local/vfolder/rss
 *     keinen Store.
 *   - Die Registry-Signale (dort vier Rückrufe, 654-760) starten einen Abgleich im
 *     Worker-GTask (README „Worker“): add_service legt je Store die Datenbank an
 *     (EDS camel-store.c:516-547). Der Abgleich deckt auch eine (de)aktivierte Collection
 *     ab, deren Mail-Konto selbst kein Signal sendet.
 *   - Datenordner ~/.local/share/revolution/mail und ~/.cache/revolution/mail statt
 *     e_get_user_data_dir () (Architektur-Festlegung 4).
 *   - add_service gibt die Referenz auf die ESource wieder frei (in der Vorlage nicht).
 *   - Ohne camel_service_migrate_files (alte URL-Ordner gibt es in Revolution nicht).
 *   - Anzeigename in der Warnung per e_source_dup_display_name; Cast von e_weak_ref_free
 *     über GCallback (-Wcast-function-type).
 */

#define G_LOG_DOMAIN "revolution"

#include "config.h"

#include <glib/gi18n.h>

#include "mail-account.h"
#include "mail-session.h"

typedef struct _ServiceProxyData ServiceProxyData;

struct _RevolutionMailSession {
	CamelSession parent_instance;

	ESourceRegistry *registry;

	/* #347: Zugangsdaten aus GOA bzw. Schlüsselbund (mail_session_authenticate_sync). */
	ESourceCredentialsProvider *credentials_provider;

	gulong registry_handler_ids[5];

	/* 1, solange ein Abgleich angefordert, aber noch nicht begonnen ist. */
	gint sync_pending;

	/* Registry-Signale können mehrere Abgleiche gleichzeitig starten. */
	GMutex sync_lock;
};

struct _ServiceProxyData {
	ESource *authentication_source;
	gulong auth_source_changed_handler_id;
};

enum {
	PROP_0,
	PROP_REGISTRY
};

G_DEFINE_FINAL_TYPE (RevolutionMailSession, revolution_mail_session, CAMEL_TYPE_SESSION)

static void
service_proxy_data_free (ServiceProxyData *proxy_data)
{
	g_signal_handler_disconnect (
		proxy_data->authentication_source,
		proxy_data->auth_source_changed_handler_id);

	g_clear_object (&proxy_data->authentication_source);

	g_slice_free (ServiceProxyData, proxy_data);
}

static void
mail_session_update_proxy_resolver (CamelService *service,
                                    ESource *authentication_source)
{
	GProxyResolver *proxy_resolver = NULL;
	ESourceAuthentication *extension;
	CamelSession *session;
	ESource *source = NULL;
	gchar *uid;

	session = camel_service_ref_session (service);

	/* #347: Fehler-Fix. Kommt „changed“ erst nach dem Ende der Session, gibt es keine mehr
	 * (Absturz im Prüfprogramm). Evolution fängt NULL über e_mail_session_get_registry
	 * (e-mail-session.c:2432). */
	if (session == NULL)
		return;

	extension = e_source_get_extension (
		authentication_source,
		E_SOURCE_EXTENSION_AUTHENTICATION);

	uid = e_source_authentication_dup_proxy_uid (extension);
	if (uid != NULL) {
		ESourceRegistry *registry;

		registry = REVOLUTION_MAIL_SESSION (session)->registry;
		source = e_source_registry_ref_source (registry, uid);
		g_free (uid);
	}

	if (source != NULL) {
		proxy_resolver = G_PROXY_RESOLVER (source);
		if (!g_proxy_resolver_is_supported (proxy_resolver))
			proxy_resolver = NULL;
	}

	camel_service_set_proxy_resolver (service, proxy_resolver);

	g_clear_object (&session);
	g_clear_object (&source);
}

static void
mail_session_auth_source_changed_cb (ESource *authentication_source,
                                     GWeakRef *service_weak_ref)
{
	CamelService *service;

	service = g_weak_ref_get (service_weak_ref);

	if (service != NULL) {
		mail_session_update_proxy_resolver (
			service, authentication_source);
		g_object_unref (service);
	}
}

static void
mail_session_configure_proxy_resolver (ESourceRegistry *registry,
                                       CamelService *service)
{
	ESource *source;
	ESource *authentication_source;
	const gchar *uid;

	uid = camel_service_get_uid (service);
	source = e_source_registry_ref_source (registry, uid);
	g_return_if_fail (source != NULL);

	authentication_source =
		e_source_registry_find_extension (
		registry, source, E_SOURCE_EXTENSION_AUTHENTICATION);

	if (authentication_source != NULL) {
		ServiceProxyData *proxy_data;
		gulong handler_id;

		mail_session_update_proxy_resolver (
			service, authentication_source);

		handler_id = g_signal_connect_data (
			authentication_source, "changed",
			G_CALLBACK (mail_session_auth_source_changed_cb),
			e_weak_ref_new (service),
			/* Über GCallback, sonst -Wcast-function-type (Revolution baut mit -Werror). */
			(GClosureNotify) (GCallback) e_weak_ref_free, 0);

		/* This takes ownership of the authentication source. */
		proxy_data = g_slice_new0 (ServiceProxyData);
		proxy_data->authentication_source = authentication_source;
		proxy_data->auth_source_changed_handler_id = handler_id;

		/* Tack the proxy data on to the CamelService,
		 * so it's destroyed along with the CamelService. */
		g_object_set_data_full (
			G_OBJECT (service),
			"proxy-data", proxy_data,
			(GDestroyNotify) service_proxy_data_free);
	}

	g_object_unref (source);
}

static void
mail_session_add_from_source (RevolutionMailSession *session,
                              ESource *source)
{
	ESourceBackend *extension;
	CamelService *service;
	const gchar *uid;
	const gchar *backend_name;
	gchar *display_name;
	GError *error = NULL;

	uid = e_source_get_uid (source);

	extension = e_source_get_extension (source, E_SOURCE_EXTENSION_MAIL_ACCOUNT);
	backend_name = e_source_backend_get_backend_name (extension);

	/* Sanity checks. */
	g_return_if_fail (uid != NULL);
	g_return_if_fail (backend_name != NULL);

	service = camel_session_add_service (
		CAMEL_SESSION (session), uid,
		backend_name, CAMEL_PROVIDER_STORE, &error);

	/* Our own CamelSession.add_service() method will handle the
	 * new CamelService, so we only need to unreference it here. */
	if (service != NULL) {
		/* #346: Beleg für die DoD; „UI-Thread“ ist der Besitzer des Standard-Kontexts. */
		g_debug ("Store registriert: %s %s (Thread %p, UI-Thread: %s)",
			 uid, backend_name, (gpointer) g_thread_self (),
			 g_main_context_is_owner (g_main_context_default ()) ? "ja" : "nein");
		g_object_unref (service);
	}

	if (error != NULL) {
		/* Der Anzeigename kann sich im D-Bus-Thread der Registry ändern, daher dup. */
		display_name = e_source_dup_display_name (source);
		g_warning (
			"Failed to add service '%s' (%s): %s",
			display_name, uid, error->message);
		g_free (display_name);
		g_error_free (error);
	}
}

static void
mail_session_remove_service (RevolutionMailSession *session,
                             CamelService *service)
{
	g_debug ("Store entfernt: %s (Thread %p)",
		 camel_service_get_uid (service), (gpointer) g_thread_self ());
	camel_session_remove_service (CAMEL_SESSION (session), service);
}

/* #346: Abgleich Konten → Stores. Ein Store je Mail-Konto, das die Seitenleiste zeigt
 * (revolution_mail_account_hidden_reason); Stores verschwundener oder ausgeblendeter
 * Konten werden entfernt wie in mail_session_source_removed_cb (e-mail-session.c:675-704).
 * Läuft im Worker. */
static void
mail_session_sync_stores (RevolutionMailSession *session)
{
	CamelSession *camel_session = CAMEL_SESSION (session);
	GList *list, *link;

	g_mutex_lock (&session->sync_lock);

	list = e_source_registry_list_sources (session->registry, E_SOURCE_EXTENSION_MAIL_ACCOUNT);

	for (link = list; link != NULL; link = g_list_next (link)) {
		ESource *source = E_SOURCE (link->data);
		CamelService *service;
		gboolean shown;

		service = camel_session_ref_service (camel_session, e_source_get_uid (source));
		shown = revolution_mail_account_hidden_reason (session->registry, source) == NULL;

		if (shown && service == NULL)
			mail_session_add_from_source (session, source);
		else if (!shown && service != NULL)
			mail_session_remove_service (session, service);

		g_clear_object (&service);
	}

	g_list_free_full (list, (GDestroyNotify) g_object_unref);

	list = camel_session_list_services (camel_session);

	for (link = list; link != NULL; link = g_list_next (link)) {
		CamelService *service = CAMEL_SERVICE (link->data);
		ESource *source;

		source = e_source_registry_ref_source (session->registry, camel_service_get_uid (service));
		if (source == NULL)
			mail_session_remove_service (session, service);

		g_clear_object (&source);
	}

	g_list_free_full (list, (GDestroyNotify) g_object_unref);

	g_mutex_unlock (&session->sync_lock);
}

static void
mail_session_sync_thread (GTask *task,
                          gpointer source_object,
                          gpointer task_data G_GNUC_UNUSED,
                          GCancellable *cancellable G_GNUC_UNUSED)
{
	RevolutionMailSession *session = REVOLUTION_MAIL_SESSION (source_object);

	/* Vor dem Abgleich zurücksetzen: ein Signal ab jetzt fordert einen neuen an. */
	g_atomic_int_set (&session->sync_pending, 0);
	mail_session_sync_stores (session);
	g_task_return_boolean (task, TRUE);
}

/* Registry-Signale kommen im UI-Thread an (README „Architektur“). Ist schon ein Abgleich
 * angefordert, deckt er dieses Signal mit ab („source-changed“ kommt auch für Kalender
 * und Adressbücher). */
static void
mail_session_registry_changed_cb (RevolutionMailSession *session)
{
	GTask *task;

	if (!g_atomic_int_compare_and_exchange (&session->sync_pending, 0, 1))
		return;

	task = g_task_new (session, NULL, NULL, NULL);
	g_task_set_source_tag (task, mail_session_registry_changed_cb);
	g_task_run_in_thread (task, mail_session_sync_thread);
	g_object_unref (task);
}

static void
mail_session_set_registry (RevolutionMailSession *session,
                           ESourceRegistry *registry)
{
	g_return_if_fail (E_IS_SOURCE_REGISTRY (registry));
	g_return_if_fail (session->registry == NULL);

	session->registry = g_object_ref (registry);
}

static void
mail_session_set_property (GObject *object,
                           guint property_id,
                           const GValue *value,
                           GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_REGISTRY:
			mail_session_set_registry (
				REVOLUTION_MAIL_SESSION (object),
				g_value_get_object (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
mail_session_get_property (GObject *object,
                           guint property_id,
                           GValue *value,
                           GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_REGISTRY:
			g_value_set_object (
				value,
				REVOLUTION_MAIL_SESSION (object)->registry);
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
mail_session_dispose (GObject *object)
{
	RevolutionMailSession *self = REVOLUTION_MAIL_SESSION (object);

	if (self->registry != NULL) {
		for (gsize ii = 0; ii < G_N_ELEMENTS (self->registry_handler_ids); ii++) {
			if (self->registry_handler_ids[ii] != 0)
				g_signal_handler_disconnect (
					self->registry,
					self->registry_handler_ids[ii]);
			self->registry_handler_ids[ii] = 0;
		}

		g_clear_object (&self->registry);
	}

	g_clear_object (&self->credentials_provider);

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (revolution_mail_session_parent_class)->dispose (object);
}

static void
mail_session_finalize (GObject *object)
{
	RevolutionMailSession *self = REVOLUTION_MAIL_SESSION (object);

	g_mutex_clear (&self->sync_lock);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (revolution_mail_session_parent_class)->finalize (object);
}

static void
mail_session_constructed (GObject *object)
{
	/* Wie Evolution source-added/-removed/-enabled/-disabled (e-mail-session.c:1253-1271);
	 * „source-changed“ dazu wie die Seitenleiste (#345, mail-view.c). */
	const gchar *signals[] = {
		"source-added", "source-removed", "source-enabled",
		"source-disabled", "source-changed"
	};
	RevolutionMailSession *session;

	G_STATIC_ASSERT (G_N_ELEMENTS (signals) == G_N_ELEMENTS (session->registry_handler_ids));

	session = REVOLUTION_MAIL_SESSION (object);

	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (revolution_mail_session_parent_class)->constructed (object);

	camel_session_set_network_monitor (CAMEL_SESSION (session), e_network_monitor_get_default ());

	/* #347: einmal je Session wie der Prompter in Evolution (e-credentials-prompter.c,
	 * Eigenschaft „provider“); lädt beim ersten Mal die Module (module-credentials-goa). */
	session->credentials_provider = e_source_credentials_provider_new (session->registry);

	/* Listen for registry changes. Vor dem ersten Abgleich, damit kein Signal dazwischen
	 * verloren geht; der Mutex in mail_session_sync_stores ordnet die Abgleiche. */
	for (gsize ii = 0; ii < G_N_ELEMENTS (signals); ii++)
		session->registry_handler_ids[ii] = g_signal_connect_swapped (
			session->registry, signals[ii],
			G_CALLBACK (mail_session_registry_changed_cb), session);

	/* Add available mail accounts. */
	mail_session_sync_stores (session);
}

static CamelService *
mail_session_add_service (CamelSession *session,
                          const gchar *uid,
                          const gchar *protocol,
                          CamelProviderType type,
                          GError **error)
{
	ESourceRegistry *registry;
	CamelService *service;
	const gchar *extension_name;

	registry = REVOLUTION_MAIL_SESSION (session)->registry;
	extension_name = e_source_camel_get_extension_name (protocol);

	/* Chain up to parents add_service() method. */
	service = CAMEL_SESSION_CLASS (revolution_mail_session_parent_class)->
		add_service (session, uid, protocol, type, error);

	/* Configure the CamelService from the corresponding ESource. */

	if (CAMEL_IS_SERVICE (service)) {
		ESource *source;
		ESource *tmp_source;

		/* Each CamelService has a corresponding ESource. */
		source = e_source_registry_ref_source (registry, uid);
		g_return_val_if_fail (source != NULL, service);

		tmp_source = e_source_registry_find_extension (
			registry, source, extension_name);
		if (tmp_source != NULL) {
			g_object_unref (source);
			source = tmp_source;
		}

		/* This handles all the messy property bindings. */
		e_source_camel_configure_service (source, service);

		/* Track the proxy resolver for this service. */
		mail_session_configure_proxy_resolver (registry, service);

		e_binding_bind_property (
			source, "display-name",
			service, "display-name",
			G_BINDING_SYNC_CREATE);

		g_object_unref (source);
	}

	return service;
}

/* Übernommen aus e-mail-session.c:1927-1979 (#347). Token über die ESourceRegistry vom
 * goa-daemon; Aufrufer ist die XOAUTH2-SASL (EDS camel-sasl-xoauth2.c:82). */
static gboolean
mail_session_get_oauth2_access_token_sync (CamelSession *session,
					   CamelService *service,
					   gchar **out_access_token,
					   gint *out_expires_in,
					   GCancellable *cancellable,
					   GError **error)
{
	RevolutionMailSession *mail_session;
	ESource *source, *cred_source;
	GError *local_error = NULL;
	gboolean success;

	g_return_val_if_fail (REVOLUTION_IS_MAIL_SESSION (session), FALSE);
	g_return_val_if_fail (CAMEL_IS_SERVICE (service), FALSE);

	mail_session = REVOLUTION_MAIL_SESSION (session);
	source = e_source_registry_ref_source (mail_session->registry, camel_service_get_uid (service));
	if (!source) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			_("Keine Datenquelle für den Dienst mit UID „%s“"),
			camel_service_get_uid (service));

		return FALSE;
	}

	cred_source = e_source_registry_find_extension (mail_session->registry, source, E_SOURCE_EXTENSION_COLLECTION);
	if (cred_source && !e_util_can_use_collection_as_credential_source (cred_source, source)) {
		g_clear_object (&cred_source);
	}

	success = e_source_get_oauth2_access_token_sync (cred_source ? cred_source : source, cancellable, out_access_token, out_expires_in, &local_error);

	/* The Connection Refused error can be returned when the OAuth2 token is expired or
	   when its refresh failed for some reason. In that case change the error domain/code,
	   thus the other Camel/mail code understands it. */
	if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED) ||
	    g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
		local_error->domain = CAMEL_SERVICE_ERROR;
		local_error->code = CAMEL_SERVICE_ERROR_CANT_AUTHENTICATE;

		e_source_invoke_credentials_required_sync (cred_source ? cred_source : source,
			E_SOURCE_CREDENTIALS_REASON_REJECTED, NULL, 0, local_error, cancellable, NULL);
	}

	if (local_error)
		g_propagate_error (error, local_error);

	g_clear_object (&cred_source);
	g_object_unref (source);

	return success;
}

/* Anmeldung (#347)
 *
 * Übernommen aus Evolution 3.56.2 (src/mail/e-mail-ui-session.c), gekürzt:
 *   mail_ui_session_trust_prompt             773-822
 *   mail_ui_session_try_credentials_sync     839-892
 *   mail_ui_session_authenticate_sync        895-1055
 * Lizenz und Urheber wie der Kopf dieser Datei (e-mail-ui-session.c:1-22, gleicher Text).
 * Geändert gegenüber der Vorlage (Freigabe Arnd 23.10.2026, Kommentar an #347):
 *   - Kein ECredentialsPrompter und kein Trust-Dialog (e-trust-prompt.c; beide GTK, Dialoge;
 *     Architektur-Festlegung 3). Zugangsdaten kommen in einem Versuch aus dem
 *     ESourceCredentialsProvider (libedataserver): GOA-Passwort (module-credentials-goa)
 *     bzw. Schlüsselbund. Das ist der erste Durchlauf des Prompters ohne Dialog
 *     (EDS e-credentials-prompter.c:1771-1780). Keine Schleife: ein abgelehntes
 *     Passwort ergibt genau einen Fehlversuch und einen Fehler für das Banner.
 *   - Mechanismus „Microsoft365“: ein Versuch wie need_password=FALSE (dort 924-927).
 *     Den SASL-Typ registriert nur das Evolution-UI-Modul (evolution-ews
 *     module-m365-configuration.c:30), camel_sasl_authtype kennt ihn hier nicht; der
 *     Store fordert ihn selbst an (camel-m365-store.c:532).
 *   - Ohne Nachfrage bei abgelehntem OAuth2 (dort 929-962): GOA-Konten erneuert man in
 *     den GNOME-Online-Konten.
 *   - Ohne e_source_invoke_authenticate_sync nach Erfolg (dort 879-889): es reicht neu
 *     eingegebene Zugangsdaten an die Registry weiter; ohne Eingabe gibt es keine.
 *   - trust_prompt fragt nicht: CAMEL_CERT_TRUST_UNKNOWN wie dort der Vorgabezweig
 *     (816-818); Camel lehnt das Zertifikat ab (EDS camel-network-service.c:250-270),
 *     das Banner meldet „Zertifikat nicht vertrauenswürdig“ (main.c).
 *   - Fehler aus der Suche nach Zugangsdaten werden CAMEL_SERVICE_ERROR_CANT_AUTHENTICATE
 *     wie in get_oauth2_access_token_sync (Banner „Anmeldung fehlgeschlagen“); ein
 *     Misserfolg ohne Fehlermeldung bekommt eine.
 *   - Meldungen deutsch wie alle msgids des Projekts (auch in get_oauth2_access_token_sync).
 *   - Ergebnis je Anmeldung im Log („angemeldet: <UID> <Mechanismus> <Ergebnis>“). */

static CamelCertTrust
mail_session_trust_prompt (CamelSession *session G_GNUC_UNUSED,
                           CamelService *service,
                           GTlsCertificate *certificate G_GNUC_UNUSED,
                           GTlsCertificateFlags errors)
{
	g_debug ("Zertifikat abgelehnt: %s (Fehler 0x%x)", camel_service_get_uid (service), errors);

	return CAMEL_CERT_TRUST_UNKNOWN;
}

static CamelAuthenticationResult
mail_session_try_credentials_sync (RevolutionMailSession *session,
                                   CamelService *service,
                                   ESource *source,
                                   const gchar *mechanism,
                                   GCancellable *cancellable,
                                   GError **error)
{
	ENamedParameters *credentials = NULL;
	gchar *credential_name = NULL;
	GError *local_error = NULL;

	if (!e_source_credentials_provider_lookup_sync (session->credentials_provider, source,
		cancellable, &credentials, &local_error)) {
		/* Wie get_oauth2_access_token_sync: als Anmeldefehler, damit das Banner
		 * „Anmeldung fehlgeschlagen“ zeigt (z. B. GOA-Passwort fehlt). */
		if (local_error != NULL && !g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			local_error->domain = CAMEL_SERVICE_ERROR;
			local_error->code = CAMEL_SERVICE_ERROR_CANT_AUTHENTICATE;
		}
		if (local_error != NULL)
			g_propagate_error (error, local_error);
		return CAMEL_AUTHENTICATION_ERROR;
	}

	if (e_source_has_extension (source, E_SOURCE_EXTENSION_AUTHENTICATION)) {
		ESourceAuthentication *auth_extension;

		auth_extension = e_source_get_extension (source, E_SOURCE_EXTENSION_AUTHENTICATION);
		credential_name = e_source_authentication_dup_credential_name (auth_extension);

		if (!credential_name || !*credential_name) {
			g_free (credential_name);
			credential_name = NULL;
		}
	}

	camel_service_set_password (service, e_named_parameters_get (credentials,
		credential_name ? credential_name : E_SOURCE_CREDENTIAL_PASSWORD));

	g_free (credential_name);
	e_named_parameters_free (credentials);

	return camel_service_authenticate_sync (service, mechanism, cancellable, error);
}

static const gchar *
mail_session_result_name (CamelAuthenticationResult result)
{
	switch (result) {
		case CAMEL_AUTHENTICATION_ACCEPTED:
			return "angenommen";
		case CAMEL_AUTHENTICATION_REJECTED:
			return "abgelehnt";
		case CAMEL_AUTHENTICATION_ERROR:
		default:
			return "Fehler";
	}
}

static gboolean
mail_session_authenticate_sync (CamelSession *session,
				CamelService *service,
				const gchar *mechanism,
				GCancellable *cancellable,
				GError **error)
{
	RevolutionMailSession *mail_session = REVOLUTION_MAIL_SESSION (session);
	ESource *source;
	CamelServiceAuthType *authtype = NULL;
	CamelAuthenticationResult result;
	const gchar *uid;
	gboolean try_empty_password = FALSE;
	GError *local_error = NULL;

	/* Do not chain up.  Camel's default method is only an example for
	 * subclasses to follow.  Instead we mimic most of its logic here. */

	uid = camel_service_get_uid (service);

	/* Treat a mechanism name of "none" as NULL. */
	if (g_strcmp0 (mechanism, "none") == 0)
		mechanism = NULL;

	/* APOP is one case where a non-SASL mechanism name is passed, so
	 * don't bail if the CamelServiceAuthType struct comes back NULL. */
	if (mechanism != NULL)
		authtype = camel_sasl_authtype (mechanism);

	/* If the SASL mechanism does not involve a user
	 * password, then it gets one shot to authenticate. */
	if ((authtype != NULL && !authtype->need_password) ||
	    g_strcmp0 (mechanism, "Microsoft365") == 0) {
		result = camel_service_authenticate_sync (service, mechanism, cancellable, &local_error);
	} else {
		/* Some SASL mechanisms can attempt to authenticate without a
		 * user password being provided (e.g. single-sign-on credentials),
		 * but can fall back to a user password.  Handle that case next. */
		if (mechanism != NULL) {
			CamelProvider *provider;
			CamelSasl *sasl;
			const gchar *service_name;

			provider = camel_service_get_provider (service);
			service_name = provider->protocol;

			sasl = camel_sasl_new (service_name, mechanism, service);
			if (sasl != NULL) {
				try_empty_password =
					camel_sasl_try_empty_password_sync (
					sasl, cancellable, &local_error);
				g_object_unref (sasl);
			}
		}

		/* Abort authentication if we got cancelled.
		 * Otherwise clear any errors and press on. */
		if (g_error_matches (local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_propagate_error (error, local_error);
			return FALSE;
		}

		g_clear_error (&local_error);

		/* Find a matching ESource for this CamelService. */
		source = e_source_registry_ref_source (mail_session->registry, uid);

		if (source == NULL) {
			g_set_error (
				error, CAMEL_SERVICE_ERROR,
				CAMEL_SERVICE_ERROR_CANT_AUTHENTICATE,
				_("Keine Datenquelle für UID „%s“"), uid);
			return FALSE;
		}

		result = CAMEL_AUTHENTICATION_REJECTED;

		if (try_empty_password)
			result = camel_service_authenticate_sync (
				service, mechanism, cancellable, &local_error);

		/* We need a password: one lookup, no prompt (see above). */
		if (result == CAMEL_AUTHENTICATION_REJECTED && local_error == NULL)
			result = mail_session_try_credentials_sync (
				mail_session, service, source, mechanism, cancellable, &local_error);

		g_object_unref (source);
	}

	g_debug ("angemeldet: %s %s %s", uid, mechanism ? mechanism : "none",
		 mail_session_result_name (result));

	if (local_error != NULL) {
		g_propagate_error (error, local_error);
		return FALSE;
	}

	if (result != CAMEL_AUTHENTICATION_ACCEPTED && mechanism != NULL)
		g_set_error (
			error, CAMEL_SERVICE_ERROR,
			CAMEL_SERVICE_ERROR_CANT_AUTHENTICATE,
			_("Anmeldung mit %s fehlgeschlagen"), mechanism);
	else if (result != CAMEL_AUTHENTICATION_ACCEPTED)
		g_set_error_literal (
			error, CAMEL_SERVICE_ERROR,
			CAMEL_SERVICE_ERROR_CANT_AUTHENTICATE,
			_("Anmeldung mit Passwort fehlgeschlagen"));

	return (result == CAMEL_AUTHENTICATION_ACCEPTED);
}

static void
revolution_mail_session_class_init (RevolutionMailSessionClass *class)
{
	GObjectClass *object_class;
	CamelSessionClass *session_class;

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = mail_session_set_property;
	object_class->get_property = mail_session_get_property;
	object_class->dispose = mail_session_dispose;
	object_class->finalize = mail_session_finalize;
	object_class->constructed = mail_session_constructed;

	session_class = CAMEL_SESSION_CLASS (class);
	session_class->add_service = mail_session_add_service;
	session_class->get_oauth2_access_token_sync = mail_session_get_oauth2_access_token_sync;
	session_class->trust_prompt = mail_session_trust_prompt;
	session_class->authenticate_sync = mail_session_authenticate_sync;

	g_object_class_install_property (
		object_class,
		PROP_REGISTRY,
		g_param_spec_object (
			"registry",
			"Registry",
			"Data source registry",
			E_TYPE_SOURCE_REGISTRY,
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT_ONLY |
			G_PARAM_STATIC_STRINGS));
}

static void
revolution_mail_session_init (RevolutionMailSession *session)
{
	g_mutex_init (&session->sync_lock);
}

/* Im Worker aufrufen: registriert die Stores der Konten (add_service, Datenbank je Store). */
RevolutionMailSession *
revolution_mail_session_new (ESourceRegistry *registry)
{
	RevolutionMailSession *session;
	gchar *user_data_dir;
	gchar *user_cache_dir;

	g_return_val_if_fail (E_IS_SOURCE_REGISTRY (registry), NULL);

	/* Wie mail_session_get_data_dir/_cache_dir (e-mail-session.c:2891-2911), unter
	 * „revolution“ statt „evolution“. */
	user_data_dir = g_build_filename (g_get_user_data_dir (), "revolution", "mail", NULL);
	g_mkdir_with_parents (user_data_dir, 0700);
	user_cache_dir = g_build_filename (g_get_user_cache_dir (), "revolution", "mail", NULL);
	g_mkdir_with_parents (user_cache_dir, 0700);

	session = g_object_new (
		REVOLUTION_TYPE_MAIL_SESSION,
		"user-data-dir", user_data_dir,
		"user-cache-dir", user_cache_dir,
		"registry", registry,
		NULL);

	g_free (user_data_dir);
	g_free (user_cache_dir);

	return session;
}
