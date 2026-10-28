#!/usr/bin/env bash
# Benachrichtigungen, Hintergrund, mailto (#366) gegen das Testkonto KONTO_IMAP_2@example.invalid.
#
# Aufruf: tests/benachrichtigung.sh [AUSGABEORDNER]   (Standard BUILD/m5-366)
#   BUILD    Build-Ordner, Standard build/ im Repo (vorher meson compile)
#   LAEUFE   Anzahl der Durchgänge, Standard 3; ARTEN Auswahl der Läufe, Standard „haupt menue“
#   LAUF_TIMEOUT  Sekunden je Treiber-Lauf, Standard 600; TD_TIMEOUT je tests/testdaten.py, Standard 300
#
# Je Durchgang zwei App-Starts im eigenen Xvfb (echter Session-Bus, Einstellungen im Speicher, X11) mit
# run-in-background=true und mark-seen=false (eigenes Schema mit .gschema.override). Die App lädt nur
# geops@ und KONTO_M365@example.invalid (REVOLUTION_SKIP_ACCOUNTS: alle anderen, auch die nur lesend freigegebenen).
# Benachrichtigungen gehen an den Benachrichtigungsdienst der Sitzung und dürfen auf dem Desktop erscheinen
# (Arnd 27.10.2026). Weg wie im Betrieb (GLib wählt unter GNOME org.gtk.Notifications); mit
# GNOTIFICATION_BACKEND=freedesktop org.freedesktop.Notifications. Beides im Mitschnitt von dbus-monitor geprüft.
# Treiber tests/benachrichtigung-treiber.py, Lauf „haupt“:
#   - Testmails per IMAP APPEND in den Posteingang von geops@ (tests/testdaten.py --benachrichtigung): eine
#     ungelesene + eine gelesene → genau eine Benachrichtigung (Titel Absender, Text Betreff), keine für die
#     gelesene; zwei weitere ungelesene → dieselbe Benachrichtigung „(und 2 weitere)“ (dbus-monitor AddNotification bzw. Notify).
#   - Aktion der Benachrichtigung (org.gtk.Actions.Activate „show-message“ mit dem Ziel aus dem Log) wählt
#     den Posteingang von geops@ und die Nachricht; Benachrichtigung zurückgezogen (RemoveNotification bzw. CloseNotification).
#   - Hinweis microsoft365 ohne Push am Abschnittskopf von KONTO_M365@example.invalid (AT-SPI); Abruf-Zeitgeber
#     je Konto (Log), ausgelöst über app.test-refresh (REVOLUTION_TEST_HOOKS=1).
#   - mailto als Argument (zweiter Prozess → laufende Instanz) und per „gio open“ mit eigenem
#     XDG_CONFIG_HOME/XDG_DATA_HOME/XDG_DATA_DIRS (mimeapps.list, Desktop-Datei mit Exec = BIN): Composer mit
#     An/Kopie/Blindkopie/Betreff/Text (AT-SPI, Screenshot). Arnds mimeapps.list bleibt unberührt.
#   - Fenster schließen (WM_DELETE_WINDOW): Prozess läuft weiter, Fenster verborgen; neue Mail meldet sich
#     weiter; Aktion zeigt das Fenster wieder; app.quit beendet (Exit 0, App-ID nicht mehr auf dem Bus).
# Lauf „menue“: Start mit dem mailto-Link als Argument (Revolution lief nicht: Hauptfenster und Composer,
# Felder wie oben), Hauptmenü per F10, Einträge „Im Hintergrund weiterlaufen“ (Haken) und „Beenden“ (Strg+Q)
# über AT-SPI (Name aus „labelled-by“), dann Strg+Q beendet.
# Danach je Durchgang: die angelegten Testmails per UID STORE \Deleted + UID EXPUNGE genau ihrer UIDs
# (--benachrichtigung … weg, Befehle in OUT/<Lauf>-weg.txt).
# Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“; Exit 0 ohne Fehler (tests/abbruch.sh).
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m5-366}"
LAUF_TIMEOUT="${LAUF_TIMEOUT:-600}"
KONTO=KONTO_IMAP_2@example.invalid
FEHLER=0

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
for tool in xvfb-run gdbus dbus-monitor gio python3; do command -v "$tool" >/dev/null || { echo "FEHLER  $tool fehlt"; exit 1; }; done
python3 -c 'import Xlib, PIL' 2>/dev/null || { echo "FEHLER  python3-xlib/python3-pil fehlt"; exit 1; }
python3 -c 'import gi; gi.require_version("Atspi", "2.0")' 2>/dev/null || { echo "FEHLER  gir1.2-atspi-2.0 fehlt"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
mkdir -p "$OUT"
. "$APP_DIR/tests/abbruch.sh"

SCHEMA="$OUT/schema"
mkdir -p "$SCHEMA"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA/"
printf '[de.freiraum_medien.Revolution]\nrun-in-background=true\nmark-seen=false\n' >"$SCHEMA/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA" || { echo "FEHLER  Schema: glib-compile-schemas $SCHEMA"; exit 1; }

# Test-XDG für „gio open mailto:“: nur die eigene Desktop-Datei (Exec = BIN) als Standard für mailto.
XDG="$OUT/xdg"
rm -rf "$XDG"
mkdir -p "$XDG/data/applications" "$XDG/config" "$XDG/dirs"
sed "s|^Exec=.*|Exec=$BIN %U|" "$BUILD/data/$APP_ID.desktop" >"$XDG/data/applications/$APP_ID.desktop"
printf '[Default Applications]\nx-scheme-handler/mailto=%s.desktop\n' "$APP_ID" >"$XDG/config/mimeapps.list"

# UIDs der beiden geladenen Konten und alle übrigen Mail-Konten (ausgeschlossen).
read -r GEOPS OUTLOOK SKIP < <(python3 -c '
import sys; sys.path.insert(0, sys.argv[1] + "/tests")
import testdaten as t
from gi.repository import EDataServer
r = EDataServer.SourceRegistry.new_sync(None)
k = t.konten_finden(r)
geops, outlook = k["KONTO_IMAP_2@example.invalid"].account.get_uid(), k["KONTO_M365@example.invalid"].account.get_uid()
print(geops, outlook, ",".join(sorted(s.get_uid() for s in r.list_sources(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT)
                                     if s.get_uid() not in (geops, outlook))))' "$APP_DIR")
[ -n "${SKIP:-}" ] || { echo "FEHLER  Konten: UIDs nicht gefunden"; exit 1; }

lauf () {
  local name="$1" was="$2" rundir rc=0
  # Innerhalb der run.lock: die vorige Instanz gibt den Namen verzögert frei (bis 30 s).
  for _ in $(seq 30); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || { echo "FEHLER  $name: $APP_ID läuft schon"; return 1; }
  rundir="$(mktemp -d)"
  timeout -k 10 "$LAUF_TIMEOUT" env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$SCHEMA" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    ADW_DEBUG_COLOR_SCHEME=prefer-light ${GNOTIFICATION_BACKEND:+GNOTIFICATION_BACKEND="$GNOTIFICATION_BACKEND"} \
    G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals REVOLUTION_TEST_HOOKS=1 REVOLUTION_SKIP_ACCOUNTS="$SKIP" \
    BIN="$BIN" OUT="$OUT" NAME="$name" WAS="$was" APP_DIR="$APP_DIR" APP_ID="$APP_ID" KONTO="$KONTO" \
    GEOPS="$GEOPS" OUTLOOK="$OUTLOOK" XDG="$XDG" NEU="$OUT/$name-neu.txt" \
    xvfb-run -a -s "-screen 0 1200x760x24" python3 -I "$APP_DIR/tests/benachrichtigung-treiber.py" || rc=$?
  rm -rf "$rundir"
  for _ in $(seq 30); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  return "$rc"
}

# Arnds Zuordnungen bleiben unberührt (nur gelesen): Prüfsumme vorher/nachher.
mimeapps_summe () { sha256sum "${XDG_CONFIG_HOME:-$HOME/.config}/mimeapps.list" 2>/dev/null || echo fehlt; }
MIMEAPPS_VORHER="$(mimeapps_summe)"

for n in $(seq "${LAEUFE:-3}"); do
  for was in ${ARTEN:-haupt menue}; do
    name="m5-366-$n-$was"
    : >"$OUT/$name-neu.txt"
    lauf "$name" "$was" | tee "$OUT/$name.txt"
    teilschritt "$name" "${PIPESTATUS[0]}" "$OUT/$name.txt"
    # Aufräumen: genau die UIDs dieses Laufs (auch nach Abbruch).
    if grep -q '^NEU ' "$OUT/$name-neu.txt"; then
      td --benachrichtigung "$KONTO" weg "$OUT/$name-neu.txt" >"$OUT/$name-weg.txt" 2>&1
      cat "$OUT/$name-weg.txt"
      n_neu="$(grep -c '^NEU ' "$OUT/$name-neu.txt")"
      n_weg="$(grep -c '^BEFEHL UID EXPUNGE .* → OK' "$OUT/$name-weg.txt")"
      if [ "$n_neu" = "$n_weg" ]; then echo "OK      $name: $n_weg eigene Testmails per UID EXPUNGE gelöscht" | tee -a "$OUT/$name.txt"
      else echo "FEHLER  $name: $n_neu angelegt, $n_weg gelöscht" | tee -a "$OUT/$name.txt"; fi
    fi
    FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))
  done
done

if [ "$(mimeapps_summe)" = "$MIMEAPPS_VORHER" ]; then echo "OK      mimeapps.list der Sitzung unverändert: $MIMEAPPS_VORHER"
else echo "FEHLER  mimeapps.list der Sitzung verändert"; FEHLER=$((FEHLER + 1)); fi
abbruch_bilanz
echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
