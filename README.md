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

## Screenshots (headless)

`scripts/screenshot.sh NAME SCHEMA [BREITExHÖHE]`. Das Skript läuft in eigenem Xvfb und eigener D-Bus-Sitzung. Einstellungen bleiben dabei im Speicher (`GSETTINGS_BACKEND=memory`). Wichtige Umgebungsvariablen: `BUILD` (Build-Ordner, Standard `build/`), `OUT_DIR` (Zielordner), `KEYS` (Tastendrücke vorab).

```sh
scripts/screenshot.sh mail-hell prefer-light
KEYS="Control_L+2" scripts/screenshot.sh kalender-hell prefer-light
KEYS="Control_L+3" scripts/screenshot.sh kontakte-hell prefer-light
```

## Git-Hook

`hooks/pre-merge-commit` baut und testet das Merge-Ergebnis in `build-hook/` (`meson compile` und `meson test`). Schlägt eins davon fehl, bricht `git merge` ohne Commit ab. Der Merge bleibt dann offen und wird mit `git merge --abort` zurückgenommen. Einmal je Klon aktivieren:

```sh
git config core.hooksPath hooks
git config merge.ff false
```

Lücken: Der Hook läuft nicht bei `git merge --no-verify` und nicht, wenn der Merge mit Konflikt stoppt. Den Konflikt-Commit dann vor dem Abschluss selbst mit `meson compile -C build-hook && meson test -C build-hook` prüfen.

Arbeitsweise: ein Branch pro Task (`task/<Nr>-<kurz>`), Merge nach `main` erst nach dem Review.
