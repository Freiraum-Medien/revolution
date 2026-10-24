#!/usr/bin/env bash
# Anhänge öffnen und speichern (#356) gegen die echten Testkonten.
#
# Aufruf: tests/anhaenge.sh [AUSGABEORDNER]   (Standard BUILD/m4-356)
#   BUILD     Build-Ordner, Standard build/ im Repo (vorher meson compile)
#   KONTEN    Testkonten, Standard „KONTO_IMAP_2@example.invalid KONTO_GOOGLE@example.invalid KONTO_M365@example.invalid“
#             (die Anhang-Mail aus tests/testdaten.py --anhang liegt nicht in privat@, Quota)
#   SPEICHER  Zielordner für „Speichern unter“ und „Alle speichern“, Standard AUSGABEORDNER/speichern
#   LAUF_TIMEOUT  Sekunden je Treiber-Lauf, Standard 900; TD_TIMEOUT je tests/testdaten.py, Standard 300
#
# Öffnen ohne Programme auf dem Desktop: kein Portal (GDK_DEBUG=no-portals, ADW_DISABLE_PORTAL=1),
# eigene XDG_CONFIG_HOME, XDG_DATA_HOME und XDG_DATA_DIRS. Darin genau ein Programm, der Test-Handler
# (revolution-test-handler.desktop, Standard für application/pdf, image/png, application/zip,
# text/plain, message/rfc822 über mimeapps.list), der nur Pfad und sha256 der Datei in eine
# Datei schreibt. XDG_DATA_DIRS zeigt auf Verweise auf alle Einträge von /usr/share außer
# „applications“ (MIME-Datenbank, Symbole, Schemas bleiben), XDG_DATA_HOME auf Verweise auf
# revolution, camel_certs, fonts, icons, mime aus ~/.local/share (ohne „applications“ und
# recently-used.xbel). Vor dem Start prüft GIO mit derselben Umgebung, dass für jeden Typ der
# Test-Handler gilt, sonst Abbruch ohne Start. Belege: strace (execve) der App mit allen Kindern
# und dbus-monitor auf die Portale OpenURI und FileChooser während des Laufs.
#
# Je Konto ein App-Start im eigenen Xvfb (wie scripts/screenshot.sh: echter Session-Bus,
# Einstellungen im Speicher, X11) mit REVOLUTION_FOLDER = Posteingang und REVOLUTION_MESSAGE =
# Anhang-Mail. Der Python-Treiber tests/anhaenge-treiber.py bedient die App per XTest: PDF (Eingabe),
# PNG (Doppelklick), ZIP (Pfeil rechts, Eingabe) öffnen; PDF über Rechtsklick „Speichern unter …“,
# dasselbe Ziel ein zweites Mal (GTK fragt, Abbrechen lässt die Datei unverändert); zweimal „Alle
# speichern …“ (Menü-Taste) in denselben Ordner (zweites Mal „Name (1).ext“ wie Evolution), dann in
# einen schreibgeschützten Ordner (alle Anhänge versucht, Fehler gesammelt im Banner). Jede
# Datei byte-gleich (sha256) mit den Teilen, die tests/testdaten.py --anhang-teile unabhängig von
# Camel vom Server holt (IMAP bzw. Graph). Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“, „WERT …“
# für Messwerte; Exit 0 ohne Fehler.
# #411: Jeder Abbruch eines Teilschritts (Traceback, Exit ≠ 0 von Treiber oder testdaten.py,
# Zeitlimit) ist ein FEHLER (tests/abbruch.sh).
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/src/revolution"
# #411: absolut, der Treiber vergleicht Pfade aus dem Log der App mit ZIEL.
OUT="$(realpath -m "${1:-$BUILD/m4-356}")"
KONTEN="${KONTEN:-KONTO_IMAP_2@example.invalid KONTO_GOOGLE@example.invalid KONTO_M365@example.invalid}"
SPEICHER="$(realpath -m "${SPEICHER:-$OUT/speichern}")"
LAUF_TIMEOUT="${LAUF_TIMEOUT:-900}"
FEHLER=0

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
for tool in xvfb-run gdbus python3 strace dbus-monitor sha256sum; do
  command -v "$tool" >/dev/null || { echo "FEHLER  $tool fehlt"; exit 1; }
