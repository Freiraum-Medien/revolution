#!/usr/bin/env bash
# Ordner anlegen, umbenennen, löschen (#364) gegen geops@ (IMAP), outlook.com (microsoft365) und Gmail (Labels).
#
# Aufruf: flock build/.run.lock tests/ordner.sh [AUSGABEORDNER]   (Standard BUILD/m5-364)
#   BUILD    Build-Ordner, Standard build/ im Repo (vorher meson compile)
#   LAEUFE   Anzahl Läufe, Standard 3;  KONTEN  Standard „KONTO_IMAP_2@example.invalid KONTO_M365@example.invalid
#            KONTO_GOOGLE@example.invalid“;  LAUF_TIMEOUT  Sekunden je App-Start, Standard 900
#
# Je Lauf und Konto ein App-Start im eigenen Xvfb (echter Session-Bus, Einstellungen im Speicher, AT-SPI wie
# tests/suche.sh), geladen wird nur dieses Konto: alle anderen per REVOLUTION_SKIP_ACCOUNTS (tests/testdaten.py
# --konten), also nie eines der nur lesend freigegebenen Konten. Ablauf und Prüfungen: tests/ordner-treiber.py.
# Angelegt und gelöscht werden nur Ordner „Revolution-Ordnertest…“ dieses Laufs; liegt vorher schon einer am
# Server (tests/testdaten.py --ordnertest), bricht der Lauf ab. Kontextmenü als Bild hell bzw. dunkel
# (abwechselnd je Konto und Lauf). #435: nach jedem Schritt kein doppelter Papierkorb/Spam. Eine Zeile je Prüfung
# „OK …“ bzw. „FEHLER …“; Exit 0 ohne Fehler.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m5-364}"
LAEUFE="${LAEUFE:-3}"
KONTEN="${KONTEN:-KONTO_IMAP_2@example.invalid KONTO_M365@example.invalid KONTO_GOOGLE@example.invalid}"
LAUF_TIMEOUT="${LAUF_TIMEOUT:-900}"
FEHLER=0

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
python3 -c 'import Xlib, PIL, gi; gi.require_version("Atspi", "2.0")' 2>/dev/null ||
  { echo "FEHLER  python3-xlib/python3-pil/gir1.2-atspi-2.0 fehlt"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
# Briefing (PM 27.10.): in run.lock vor jedem App-Start bis 30 s warten, bis der Name frei ist.
bus_frei () { for _ in $(seq 30); do [ "$(bus_hat_app)" = "(false,)" ] && return 0; sleep 1; done; [ "$(bus_hat_app)" = "(false,)" ]; }
mkdir -p "$OUT"
. "$APP_DIR/tests/abbruch.sh"

SCHEMA="$OUT/schema"
mkdir -p "$SCHEMA"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA/"
printf '[de.freiraum_medien.Revolution]\nmark-seen=false\nwindow-width=1400\nwindow-height=900\n' \
  >"$SCHEMA/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA" || { echo "FEHLER  Schema: glib-compile-schemas $SCHEMA"; exit 1; }
td --konten >"$OUT/konten.txt" 2>&1 || { echo "FEHLER  tests/testdaten.py --konten"; exit 1; }

lauf () {
  local name="$1" konto="$2" schema="$3" uid skip rundir rc=0 m365=0
  uid="$(awk -v k="$konto" '$1 == "KONTO" && $3 == k { print $2 }' "$OUT/konten.txt")"
  skip="$(awk -v k="$konto" '$1 == "KONTO" && $3 != k { printf "%s%s", s, $2; s = "," }' "$OUT/konten.txt")"
  [ -n "$uid" ] || { echo "FEHLER  $name: $konto nicht in GOA"; return 1; }
  # outlook.com: Graph-Anfragen ins Log (nur Methode und Pfad, Treiber filtert), Prüfung auf Echo-PATCH (#416).
  [ "$konto" = KONTO_M365@example.invalid ] && m365=1
  bus_frei || { echo "FEHLER  $name: Session-Bus: $APP_ID läuft schon"; return 1; }
  rundir="$(mktemp -d)"
  timeout -k 10 "$LAUF_TIMEOUT" env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK \
    ECHT_XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$SCHEMA" \
    GSETTINGS_BACKEND=memory GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local \
    GSK_RENDERER=cairo ADW_DEBUG_COLOR_SCHEME="$schema" G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals \
    REVOLUTION_TEST_HOOKS=1 REVOLUTION_SKIP_ACCOUNTS="$skip" M365_DEBUG="$m365" \
    BIN="$BIN" OUT="$OUT" NAME="m5-364-$name" UID="$uid" KONTO="$konto" TESTDATEN="$APP_DIR/tests/testdaten.py" \
    xvfb-run -a -s "-screen 0 1400x900x24" python3 -I "$APP_DIR/tests/ordner-treiber.py" || rc=$?
  rm -rf "$rundir"
  for _ in $(seq 10); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  # #363: microsoft365-Ordner-Cache (camel-m365-store.c:81) und Summaries (folders.db, eds-compat.c) ohne
  # verwaiste Testordner.
  baum="$HOME/.cache/revolution/mail/$uid/folder-tree"
  if [ -f "$baum" ]; then
    rest="$(grep 'Revolution-Ordnertest' "$baum"; python3 -c 'import sqlite3, sys
db = sqlite3.connect("file:%s?mode=ro" % sys.argv[1], uri=True)
print(*[r[0] for r in db.execute("select folder_name from folders where folder_name like ?", ("%Revolution-Ordnertest%",))])' \
      "$HOME/.cache/revolution/mail/$uid/folders.db")"
    if [ -n "$rest" ]; then
      echo "FEHLER  $name: verwaist in folder-tree/folders.db: $(echo "$rest" | tr '\n' ' ')"
    else
      echo "OK      $name: kein Testordner in $baum und folders.db"
    fi
  fi
  return "$rc"
}

i=0
for n in $(seq "$LAEUFE"); do
  for konto in $KONTEN; do
    i=$((i + 1))
    schema=prefer-light; [ $((i % 2)) -eq 0 ] && schema=prefer-dark
    name="$n-${konto%%@*}"
    lauf "$name" "$konto" "$schema" | tee "$OUT/$name.txt"
    teilschritt "$name" "${PIPESTATUS[0]}" "$OUT/$name.txt"
    FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))
  done
done

abbruch_bilanz

echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
