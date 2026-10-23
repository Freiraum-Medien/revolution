#!/usr/bin/env bash
# Gelesen, Markieren, Aktualisieren (#353) gegen die echten Testkonten.
#
# Aufruf: tests/flags.sh [AUSGABEORDNER]   (Standard BUILD/m4-353)
#   BUILD    Build-Ordner, Standard build/ im Repo (vorher meson compile)
#   KONTEN   Testkonten, Standard „KONTO_IMAP_1@example.invalid KONTO_M365@example.invalid KONTO_GOOGLE@example.invalid“
#   LAST     1 (Standard): zusätzlich Lauf in Revolution-Last von privat@ (Auswahl/Scrollposition)
#   IDLE     1 (Standard): zusätzlich Lauf im Posteingang von privat@ (neue/gelöschte Mail per IDLE)
#
# Je Konto ein App-Start im eigenen Xvfb (wie scripts/screenshot.sh: echter Session-Bus,
# Einstellungen im Speicher, X11) mit REVOLUTION_FOLDER = Ordner Revolution-Test des Kontos
# (eine Mail: die Einladung aus tests/testdaten.py). Ein Python-Treiber bedient die App per XTest
# (python3-xlib; xdotool ist nicht installiert, gleicher Mechanismus) und prüft jede Änderung
# unabhängig direkt am Server: tests/testdaten.py --flags (IMAP EXAMINE + UID FETCH FLAGS bzw.
# Graph GET, nur lesend). Schritte: Auswahl (Gelesen nach Verzögerung), Strg+Umschalt+K, Strg+K,
# Strg+S, Strg+Umschalt+S, Kontextmenü (Rechtsklick, Pfeil ab, Eingabe), Auswahl neu (Gelesen nach
# 1,5 s), Wischen rechts/links (Maus-Ziehen), Stern-Knopf der Nachricht, serverseitige Änderung
# (tests/testdaten.py --server) mit IDLE bzw. F5, bei IMAP zusätzlich neue Mail (APPEND) und
# Löschen. Am Ende Ausgangszustand am Server wie vorher. Screenshots je Schritt in AUSGABEORDNER.
# Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“, „WERT …“ für Messwerte; Exit 0 ohne Fehler.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m4-353}"
KONTEN="${KONTEN:-KONTO_IMAP_1@example.invalid KONTO_M365@example.invalid KONTO_GOOGLE@example.invalid}"
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

lauf () {
  local name="$1" konto="$2" ordner="$3" rundir rc=0
  rundir="$(mktemp -d)"
  env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$BUILD/data" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    GTK_A11Y=none NO_AT_BRIDGE=1 ADW_DEBUG_COLOR_SCHEME=prefer-light \
    G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals REVOLUTION_FOLDER="$ordner" \
    BIN="$BIN" OUT="$OUT" NAME="$name" KONTO="$konto" ORDNER="$ordner" APP_DIR="$APP_DIR" \
    xvfb-run -a -s "-screen 0 1200x760x24" python3 -I "$APP_DIR/tests/flags-treiber.py" || rc=$?
  rm -rf "$rundir"
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  return "$rc"
}

for konto in $KONTEN; do
  name="m4-353-${konto%%@*}"
  if ! python3 "$APP_DIR/tests/testdaten.py" --flags "$konto" >"$OUT/$name-vorher.txt" 2>&1; then
    echo "FEHLER  $konto: tests/testdaten.py --flags (siehe $OUT/$name-vorher.txt)"; FEHLER=$((FEHLER + 1)); continue
  fi
  ordner="$(sed -n 's/^ORDNER //p' "$OUT/$name-vorher.txt")"
  lauf "$name" "$konto" "$ordner" | tee "$OUT/$name.txt"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))
done

# IDLE: imapx hält IDLE am Posteingang (camel-imapx-conn-manager.c:1266-1285), daher dort.
if [ "${IDLE:-1}" = 1 ]; then
  uid="$(python3 "$APP_DIR/tests/testdaten.py" --flags KONTO_IMAP_1@example.invalid 2>/dev/null | sed -n 's#^ORDNER \([^/]*\)/.*#\1#p')"
  lauf m4-353-idle KONTO_IMAP_1@example.invalid "$uid/INBOX" | tee "$OUT/m4-353-idle.txt"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/m4-353-idle.txt")))
fi

if [ "${LAST:-1}" = 1 ]; then
  ordner="$(python3 "$APP_DIR/tests/testdaten.py" --last 3000 --dry-run 2>/dev/null | sed -n 's/^LAST \(.*\) [0-9]*$/\1/p')"
  lauf m4-353-last KONTO_IMAP_1@example.invalid "$ordner" | tee "$OUT/m4-353-last.txt"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/m4-353-last.txt")))
fi

echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
