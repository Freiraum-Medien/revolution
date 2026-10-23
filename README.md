# Revolution

Mail-Client für GNOME (GTK4, libadwaita, Blueprint, Evolution-Data-Server 3.56). App-ID `de.freiraum_medien.Revolution`, Entwicklungsvariante `de.freiraum_medien.Revolution.Devel`.

## Build und Test

Voraussetzungen (Ubuntu 26.04): meson, ninja, blueprint-compiler, gtk4 ≥ 4.22, libadwaita-1 ≥ 1.9, camel-1.2 und libedataserver-1.2 ≥ 3.56, webkitgtk-6.0 ≥ 2.52, appstreamcli, desktop-file-validate, glib-compile-schemas.

Einrichten und bauen:

```sh
meson setup build                              # Profil default
meson setup build-devel -Dprofile=development  # Entwicklungsvariante (eigene App-ID, Devel-Icon)
meson compile -C build
```

Tests (Desktop-Datei, Metainfo mit `appstreamcli validate --no-net`, GSettings-Schema):

```sh
meson test -C build
```

### Smoke-Test (#349)

Gegen die echten Konten aus GNOME Online Accounts, daher eigene Suite `smoke`, die `meson test` ohne `--suite` auslässt (`add_test_setup` in `meson.build`):

```sh
meson test -C build --suite smoke -v      # oder: tests/smoke.sh [AUSGABEORDNER]
```

Vorbedingungen: Netz, Konten in GOA, echter Session-Bus, Revolution läuft nicht (`NameHasOwner` = false), Pakete wie unter Screenshots. Ohne `meson test` baut das Skript vorher selbst unter `flock` (`BUILD_LOCK`, Standard `build/.build.lock`). Ausgaben (Logs, PNGs, Vergleichslisten) nach `build/smoke/` bzw. in den Ordner aus Argument oder `SMOKE_OUT`; Zeitlimit je Start `SMOKE_TIMEOUT` (Standard 120 s).

Ablauf: vier App-Starts über `scripts/screenshot.sh` (Mail hell und dunkel in 1200×4800 mit allen Konten und Ordnern, Kalender mit Strg+2, Kontakte mit Strg+3) mit `G_MESSAGES_DEBUG=revolution` und `G_DEBUG=fatal-criticals`. Je Start wartet das Skript, bis jedes angezeigte Konto fertig ist (Log `Ordner: <UID> <N> geladen` bzw. Fehlerzeile), nimmt den Screenshot auf und schließt das Fenster per `WM_DELETE_WINDOW`. Geprüft wird je Start:

- (a) Konten: angezeigte Konten = Regel aus `src/mail/mail-account.c`, unabhängig per Python-GI gegen die Registry ausgewertet
- (b) Anmeldung: je Konto `angemeldet: <UID> … angenommen`
- (c) Ordner: Log `Ordner: <UID> <Name>` und Anzahl = `tests/testdaten.py --ordner` (nur lesend)
- (d) Log: Positivliste, nur DEBUG-Zeilen der App; ausgenommen zwei feste `libEGL warning: …DRI3…`-Zeilen von Mesa unter Xvfb, jede andere Zeile ist ein Fehler
- (e) Beenden: Exit 0, kein Rest-Prozess (App, Xvfb, Compositing-Attrappe), App-ID nicht mehr auf dem Bus

Am Ende (f): PIDs von `evolution-source-registry` und `goa-daemon` sowie sha256, Änderungszeit und Inode jeder Datei in `~/.config/evolution/sources` unverändert. Je Prüfung eine Zeile `OK …` bzw. `FEHLER …`, Exit 0 nur ohne Fehler. Gegenprobe ohne Netz (Bus bleibt erreichbar): `unshare -n --map-current-user tests/smoke.sh` meldet FEHLER und Exit 1.

Starten aus dem Build-Ordner: Das Schema liegt kompiliert in `build/data`.

```sh
GSETTINGS_SCHEMA_DIR=build/data build/src/revolution
```

Headless-Start ohne Fenster auf dem Desktop (Anzeige nur Xvfb, X11 erzwungen):

```sh
env -u WAYLAND_DISPLAY GDK_BACKEND=x11 GSETTINGS_SCHEMA_DIR=build/data GSETTINGS_BACKEND=memory \
  xvfb-run -a build/src/revolution
```

