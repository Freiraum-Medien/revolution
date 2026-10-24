#!/usr/bin/env bash
# Wechsel weg von einer großen Mail (#412) gegen ein echtes Testkonto.
#
# Aufruf: tests/wechsel.sh [AUSGABEORDNER]   (Standard BUILD/m4-412)
#   BUILD    Build-Ordner, Standard build/ im Repo (vorher meson compile)
#   KONTO    Testkonto (IMAP, nicht privat@), Standard KONTO_IMAP_2@example.invalid
#   PAUSEN   Sekunden zwischen Pfeil ab zu Schwer und Pfeil ab zu Ziel, Standard „0.2 0.24 0.3“
#   WDH      Wiederholungen je Pause, Standard 3
#   PAPIERKORB  1 (Standard): die drei Testmails danach in den Papierkorb (nicht endgültig gelöscht)
#   LAUF_TIMEOUT  Sekunden für den Treiber-Lauf, Standard 1200; TD_TIMEOUT je tests/testdaten.py
#
# tests/testdaten.py --schwer legt drei Mails per APPEND in den Posteingang (Start, Schwer ≥ 500 KB,
# Ziel ungelesen). Ein App-Start im eigenen Xvfb (echter Session-Bus, Einstellungen im Speicher,
# X11), Treiber tests/wechsel-treiber.py (XTest, Bilder per XGetImage). Eine Zeile je Prüfung
# „OK …“ bzw. „FEHLER …“, „WERT …“ für Messwerte; Exit 0 ohne Fehler. Abbrüche: tests/abbruch.sh.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m4-412}"
KONTO="${KONTO:-KONTO_IMAP_2@example.invalid}"
LAUF_TIMEOUT="${LAUF_TIMEOUT:-1200}"
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
. "$APP_DIR/tests/abbruch.sh"

td --schwer "$KONTO" neu >"$OUT/testdaten.txt" 2>&1
uid="$(sed -n 's/^ORDNER \([^/]*\)\/INBOX$/\1/p' "$OUT/testdaten.txt")"
uids="$(for r in Start Schwer Ziel; do sed -n "s/^SCHWER $r \([0-9]*\) .*/\1/p" "$OUT/testdaten.txt"; done | xargs)"
if [ -z "$uid" ] || [ "$(echo "$uids" | wc -w)" != 3 ]; then
  echo "FEHLER  Testdaten: Konto-UID „$uid“, Mails „$uids“ (siehe $OUT/testdaten.txt)"
else
  grep '^SCHWER' "$OUT/testdaten.txt" | sed 's/^/WERT    /'
  rundir="$(mktemp -d)"
  rc=0
  timeout -k 10 "$LAUF_TIMEOUT" env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$BUILD/data" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    GTK_A11Y=none NO_AT_BRIDGE=1 ADW_DEBUG_COLOR_SCHEME=prefer-light \
    G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals REVOLUTION_FOLDER="$uid/INBOX" REVOLUTION_MESSAGE="Pruef412 Start" \
    BIN="$BIN" OUT="$OUT" NAME=m4-412 ORDNER="$uid/INBOX" UIDS="$uids" \
    xvfb-run -a -s "-screen 0 1200x760x24" python3 -I "$APP_DIR/tests/wechsel-treiber.py" | tee "$OUT/m4-412.txt"
  rc="${PIPESTATUS[0]}"
  rm -rf "$rundir"
  for _ in $(seq 10); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  m4-412: App nach dem Lauf noch auf dem Bus" | tee -a "$OUT/m4-412.txt"
  teilschritt m4-412 "$rc" "$OUT/m4-412.txt"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/m4-412.txt")))
fi

if [ "${PAPIERKORB:-1}" = 1 ]; then
  td --schwer "$KONTO" papierkorb >"$OUT/papierkorb.txt" 2>&1
  sed 's/^/        /' "$OUT/papierkorb.txt"
fi

abbruch_bilanz
echo "Ergebnis: $FEHLER Fehler"
[ "$FEHLER" = 0 ]
