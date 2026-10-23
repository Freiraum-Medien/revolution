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

Starten aus dem Build-Ordner: Das Schema liegt kompiliert in `build/data`.

```sh
GSETTINGS_SCHEMA_DIR=build/data build/src/revolution
```

Headless-Start ohne Fenster auf dem Desktop:

```sh
GSETTINGS_SCHEMA_DIR=build/data GSETTINGS_BACKEND=memory \
  xvfb-run -a dbus-run-session -- build/src/revolution
```

## Architektur

Muster, keine Hilfsbibliothek (CLAUDE.md Architektur-Festlegungen 1–6). Beispiel im Code: `src/main.c` (#343).

**Worker:** Jeder Camel-/EDS-Aufruf läuft per `GTask` im Worker-Thread, nie im UI-Thread. Der `*_sync`-Aufruf steht in der Thread-Funktion, das Ergebnis kommt im Rückruf im UI-Thread an. Nur dort werden Widgets und Modelle geändert. Vorlage: Evolution `e_mail_session_get_inbox` (libemail-engine/e-mail-session.c:2538-2618).

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

**Datenmodell:** `GListStore` aus konkreten GObject-Items, angezeigt mit `GtkListView`/`GtkColumnView` (Beispiel `src/mail/mail-view.c:208`, `mail-view.blp:124`). Änderungen am Store (`g_list_store_append`, `g_list_store_splice`) nur im UI-Thread, also im GTask-Rückruf. Eine eigene `GListModel`-Klasse erst, wenn ein `GListStore` nicht reicht.

**eds-compat.c:** Die einzige Datei für Aufrufe, die von der EDS-/Camel-Version abhängen (3.56 heute, 3.62 später), jeweils mit `#if EDS_CHECK_VERSION (…)`. Heute gibt es keinen solchen Aufruf; die Datei bricht nur den Build ab, wenn EDS älter als 3.56 ist.

**Verzeichnisse:** Beim Start werden `~/.local/share/revolution` und `~/.cache/revolution` angelegt (`g_get_user_data_dir ()`/`g_get_user_cache_dir ()` + `revolution`, Rechte 0700), getrennt von Evolution (`…/evolution`). Beide Profile nutzen dieselben Ordner, wie Stamp (stamp-helper.c:46-58) und wie das gemeinsame GSettings-Schema.

**Fehleranzeige:** Fehler zeigt das `AdwBanner` oben im Hauptfenster (`banner` in `src/window.blp`), keine Dialoge. Hinweise erscheinen als Pille (Dokument #5 „M2 Specification“). Test: `XDG_DATA_HOME=/proc/revolution-test scripts/screenshot.sh fehler-banner prefer-light`.

## Screenshots (headless)

`scripts/screenshot.sh NAME SCHEMA [BREITExHÖHE]`. Das Skript läuft in eigenem Xvfb und eigener D-Bus-Sitzung. Einstellungen bleiben dabei im Speicher (`GSETTINGS_BACKEND=memory`). Wichtige Umgebungsvariablen: `BUILD` (Build-Ordner, Standard `build/`), `OUT_DIR` (Zielordner), `KEYS` (Tastendrücke vorab).

```sh
scripts/screenshot.sh mail-hell prefer-light
KEYS="Control_L+2" scripts/screenshot.sh kalender-hell prefer-light
KEYS="Control_L+3" scripts/screenshot.sh kontakte-hell prefer-light
```

## Git-Hook

`hooks/pre-merge-commit` baut und testet das Merge-Ergebnis in `build-hook/` mit `werror=true` (`meson compile` und `meson test`); jede Compiler-Warnung zählt also als Fehler. Schlägt eins davon fehl, bricht `git merge` ohne Commit ab. Der Merge bleibt dann offen und wird mit `git merge --abort` zurückgenommen. Einmal je Klon aktivieren:

```sh
git config core.hooksPath hooks
git config merge.ff false
```

Lücken: Der Hook läuft nicht bei `git merge --no-verify` und nicht, wenn der Merge mit Konflikt stoppt. Den Konflikt-Commit dann vor dem Abschluss selbst mit `meson configure -Dwerror=true build-hook && meson compile -C build-hook && meson test -C build-hook` prüfen.

Arbeitsweise: ein Branch pro Task (`task/<Nr>-<kurz>`), Merge nach `main` erst nach dem Review.