Der Start nutzt den echten Session-Bus, keine eigene `dbus-run-session`. Eine eigene Sitzung startete eine zweite `evolution-source-registry` und einen zweiten `goa-daemon` gegen das echte `~/.config`; die zweite Registry schrieb Dateien in `~/.config/evolution/sources` neu (#343). Vorher prüfen, dass Revolution nicht schon läuft, sonst öffnet der Start ein Fenster in der laufenden Instanz auf dem Desktop: `gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus -m org.freedesktop.DBus.NameHasOwner de.freiraum_medien.Revolution` muss `(false,)` liefern.

## Architektur

Muster, keine Hilfsbibliothek (CLAUDE.md Architektur-Festlegungen 1–6). Beispiel im Code: `src/main.c` (#343).

**Worker:** Jeder Camel-/EDS-Aufruf läuft per `GTask` im Worker-Thread, nie im UI-Thread. Der `*_sync`-Aufruf steht in der Thread-Funktion, das Ergebnis kommt im Rückruf im UI-Thread an. Nur dort werden Widgets und Modelle geändert. Vorlage: Evolution `e_mail_session_get_inbox` (libemail-engine/e-mail-session.c:2538-2618). Ausnahme (#345): reine Lesezugriffe der `ESourceRegistry` auf ihre Quellen im Speicher (`e_source_registry_list_sources`, `_ref_source`, `_find_extension`) sind im UI-Thread zulässig; sie lesen eine Hashtabelle unter Mutex, ohne D-Bus (EDS e-source-registry.c:459-472, 2432-2439, 2465-2492, 2576-2600). Evolution macht es ebenso (e-mail-account-store.c:1216-1225). Alles mit D-Bus, Netz oder Datei bleibt im Worker.

```c
static void
load_registry_thread (GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  GError *error = NULL;
  ESourceRegistry *registry = e_source_registry_new_sync (cancellable, &error);

  if (registry == NULL)
    g_task_return_error (task, error);
  else
    g_task_return_pointer (task, registry, g_object_unref);
}

/* Aufruf im UI-Thread; on_registry_loaded läuft wieder im UI-Thread. */
task = g_task_new (app, cancellable, on_registry_loaded, user_data);
g_task_run_in_thread (task, load_registry_thread);
```

Die `ESourceRegistry` wird beim Start so geladen und an der Anwendung gehalten (`g_object_get_data (app, "registry")`). Ihre Signale kommen trotzdem im UI-Thread an: EDS merkt sich beim Anlegen `g_main_context_ref_thread_default ()` (e-source-registry.c:1726), und das ist im GTask-Worker der globale Standard-Kontext. Beleg im Log: `G_MESSAGES_DEBUG=revolution` zeigt Worker- und UI-Thread.

**Datenmodell:** `GListStore` aus konkreten GObject-Items, angezeigt mit `GtkListView`/`GtkColumnView` (Beispiel: Seitenleiste, `src/mail/mail-view.c:359-363`, `mail-view.blp:131`: Konten als `GListStore`, je Konto ein `GtkTreeListModel` über seinen Ordnern (`RevolutionMailFolder`), über `GtkMapListModel` und `GtkFlattenListModel` als Abschnitte mit dem Kontonamen als Kopf, #348). Änderungen am Store (`g_list_store_append`, `g_list_store_splice`) nur im UI-Thread, also im GTask-Rückruf oder in Signalen der Registry (#345). Eine eigene `GListModel`-Klasse erst, wenn ein `GListStore` nicht reicht.

**Camel-Ordner:** Immer über `camel_store_get_folder_sync (store, full_name, …)` holen, nie das Ergebnis von `camel_store_get_inbox_folder_sync` für Anzeige, Flags oder Refresh verwenden. Die Standardversion ruft die vfunc `get_folder_sync` direkt mit „inbox“ und umgeht den Ordner-Cache (EDS camel-store.c:373-388 gegen 1369/1469); imapx überschreibt sie nicht, es entstehen zwei Instanzen. Bei microsoft365 den Posteingang über den Ordnertyp `CAMEL_FOLDER_TYPE_INBOX` finden (camel-m365-store.c:161), nicht über den festen Namen „inbox“ der Standardversion. Evolution macht es ebenso (e-mail-session.c:2763, mail-folder-cache.c:779).

**eds-compat.c:** Die einzige Datei für Aufrufe, die von der EDS-/Camel-Version abhängen (3.56 heute, 3.62 später), jeweils mit `#if EDS_CHECK_VERSION (…)`. Heute gibt es keinen solchen Aufruf; die Datei bricht nur den Build ab, wenn EDS älter als 3.56 ist.

**Verzeichnisse:** Beim Start werden `~/.local/share/revolution` und `~/.cache/revolution` angelegt (`g_get_user_data_dir ()`/`g_get_user_cache_dir ()` + `revolution`, Rechte 0700), getrennt von Evolution (`…/evolution`). Beide Profile nutzen dieselben Ordner, wie Stamp (stamp-helper.c:46-58) und wie das gemeinsame GSettings-Schema.

**Ordner (#348):** Nach der Anmeldung lädt derselbe Worker-GTask je Store die Ordnerliste (`camel_store_get_folder_info_sync`, RECURSIVE|SUBSCRIBED ohne FAST, Begründung in `src/main.c` bei `note_store_thread`); Konten, die die Session zur Laufzeit hinzufügt, laufen über das Signal `services-changed` denselben Weg. Vergleichsliste nur lesend: `tests/testdaten.py --ordner`; App-Log mit `G_MESSAGES_DEBUG=revolution`: `Ordner: <UID> <Name>`. Screenshot aller Ordner: `WAIT=40 scripts/screenshot.sh ordner-hell prefer-light 1200x4800`.

**Fehleranzeige:** Fehler zeigt das `AdwBanner` im Inhaltsbereich unter der Kopfleiste der Nachrichtenliste (`banner` in `src/mail/mail-view.blp`, `revolution_mail_view_show_error` sammelt alle Fehler, seit #352 in `src/mail/mail-view.c`), keine Dialoge: kurzer Titel, Einzelheiten im Tooltip und im Log. Hinweise erscheinen als Pille (Dokument #5 „M2 Specification“). Test: `XDG_DATA_HOME=/proc/revolution-test scripts/screenshot.sh fehler-banner prefer-light`.

**Anmeldung (#347):** Ohne Dialoge. Zugangsdaten kommen in einem Versuch aus GOA (OAuth2-Token über die Registry, Passwort über `ESourceCredentialsProvider`) bzw. aus dem Schlüsselbund (`src/mail/mail-session.c`, Abschnitt „Anmeldung“). Nach dem Start schaltet je Store ein Worker-GTask den Store online und meldet ihn damit an (`src/main.c`). Fehler sammelt ein Banner („Keine Verbindung“, „Anmeldung fehlgeschlagen“, „Zertifikat nicht vertrauenswürdig“), je Konto eine Zeile im Tooltip. Log mit `G_MESSAGES_DEBUG=revolution`: `angemeldet: <UID> <Mechanismus> <Ergebnis>`. Grenze des Unterbaus: Camel speichert jedes neue Zertifikat, auch abgelehnte (`CAMEL_CERT_TRUST_UNKNOWN`), als Datei unter `g_get_user_data_dir ()/camel_certs`, also `~/.local/share/camel_certs`, gemeinsam mit Evolution (EDS camel-network-service.c:252-258, camel-certdb.c:299-311). `camel_init` verlegt nur `camel-cert.db` nach `~/.local/share/revolution`. Prüfen:

```sh
unshare -n --map-current-user env BUILD=build scripts/screenshot.sh fehler-netz prefer-light  # ohne Netz, Bus erreichbar
G_MESSAGES_DEBUG=revolution build/tests/anmeldung UID…                         # Konten anmelden
G_MESSAGES_DEBUG=revolution build/tests/anmeldung --benutzer x UID             # Ablehnung (nur im Speicher)
G_MESSAGES_DEBUG=revolution build/tests/anmeldung --zertifikat UID             # abgelaufenes Zertifikat
```

**Nachrichtenliste (#352):** Die Auswahl eines Ordners lädt dessen Nachrichten in einem Worker-GTask (`src/mail/mail-view.c`, `on_folder_selected`): `camel_store_get_folder_sync`, erst der Stand der Summary im Cache, nach `camel_folder_refresh_info_sync` der Stand des Servers (nur wenn anders), sortiert nach Datum absteigend, in Blöcken zu 25 Einträgen (`RevolutionMailMessage`, `src/mail/mail-message.c`) in den `GListStore`; weicht der Server vom angezeigten Cache ab, gleicht ein Block die Liste ab (nur neue, entfernte und geänderte Einträge, Auswahl und Scrollposition bleiben). Ein anderer Ordner bricht das Laden ab. Die Vorschau liefert Camel (`camel_message_info_get_preview`): imapx über IMAP `PREVIEW`, sofern der Server es kann, microsoft365 über `bodyPreview` von Graph; Revolution lädt keine Nachrichtentexte dafür. Log mit `G_MESSAGES_DEBUG=revolution`: `Nachrichten: <UID> <Ordner> lädt | erste Zeile nach … | Cache/Server <N> (ungelesen …, mit Vorschau …) | vollständig <N> nach …`. Nur für Tests: `REVOLUTION_FOLDER=<UID>/<Ordner>` wählt einen Ordner, `REVOLUTION_FRAME_LOG=1` schreibt die Dauer jedes Frames ins Log.

Lasttest (echtes Konto privat@, 3.000 Mails per APPEND in „Revolution-Last“, `tests/testdaten.py --last 3000`, idempotent): `flock build/.run.lock tests/nachrichtenliste.sh` (Ausgaben und `m4-352-oben.png`, `m4-352-unten.png` in `build/m4-352/`). Zwei Starts: Liste geladen; Liste geladen, Klick in die erste Zeile, 480× Bild ab im Abstand von 20 ms (wie Tastenwiederholung) und Ende. Gemessen: Anzahl, Ladezeiten, Frame-Dauer beim Scrollen, längste Antwortzeit des UI-Threads (`org.gtk.Actions.List` über den Session-Bus alle 20 ms).

**Gelesen, Markieren, Aktualisieren (#353):** Aktionen `message.mark-read` (Strg+K), `message.mark-unread` (Umschalt+Strg+K) wie Evolution, `message.star` (Markieren umschalten, Umschalt+Strg+L wie Apple Mail ⇧⌘L; auch der Stern-Knopf der Nachricht), `message.star-on`/`-off` (Kontextmenü), `list.refresh` (F5, Hauptmenü „Aktualisieren“) und `list.context-menu` (Menü-Taste, Umschalt+F10). Kontextmenü der Zeile per Rechtsklick (`row_menu` in `src/mail/mail-view.blp`), Wischen über eine Zeile: rechts Gelesen/Ungelesen, links Markieren/Entfernen. Die Zeile zeigt die Bedienung sofort; die Flags setzt eine Warteschlange von Worker-GTasks nacheinander (`camel_folder_set_message_flags`, danach `camel_folder_synchronize_sync`; ohne Verbindung bleibt die Änderung in der Summary und geht beim nächsten Abgleich mit). Der Ordner steht für Flags bereit, sobald er geholt ist (vor Cache- und Server-Durchgang). Eine gewählte ungelesene Nachricht wird nach `mark-seen-timeout` (GSettings, 1500 ms wie Evolution) gelesen. Die Liste folgt dem Signal `changed` des CamelFolder (neu, entfernt, geändert am bestehenden Eintrag; Auswahl und Scrollposition bleiben), der Untertitel zählt mit. Log: `Flags: <UID> <Ordner> <Nachricht> <was> [gespeichert nach … | lokal gespeichert (offline)]`, `Nachrichten: … Änderung <N> neu, <N> entfernt, <N> geändert`, `Gelesen: … in <ms> ms`, `Aktualisieren: … fertig`. Grenzen des Unterbaus: imapx startet IDLE bevorzugt am Posteingang, der gewählte Ordner bekommt IDLE nur, wenn der Posteingang schon auf einer anderen Verbindung idlet (EDS camel-imapx-conn-manager.c:1268-1278); EXPUNGE außerhalb von IDLE übergeht imapx (camel-imapx-server.c:789-820), eine anderswo gelöschte Mail bleibt dann bis zum nächsten Start stehen. microsoft365 bildet „markiert“ auf `importance: high` ab, nicht auf die Outlook-Kennzeichnung (camel-m365-utils.c:744-745, camel-m365-folder.c:659-660), und hat keinen Push.

Test gegen die Testkonten (privat@, outlook.com, Gmail; Flags direkt am Server per `tests/testdaten.py --flags`, nur lesend; Ausgangszustand danach wie vorher, im Ordner Revolution-Last per `--ungelesen` geprüft): `flock build/.run.lock tests/flags.sh` (Treiber `tests/flags-treiber.py`, Ausgaben und Screenshots in `build/m4-353/`). Testläufe, die Zeilen wählen, ohne Gelesen zu prüfen, setzen `mark-seen=false` über ein eigenes Schema mit `.gschema.override` (`SCHEMA_DIR` für `scripts/screenshot.sh`, so in `tests/nachrichtenliste.sh`).

**Lesebereich (#354):** Die Auswahl einer Nachricht zeigt Kopf und Inhalt rechts (`src/mail/mail-reader.{c,blp}`). Ein Worker-GTask holt die Nachricht (`camel_folder_get_message_sync`, Cache oder Server) und zerlegt die MIME-Teile: `multipart/alternative` HTML vor Text, `multipart/related` mit Teilen für `cid:`, Text über `camel_text_to_html` wie Evolution, Zeichensatz nach UTF-8 wie Evolution (`iso-8859-*` mit Prüfung auf windows-125x). Anzeige in WebKitGTK 6.0: JavaScript aus, flüchtige Netzwerksitzung, Inhalte nur aus `cid:` (Teile im Speicher), `data:`, `about:`. Jede andere Anfrage sperrt die Web-Process-Extension `src/web-extension/web-extension.c` (`send-request`, wie Stamp und Evolution); die App lädt Mail-Inhalt erst, wenn die Erweiterung auf `revolution-guard` geantwortet hat, sonst Banner „Nachricht nicht angezeigt“. Erweiterung aus `build/src/web-extension`, wenn die App aus dem Build-Ordner läuft, sonst aus `libdir/revolution/web-extensions`. Links öffnet der Standardbrowser. Themes (Dokument #5 „HTML behält eigenen Hintergrund“): Theme „Hell“ zeigt die Mail unverändert; „Dunkel mit hellem Inhalt“ zeigt ihre helle Fassung; „Dunkel“ zeigt die eigene Dunkel-Fassung der Mail, wenn sie eine hat (Medienabfrage `prefers-color-scheme: dark` und `color-scheme` mit „dark“ als `<meta>` oder CSS), sonst die helle Fassung invertiert und im Farbton zurückgedreht (Farben bleiben erkennbar). Ein Theme-Wechsel lädt die Mail neu. Krücke für eine Grenze des Unterbaus: WebKitGTK 2.52.6 setzt `prefers-color-scheme` fensterweit nach GtkSettings `gtk-application-prefer-dark-theme` (keine API je WebView, `<meta name="color-scheme">` wirkt nicht); daher tauscht der Worker in `<style>`-Elementen und `media`-Attributen `prefers-color-scheme: dark` und `light` für die helle Fassung. Log mit `G_MESSAGES_DEBUG=revolution`: `Nachricht: <UID> <Ordner> <Nachricht> geladen nach … (HTML|Text, …, <N> Abfragen prefers-color-scheme, eigene Dunkel-Fassung ja|nein)`, `Nachricht: Sperre der Netzzugriffe aktiv`. Testdaten: `tests/testdaten.py --anzeige` (HTML, Text ISO-8859-1, Inline-Bild, Remote-Inhalte per APPEND in die vier Testkonten). Nur für Tests: `REVOLUTION_MESSAGE=<Teil des Betreffs>` wählt die erste passende Nachricht.

## Screenshots (headless)

`scripts/screenshot.sh NAME SCHEMA [BREITExHÖHE]`. Das Skript läuft in eigenem Xvfb am echten Session-Bus (Grund oben unter Headless-Start) und bricht ab, wenn Revolution dort schon läuft. Einstellungen bleiben dabei im Speicher (`GSETTINGS_BACKEND=memory`). Wichtige Umgebungsvariablen: `BUILD` (Build-Ordner, Standard `build/`), `OUT_DIR` (Zielordner), `KEYS` (Tastendrücke vorab); für den Smoke-Test `LOG` (Ausgabe der App), `READY` (warten, bis die Datei existiert) und `QUIT` (Beenden per `WM_DELETE_WINDOW`, Exit-Code in die Datei), Einzelheiten im Kopf des Skripts.

```sh
scripts/screenshot.sh mail-hell prefer-light
KEYS="Control_L+2" scripts/screenshot.sh kalender-hell prefer-light
KEYS="Control_L+3" scripts/screenshot.sh kontakte-hell prefer-light
```

## Git-Hook

`hooks/pre-merge-commit` baut und testet das Merge-Ergebnis in `build-hook/` mit `werror=true` (`meson compile` und `meson test --no-suite smoke`, also offline ohne den Smoke-Test); jede Compiler-Warnung zählt also als Fehler. Schlägt eins davon fehl, bricht `git merge` ohne Commit ab. Der Merge bleibt dann offen und wird mit `git merge --abort` zurückgenommen. Einmal je Klon aktivieren:

```sh
git config core.hooksPath hooks
git config merge.ff false
```

Lücken: Der Hook läuft nicht bei `git merge --no-verify` und nicht, wenn der Merge mit Konflikt stoppt. Den Konflikt-Commit dann vor dem Abschluss selbst mit `meson configure -Dwerror=true build-hook && meson compile -C build-hook && meson test -C build-hook --no-suite smoke` prüfen.

Arbeitsweise: ein Branch pro Task (`task/<Nr>-<kurz>`), Merge nach `main` erst nach dem Review.
