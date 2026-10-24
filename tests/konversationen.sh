#!/usr/bin/env bash
# Konversationen (#357) gegen die echten Testkonten.
#
# Aufruf: tests/konversationen.sh [AUSGABEORDNER]   (Standard BUILD/m4-357)
#   BUILD    Build-Ordner, Standard build/ im Repo (vorher meson compile)
#   KONTEN   Testkonten, Standard „KONTO_IMAP_2@example.invalid KONTO_M365@example.invalid“ (Konversation aus
#            tests/testdaten.py --konversation: 3 Mails im Posteingang, 2 eigene im Gesendet-Ordner)
#
# Je Konto ein App-Start im eigenen Xvfb (wie tests/flags.sh: echter Session-Bus, Einstellungen im
# Speicher, X11) im Posteingang, mit Schema conversations=true (.gschema.override) und
# REVOLUTION_MESSAGE = Betreff der Konversation. Der Treiber tests/konversationen-treiber.py bedient
# die App per XTest und prüft: Gruppierung (5 Nachrichten, Datum und Ordner je Nachricht wie am Server,
# Eingang und Gesendet gemischt), Gelesen-Timer, Markieren und Gelesen je Nachricht, Aufklappen (HTML mit
# Anhang und gesperrtem entferntem Bild, Höhe nach Inhalt) und Zuklappen, Markieren an der Konversation
# (alle 5), Umschalter Strg+T flach/Konversation. Flags unabhängig am Server (tests/testdaten.py
# --konversation-flags, IMAP EXAMINE bzw. Graph GET); am Ende Ausgangszustand wie vorher. Danach je Konto
# ein Bild dunkel (aufgeklappt). #415: M365_DEBUG=1, Graph-Anfragen als „[graph] METHODE Pfad“ im Log;
# ohne Bedienung keine schreibende Anfrage, insgesamt höchstens eine je eigener Flag-Änderung. Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“; Exit 0 ohne Fehler.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m4-357}"
KONTEN="${KONTEN:-KONTO_IMAP_2@example.invalid KONTO_M365@example.invalid}"
FEHLER=0

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
for tool in xvfb-run gdbus python3; do command -v "$tool" >/dev/null || { echo "FEHLER  $tool fehlt"; exit 1; }; done
python3 -c 'import Xlib, PIL' 2>/dev/null || { echo "FEHLER  python3-xlib/python3-pil fehlt"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
[ "$(bus_hat_app)" = "(false,)" ] || { echo "FEHLER  Session-Bus: $APP_ID läuft schon"; exit 1; }
mkdir -p "$OUT"

SCHEMA="$OUT/schema"
mkdir -p "$SCHEMA"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA/"
printf '[de.freiraum_medien.Revolution]\nconversations=true\nwindow-width=1400\nwindow-height=900\n' \
  >"$SCHEMA/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA" || { echo "FEHLER  Schema: glib-compile-schemas $SCHEMA"; exit 1; }
# Bild dunkel ohne Gelesen-Timer: sonst änderte der Lauf Nr. 5 am Server (beobachtet r415, Lauf dunkel).
BILD="$OUT/schema-bild"
mkdir -p "$BILD"
cp "$SCHEMA/"*.gschema.xml "$BILD/"
printf '[de.freiraum_medien.Revolution]\nconversations=true\nmark-seen=false\nwindow-width=1400\nwindow-height=900\n' \
  >"$BILD/90_test.gschema.override"
glib-compile-schemas --strict "$BILD" || { echo "FEHLER  Schema: glib-compile-schemas $BILD"; exit 1; }

if timeout 900 python3 "$APP_DIR/tests/testdaten.py" --konversation >"$OUT/testdaten.txt" 2>&1; then
  echo "OK      Testdaten: $(grep -c '^KONVERSATION ' "$OUT/testdaten.txt") Ordner"
else
  echo "FEHLER  Testdaten: tests/testdaten.py --konversation (siehe $OUT/testdaten.txt)"; exit 1
fi

lauf () {
  local name="$1" konto="$2" ordner="$3" schema="$4" nur_bild="${5:-0}" rundir rc=0
  rundir="$(mktemp -d)"
  env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$([ "$nur_bild" = 1 ] && echo "$BILD" || echo "$SCHEMA")" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    GTK_A11Y=none NO_AT_BRIDGE=1 ADW_DEBUG_COLOR_SCHEME="$schema" \
    G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals REVOLUTION_FOLDER="$ordner" M365_DEBUG=1 \
    REVOLUTION_MESSAGE="Konversation Marktstand" NUR_BILD="$nur_bild" \
    BIN="$BIN" OUT="$OUT" NAME="$name" KONTO="$konto" ORDNER="$ordner" APP_DIR="$APP_DIR" \
    xvfb-run -a -s "-screen 0 1400x900x24" python3 -I "$APP_DIR/tests/konversationen-treiber.py" || rc=$?
  rm -rf "$rundir"
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  return "$rc"
}

for konto in $KONTEN; do
  name="m4-357-${konto%%@*}"
  # „KONVERSATION <Adresse> <UID>/<Ordner> <Anzahl>“, erster Ordner ist der Posteingang.
  ordner="$(sed -n "s#^KONVERSATION $konto \(.*\) [0-9]*\$#\1#p" "$OUT/testdaten.txt" | head -1)"
  [ -n "$ordner" ] || { echo "FEHLER  $konto: Posteingang nicht in $OUT/testdaten.txt"; FEHLER=$((FEHLER + 1)); continue; }
  lauf "$name" "$konto" "$ordner" prefer-light | tee "$OUT/$name.txt"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))
  lauf "$name-dunkel" "$konto" "$ordner" prefer-dark 1 | tee "$OUT/$name-dunkel.txt"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name-dunkel.txt")))
done

echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