done
python3 -c 'import Xlib, PIL' 2>/dev/null || { echo "FEHLER  python3-xlib/python3-pil fehlt"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
mkdir -p "$OUT" "$SPEICHER"
. "$APP_DIR/tests/abbruch.sh"

# ---- Umgebung mit Test-Handler
XDG="$OUT/xdg"
rm -rf "$XDG"
mkdir -p "$XDG/config" "$XDG/data/applications" "$XDG/share"
GEOEFFNET="$OUT/geoeffnet.txt"
: >"$GEOEFFNET"
cat >"$XDG/handler.sh" <<EOF
#!/bin/sh
# Test-Handler (#356): nur Pfad und sha256 festhalten, nichts anzeigen.
printf 'GEOEFFNET %s\n' "\$(sha256sum "\$1")" >>"$GEOEFFNET"
EOF
chmod 755 "$XDG/handler.sh"
TYPEN="application/pdf image/png application/zip text/plain message/rfc822"
cat >"$XDG/data/applications/revolution-test-handler.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Revolution Test-Handler
Exec=$XDG/handler.sh %f
MimeType=$(printf '%s;' $TYPEN)
NoDisplay=true
EOF
{
  echo "[Default Applications]"
  for t in $TYPEN; do echo "$t=revolution-test-handler.desktop"; done
} >"$XDG/config/mimeapps.list"
for e in /usr/share/*; do
  [ "$(basename "$e")" = applications ] || ln -s "$e" "$XDG/share/$(basename "$e")"
done
for e in revolution camel_certs fonts icons mime; do
  [ -e "$HOME/.local/share/$e" ] && ln -s "$HOME/.local/share/$e" "$XDG/data/$e"
done
export XDG_CONFIG_HOME="$XDG/config" XDG_DATA_HOME="$XDG/data" XDG_DATA_DIRS="$XDG/share"

if ! python3 -I - "$TYPEN" <<'EOF'
import sys
from gi.repository import Gio
fehler = 0
for t in sys.argv[1].split():
    app = Gio.AppInfo.get_default_for_type(t, False)
    name = app.get_id() if app else None
    ok = name == 'revolution-test-handler.desktop'
    fehler += not ok
    print('%s  Standardprogramm %s: %s' % ('OK     ' if ok else 'FEHLER ', t, name))
alle = sorted(a.get_id() for a in Gio.AppInfo.get_all())
print('%s  sichtbare Programme: %s' % ('OK     ' if alle == ['revolution-test-handler.desktop'] else 'FEHLER ', alle))
sys.exit(1 if fehler or alle != ['revolution-test-handler.desktop'] else 0)
EOF
then
  echo "FEHLER  Test-Handler nicht wirksam, kein Start"; exit 1
fi

# ---- Aufräumen der temporären Dateien beim Start (älter als 2 h): ein alter und ein neuer Ordner
TMPBASE="$HOME/.cache/revolution/attachments"
mkdir -p "$TMPBASE"
alt="$(mktemp -d "$TMPBASE/test-alt-XXXXXX")"
neu="$(mktemp -d "$TMPBASE/test-neu-XXXXXX")"
echo alt >"$alt/alt.txt"
echo neu >"$neu/neu.txt"
touch -a -m -d '3 hours ago' "$alt/alt.txt" "$alt"
aufraeumen_geprueft=0

# ---- Läufe je Konto
# #357: Standard ist die Konversationsansicht. Dieser Test wählt die Anhang-Mail als einzelne Zeile
# (REVOLUTION_MESSAGE) und bedient die Anhangleiste des Lesebereichs der Nachricht (#356); flach
# (conversations=false per .gschema.override), damit Auswahl und Koordinaten wie bisher gelten.
# Anhänge in der Konversation prüft tests/konversationen.sh (Anhangleiste je Nachricht).
SCHEMA="$OUT/schema"
mkdir -p "$SCHEMA"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA/"
printf '[de.freiraum_medien.Revolution]\nconversations=false\n' >"$SCHEMA/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA" || { echo "FEHLER  Schema: glib-compile-schemas $SCHEMA"; exit 1; }

for konto in $KONTEN; do
  name="m4-356-${konto%%@*}"
  teile="$OUT/$name-server"
  rm -rf "$teile"
  if ! td --anhang-teile "$konto" "$teile" >"$OUT/$name-server.txt" 2>&1; then
    echo "        $konto übersprungen (siehe $OUT/$name-server.txt)"; continue
  fi
  # Posteingang: imapx „INBOX“, microsoft365 der Anzeigename (Log „Ordner: <UID> Posteingang“).
  inbox=INBOX
  case "$konto" in *outlook*) inbox=Posteingang ;; esac
  ordner="$(td --flags "$konto" | sed -n "s#^ORDNER \([^/]*\)/.*#\1/$inbox#p")"
  ziel="$SPEICHER/$name"
  [ -d "$ziel/gesperrt" ] && chmod 700 "$ziel/gesperrt"
  rm -rf "$ziel"
  mkdir -p "$ziel/einzeln" "$ziel/alle"
  if [ "$(bus_hat_app)" != "(false,)" ]; then
    echo "FEHLER  Session-Bus: $APP_ID läuft schon, $konto übersprungen"; FEHLER=$((FEHLER + 1)); continue
  fi
  dbus-monitor --session "interface='org.freedesktop.portal.OpenURI'" "interface='org.freedesktop.portal.FileChooser'" \
    >"$OUT/$name-portale.txt" 2>&1 & monitor=$!
  sleep 1
  rundir="$(mktemp -d)"
  timeout -k 10 "$LAUF_TIMEOUT" env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$SCHEMA" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    GTK_A11Y=none NO_AT_BRIDGE=1 ADW_DEBUG_COLOR_SCHEME=prefer-light \
    G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals REVOLUTION_FOLDER="$ordner" \
    REVOLUTION_MESSAGE="Anhänge PDF, Bild, ZIP" \
    BIN="$BIN" OUT="$OUT" NAME="$name" KONTO="$konto" ORDNER="$ordner" APP_DIR="$APP_DIR" \
    TEILE="$OUT/$name-server.txt" GEOEFFNET="$GEOEFFNET" ZIEL="$ziel" \
    xvfb-run -a -s "-screen 0 1200x760x24" python3 -I "$APP_DIR/tests/anhaenge-treiber.py" | tee "$OUT/$name.txt"
  teilschritt "$name" "${PIPESTATUS[0]}" "$OUT/$name.txt"
  rm -rf "$rundir"
  kill "$monitor" 2>/dev/null; wait "$monitor" 2>/dev/null
  # #411: bis 10 s auf die Freigabe des Namens warten (nach Zeitlimit endet die App per Signal).
  for _ in $(seq 10); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus" | tee -a "$OUT/$name.txt"
  portale="$(grep -c 'member=\(OpenFile\|OpenURI\|OpenDirectory\|SaveFile\|OpenFile\|SaveFiles\)' "$OUT/$name-portale.txt")"
  if [ "$portale" = 0 ]; then echo "OK      $name: dbus-monitor: 0 Aufrufe der Portale OpenURI/FileChooser"
  else echo "FEHLER  $name: dbus-monitor: $portale Portal-Aufrufe ($OUT/$name-portale.txt)"; fi | tee -a "$OUT/$name.txt"
  if [ "$aufraeumen_geprueft" = 0 ]; then
    aufraeumen_geprueft=1
    zeile="$(grep -o 'Anhang: temporäre Dateien aufgeräumt.*' "$OUT/$name.log")"
    if [ ! -e "$alt" ] && [ -f "$neu/neu.txt" ] && [ -n "$zeile" ]; then
      echo "OK      $name: Start räumt auf: alter Ordner (3 h) entfernt, neuer bleibt; Log „$zeile“"
    else
      echo "FEHLER  $name: Aufräumen beim Start: alt vorhanden=$([ -e "$alt" ] && echo ja || echo nein), neu vorhanden=$([ -f "$neu/neu.txt" ] && echo ja || echo nein), Log „$zeile“"
    fi | tee -a "$OUT/$name.txt"
    rm -rf "$alt" "$neu"
  fi
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))
done

abbruch_bilanz
echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
