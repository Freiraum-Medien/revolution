#!/usr/bin/env bash
# Adressbücher und Kontaktliste (#368) gegen die echten Adressbücher aus GOA.
#
# Aufruf: tests/kontakte.sh [AUSGABEORDNER]   (Standard BUILD/m6-368)
#   BUILD    Build-Ordner, Standard build-devel/ im Repo (#439, Profil development) (vorher meson compile)
#   LAUF_TIMEOUT  Sekunden je Treiber-Lauf, Standard 900; TD_TIMEOUT je tests/testdaten.py, Standard 300
#   GRAPH_WARTEN  Sekunden, die der Graph-Lauf auf eine Server-Änderung ohne Refresh wartet, Standard 60
#
# Vergleich unabhängig von der App: tests/testdaten.py --adressbuecher (eigener Prozess, Python-GI EBook.BookClient,
# dieselbe Schicht wie GNOME Contacts; GNOME Contacts selbst öffnete ein Fenster). Zwei App-Starts im eigenen Xvfb
# (echter Session-Bus, Einstellungen im Speicher, AT-SPI wie tests/suche.sh), Treiber tests/kontakte-treiber.py:
#   google   hell, Google-CardDAV des Gmail test account (518e9322…): Seitenleiste = Registry (UID, Konto, Name, Kopfzeilen,
#            nur lesend), je Buch Kontaktzahl = Python-GI, Alt+M; Testkontakt von außen anlegen, suchen (Treffer,
#            Nicht-Treffer, Escape), ändern, löschen – jede Änderung live ohne Neuladen.
#   outlook  dunkel, microsoft365 „Kontakte“ outlook.com (3be9d2a4…): dasselbe live über EDS; dazu Gegenprobe am Server
#            über Graph (ohne EDS): erscheint/verschwindet der Kontakt ohne Refresh? sonst nach e_book_client_refresh.
# Schreiben nur in diese beiden Bücher (Briefing Zusatz M6), nur Testkontakte „Revolution-Test 368 …“ über die
# gespeicherte UID bzw. Graph-ID; höchstens 20 Schreibvorgänge je Lauf und Buch (geprüft). Am Ende: kein Testkontakt
# übrig (--kontakt-live pruefen, Graph GET 404) und Kontaktzahlen wie vorher. Zeilen „OK …“/„FEHLER …“, Exit 0 ohne
# Fehler. #411: Abbruch eines Teilschritts zählt als FEHLER (tests/abbruch.sh).
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "$APP_DIR/tests/testkonten.sh"
BUILD="${BUILD:-$APP_DIR/build-devel}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m6-368}"
LAUF_TIMEOUT="${LAUF_TIMEOUT:-900}"
GOOGLE_BUCH=518e93226d14866ae0d8074ec0128a43c0c0c257
OUTLOOK_BUCH=3be9d2a4067fd00b72a3d3d5dbd529075a08ea88
FEHLER=0

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
for tool in xvfb-run gdbus python3; do command -v "$tool" >/dev/null || { echo "FEHLER  $tool fehlt"; exit 1; }; done
python3 -c 'import Xlib, PIL' 2>/dev/null || { echo "FEHLER  python3-xlib/python3-pil fehlt"; exit 1; }
python3 -c 'import gi; gi.require_version("Atspi", "2.0"); gi.require_version("EBook", "1.2")' 2>/dev/null \
  || { echo "FEHLER  gir1.2-atspi-2.0/gir1.2-ebook-1.2 fehlt"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
bus_frei () { for _ in $(seq 30); do [ "$(bus_hat_app)" = "(false,)" ] && return 0; sleep 1; done; [ "$(bus_hat_app)" = "(false,)" ]; }
bus_frei || { echo "FEHLER  Session-Bus: $APP_ID läuft schon"; exit 1; }
mkdir -p "$OUT"
. "$APP_DIR/tests/abbruch.sh"

SCHEMA="$OUT/schema"
mkdir -p "$SCHEMA"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA/"
printf '[de.freiraum_medien.Revolution]\nwindow-width=1400\nwindow-height=900\n' >"$SCHEMA/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA" || { echo "FEHLER  Schema: glib-compile-schemas $SCHEMA"; exit 1; }

if td --adressbuecher >"$OUT/adressbuecher.txt" 2>&1; then
  echo "OK      Vergleich: $(grep -c '^ADRESSBUCH ' "$OUT/adressbuecher.txt") Adressbücher (tests/testdaten.py --adressbuecher)"
else
  echo "FEHLER  Vergleich: tests/testdaten.py --adressbuecher (siehe $OUT/adressbuecher.txt)"; exit 1
fi

lauf () {
  local name="$1" buch="$2" modus="$3" schema="$4" graph="$5" rundir rc=0
  bus_frei || { echo "FEHLER  $name: Session-Bus: $APP_ID läuft schon"; return 1; }
  rundir="$(mktemp -d)"
  timeout -k 10 "$LAUF_TIMEOUT" env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK -u M365_DEBUG \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$SCHEMA" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    ADW_DEBUG_COLOR_SCHEME="$schema" G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals \
    REVOLUTION_BOOK="$buch" BIN="$BIN" OUT="$OUT" NAME="m6-368-$name" BUCH="$buch" MODUS="$modus" GRAPH="$graph" \
    VERGLEICH="$OUT/adressbuecher.txt" KONTAKT="$OUT/$name-kontakt.txt" GRAPH_DATEI="$OUT/$name-graph.txt" \
    TESTDATEN="$APP_DIR/tests/testdaten.py" \
    xvfb-run -a -s "-screen 0 1400x900x24" python3 -I "$APP_DIR/tests/kontakte-treiber.py" || rc=$?
  rm -rf "$rundir"
  for _ in $(seq 10); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  return "$rc"
}

# Aufräumen und Beleg: gespeicherter Testkontakt nicht mehr vorhanden (sonst jetzt löschen, zählt als FEHLER).
aufgeraeumt () {
  local name="$1" buch="$2"
  if [ -s "$OUT/$name-kontakt.txt" ]; then
    td --kontakt-live pruefen "$buch" "$OUT/$name-kontakt.txt" >"$OUT/$name-aufraeumen.txt" 2>&1
    if grep -q '^VORHANDEN 0 ' "$OUT/$name-aufraeumen.txt"; then
      echo "OK      $name: Testkontakt $(cut -d' ' -f3 "$OUT/$name-kontakt.txt") nicht mehr im Buch"
    else
      echo "FEHLER  $name: Testkontakt noch vorhanden, wird gelöscht: $(cat "$OUT/$name-aufraeumen.txt")"; FEHLER=$((FEHLER + 1))
      td --kontakt-live loeschen "$buch" "$OUT/$name-kontakt.txt" >>"$OUT/$name-aufraeumen.txt" 2>&1
    fi
  fi
  if [ -s "$OUT/$name-graph.txt" ] && ! grep -q ': WEG ' "$OUT/$name.txt"; then
    echo "FEHLER  $name: Graph-Kontakt nicht als gelöscht belegt, wird gelöscht"; FEHLER=$((FEHLER + 1))
    td --kontakt-graph loeschen "$OUT/$name-graph.txt" >>"$OUT/$name-aufraeumen.txt" 2>&1
  fi
  local n
  for b in "$GOOGLE_BUCH" "$OUTLOOK_BUCH"; do
    n="$(grep -c "SCHREIBVORGANG $b" "$OUT/$name.txt")"
    if [ "$n" -le 20 ]; then echo "OK      $name: $n Schreibvorgänge in ${b:0:8} (≤ 20)"
    else echo "FEHLER  $name: $n Schreibvorgänge in ${b:0:8} (> 20)"; FEHLER=$((FEHLER + 1)); fi
  done
  n="$(grep -c 'SCHREIBVORGANG graph' "$OUT/$name.txt")"
  [ "$n" -le 20 ] || { echo "FEHLER  $name: $n Graph-Schreibvorgänge (> 20)"; FEHLER=$((FEHLER + 1)); }
}

teil () {
  local name="$1" buch="$2"; shift
  rm -f "$OUT/$name-kontakt.txt" "$OUT/$name-graph.txt"
  lauf "$name" "$@" | tee "$OUT/$name.txt"
  teilschritt "$name" "${PIPESTATUS[0]}" "$OUT/$name.txt"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))
  aufgeraeumt "$name" "$buch"
}

teil google "$GOOGLE_BUCH" alle prefer-light 0
teil outlook "$OUTLOOK_BUCH" live prefer-dark 1

td --adressbuecher >"$OUT/adressbuecher-nachher.txt" 2>&1
if diff -q <(grep '^ADRESSBUCH' "$OUT/adressbuecher.txt") <(grep '^ADRESSBUCH' "$OUT/adressbuecher-nachher.txt") >/dev/null; then
  echo "OK      Kontaktzahlen aller Bücher nach dem Test wie vorher (adressbuecher-nachher.txt)"
else
  echo "FEHLER  Kontaktzahlen geändert: $(diff <(grep '^ADRESSBUCH' "$OUT/adressbuecher.txt") <(grep '^ADRESSBUCH' "$OUT/adressbuecher-nachher.txt") | tr '\n' ' ')"
  FEHLER=$((FEHLER + 1))
fi

abbruch_bilanz

echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
