#!/usr/bin/env bash
# Suche (#365) gegen die echten Testkonten.
#
# Aufruf: tests/suche.sh [AUSGABEORDNER]   (Standard BUILD/m5-365)
#   BUILD    Build-Ordner, Standard build/ im Repo (vorher meson compile)
#   LAUF_TIMEOUT  Sekunden je Treiber-Lauf, Standard 900; TD_TIMEOUT je tests/testdaten.py, Standard 300
#
# Testdaten: tests/testdaten.py --suche (Mail mit einem Wort nur im Text in geops@ Posteingang und
# Revolution-Test sowie outlook.com Posteingang). Drei App-Starts im eigenen Xvfb (echter Session-Bus,
# Einstellungen im Speicher, flach, mark-seen=false, AT-SPI wie tests/netz.sh), Treiber tests/suche-treiber.py:
#   geops     geops@ Posteingang, hell: Suche im Ordner, Alt+A „Alle Ordner“ (Summe der Testkonten), Alt+O,
#             Escape stellt Liste und Auswahl wieder her, Filter Ungelesen/Markiert/Mit Anhang, Filter „Alle“.
#   markiert  geops@ Revolution-Test, dunkel; vorher Einladung am Server markiert (testdaten.py --server,
#             danach wieder unmarkiert): Filter Markiert mit Treffer.
#   outlook   outlook.com Posteingang (microsoft365), hell: Suche im Ordner und Filter.
# Treffer je Schritt gegen tests/testdaten.py --suche-zahlen (IMAP UID SEARCH/BODYSTRUCTURE bzw. Graph, nur
# lesend). Die Suche schreibt nichts: --suche-zahlen vor und nach jedem Lauf gleich (Ungelesen, Markiert).
# Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“; Exit 0 ohne Fehler. #411: Abbruch eines Teilschritts zählt
# als FEHLER (tests/abbruch.sh).
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m5-365}"
LAUF_TIMEOUT="${LAUF_TIMEOUT:-900}"
GEOPS=KONTO_IMAP_2@example.invalid
OUTLOOK=KONTO_M365@example.invalid
FEHLER=0

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
for tool in xvfb-run gdbus python3; do command -v "$tool" >/dev/null || { echo "FEHLER  $tool fehlt"; exit 1; }; done
python3 -c 'import Xlib, PIL' 2>/dev/null || { echo "FEHLER  python3-xlib/python3-pil fehlt"; exit 1; }
python3 -c 'import gi; gi.require_version("Atspi", "2.0")' 2>/dev/null || { echo "FEHLER  gir1.2-atspi-2.0 fehlt"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
# Briefing (PM 27.10.): in run.lock vor jedem App-Start bis 30 s warten, bis der Name frei ist (Vorlauf
# eines anderen Agenten gibt ihn verzögert frei).
bus_frei () { for _ in $(seq 30); do [ "$(bus_hat_app)" = "(false,)" ] && return 0; sleep 1; done; [ "$(bus_hat_app)" = "(false,)" ]; }
bus_frei || { echo "FEHLER  Session-Bus: $APP_ID läuft schon"; exit 1; }
mkdir -p "$OUT"
. "$APP_DIR/tests/abbruch.sh"

SCHEMA="$OUT/schema"
mkdir -p "$SCHEMA"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA/"
printf '[de.freiraum_medien.Revolution]\nconversations=false\nmark-seen=false\nwindow-width=1400\nwindow-height=900\n' \
  >"$SCHEMA/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA" || { echo "FEHLER  Schema: glib-compile-schemas $SCHEMA"; exit 1; }

if td --suche >"$OUT/testdaten.txt" 2>&1; then
  echo "OK      Testdaten: $(grep -c '^SUCHE ' "$OUT/testdaten.txt") Ordner mit der Such-Mail"
else
  echo "FEHLER  Testdaten: tests/testdaten.py --suche (siehe $OUT/testdaten.txt)"; exit 1
fi
uid_von () { sed -n "s#^SUCHE $1 \([^/]*\)/.*#\1#p" "$OUT/testdaten.txt" | head -1; }
GEOPS_UID="$(uid_von "$GEOPS")"
OUTLOOK_UID="$(uid_von "$OUTLOOK")"
OUTLOOK_INBOX="$(sed -n "s#^SUCHE $OUTLOOK [^/]*/\(.*\)#\1#p" "$OUT/testdaten.txt" | head -1)"
RT="$(sed -n "s#^SUCHE $GEOPS [^/]*/\(.*Revolution-Test\)\$#\1#p" "$OUT/testdaten.txt" | head -1)"

{ td --suche-zahlen "$GEOPS" '*' && td --suche-zahlen "$OUTLOOK" '*'; } >"$OUT/zahlen-alle.txt" 2>&1 \
  || echo "FEHLER  tests/testdaten.py --suche-zahlen … * (siehe $OUT/zahlen-alle.txt)"

# Ungelesen/Markiert am Server vor und nach einem Lauf gleich: die Suche schreibt keine Flags.
unveraendert () {
  local name="$1" vorher="$2" nachher="$3"
  if [ "$(grep -o 'ungelesen [0-9]* markiert [0-9]*' "$vorher")" = "$(grep -o 'ungelesen [0-9]* markiert [0-9]*' "$nachher")" ]; then
    echo "OK      $name: Flags am Server unverändert ($(grep -o 'ungelesen [0-9]* markiert [0-9]*' "$nachher"))"
  else
    echo "FEHLER  $name: Flags am Server geändert: $(cat "$vorher") → $(cat "$nachher")"; FEHLER=$((FEHLER + 1))
  fi
}

lauf () {
  local name="$1" konto="$2" uid="$3" ordner="$4" ref="$5" schema="$6" inbox="$7" alle="$8" filter="$9" rundir rc=0
  td --suche-zahlen "$konto" "$ref" >"$OUT/$name-zahlen.txt" 2>&1 || { echo "FEHLER  $name: --suche-zahlen"; return 1; }
  bus_frei || { echo "FEHLER  $name: Session-Bus: $APP_ID läuft schon"; return 1; }
  rundir="$(mktemp -d)"
  timeout -k 10 "$LAUF_TIMEOUT" env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK -u M365_DEBUG \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$SCHEMA" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    ADW_DEBUG_COLOR_SCHEME="$schema" G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals \
    REVOLUTION_FOLDER="$uid/$ordner" BIN="$BIN" OUT="$OUT" NAME="m5-365-$name" UID="$uid" ORDNER="$ordner" \
    ORDNER_REF="$ref" INBOX="$inbox" ALLE="$alle" FILTER="$filter" \
    ZAHLEN="$OUT/$name-zahlen.txt" ZAHLEN_ALLE="$OUT/zahlen-alle.txt" \
    xvfb-run -a -s "-screen 0 1400x900x24" python3 -I "$APP_DIR/tests/suche-treiber.py" || rc=$?
  rm -rf "$rundir"
  for _ in $(seq 10); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  td --suche-zahlen "$konto" "$ref" >"$OUT/$name-zahlen-nachher.txt" 2>&1
  return "$rc"
}

teil () {
  local name="$1"; shift
  lauf "$name" "$@" | tee "$OUT/$name.txt"
  teilschritt "$name" "${PIPESTATUS[0]}" "$OUT/$name.txt"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))
  unveraendert "$name" "$OUT/$name-zahlen.txt" "$OUT/$name-zahlen-nachher.txt"
}

teil geops "$GEOPS" "$GEOPS_UID" INBOX INBOX prefer-light 1 1 "unread starred attachment"
td --server "$GEOPS" markiert >"$OUT/markiert-server.txt" 2>&1
teil markiert "$GEOPS" "$GEOPS_UID" "$RT" Revolution-Test prefer-dark 0 0 "starred unread attachment"
td --server "$GEOPS" unmarkiert >>"$OUT/markiert-server.txt" 2>&1
teil outlook "$OUTLOOK" "$OUTLOOK_UID" "$OUTLOOK_INBOX" "$OUTLOOK_INBOX" prefer-light 1 0 "unread starred attachment"

abbruch_bilanz

echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
