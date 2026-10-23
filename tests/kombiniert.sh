#!/usr/bin/env bash
# Kombinierter Eingang / Favoriten (#351) gegen die echten Konten.
#
# Aufruf: tests/kombiniert.sh [AUSGABEORDNER]   (Standard BUILD/m4-351)
#   BUILD  Build-Ordner, Standard build/ im Repo (vorher meson compile)
#   KONTO  Testkonto für Flags und Live-Mail, Standard KONTO_IMAP_2@example.invalid (IMAP mit IDLE;
#          nicht privat@: Quota)
#
# Zwei App-Starts im eigenen Xvfb (wie tests/flags.sh: echter Session-Bus, Einstellungen im
# Speicher, X11) mit REVOLUTION_FOLDER=revolution-favoriten/inbox („Alle Eingänge“), Treiber
# tests/kombiniert-treiber.py (XTest, python3-xlib):
#   1. m4-351-dunkel: nur laden und Screenshot; Log mit IMAP-Befehlen (CAMEL_DEBUG=imapx:command)
#      und Graph-Anfragen (M365_DEBUG=1, nur Methode und Pfad). Beleg nur lesend: in diesem Lauf
#      kein schreibender IMAP-Befehl und keine schreibende Graph-Anfrage (kein Konto schreibt);
#      Last: SELECT/EXAMINE je Ordner. Danach tests/testdaten.py --kombiniert LOG: je Konto
#      Posteingang am Server (nur lesend) = „Favoriten: Anzahl inbox“ der App, Summe = Liste,
#      ungelesen = Zähler „Alle Eingänge“.
#   2. m4-351-hell: Live-Mail per APPEND in den Posteingang von KONTO (vor dem Start), dann
#      Gelesen nach Verzögerung, Markieren, Markierung entfernen, Ungelesen aus „Alle Eingänge“,
#      jede Änderung am Server von KONTO geprüft; neue Mail und Löschen live (IDLE); am Ende sind
#      die Live-Mails gelöscht (Ausgangszustand).
# Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“, „WERT …“ für Messwerte; Exit 0 ohne Fehler.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m4-351}"
KONTO="${KONTO:-KONTO_IMAP_2@example.invalid}"
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
  local name="$1" schema="$2" rundir rc=0
  rundir="$(mktemp -d)"
  env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$BUILD/data" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    GTK_A11Y=none NO_AT_BRIDGE=1 ADW_DEBUG_COLOR_SCHEME="$schema" \
    G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals REVOLUTION_FOLDER=revolution-favoriten/inbox \
    CAMEL_DEBUG=imapx:command M365_DEBUG=1 \
    BIN="$BIN" OUT="$OUT" NAME="$name" KONTO="$KONTO" APP_DIR="$APP_DIR" \
    xvfb-run -a -s "-screen 0 1200x760x24" python3 -I "$APP_DIR/tests/kombiniert-treiber.py" || rc=$?
  rm -rf "$rundir"
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  return "$rc"
}

# 1. Nur lesend: laden, Screenshot dunkel, Zählung je Konto gegen den Server.
name=m4-351-dunkel
lauf "$name" prefer-dark | tee "$OUT/$name.txt"
log="$OUT/$name.log"
schreibend="$(grep -E '^\[imapx:[A-Za-z]\] (APPEND|STORE|EXPUNGE|COPY|MOVE|CREATE|DELETE|RENAME|SUBSCRIBE|UNSUBSCRIBE)\b' "$log" | sort | uniq -c)"
# Graph-$batch (POST) gilt als lesend, wenn jede Teilanfrage GET ist (Rumpf im Log als [graph-batch]).
graph_schreibend="$(grep -E '^\[graph\] (POST|PATCH|PUT|DELETE)\b' "$log" | grep -vE '^\[graph\] POST /v1\.0/\$batch$' | sort | uniq -c)"
batch_post="$(grep -cE '^\[graph\] POST /v1\.0/\$batch$' "$log")"
batch_teile="$(grep -cE '^\[graph-batch\] ' "$log")"
batch_schreibend="$(grep -E '^\[graph-batch\] ' "$log" | grep -vE '^\[graph-batch\] GET ' | sort | uniq -c)"
if [ "$batch_post" -gt 0 ] && [ "$batch_teile" = 0 ]; then batch_schreibend="$batch_post \$batch ohne erkannte Teilanfragen"; fi
graph_schreibend="$graph_schreibend$batch_schreibend"
echo "WERT    $name: Graph-\$batch: $batch_post Anfragen, Teilanfragen $(grep -oE '^\[graph-batch\] [A-Z]+' "$log" | sort | uniq -c | awk '{printf "%s%s×%s", s, $1, $3; s=", "}')" | tee -a "$OUT/$name.txt"
if [ -z "$schreibend$graph_schreibend" ]; then
  echo "OK      $name: nur lesend: $(grep -cE '^\[imapx:' "$log") IMAP-Befehle ($(grep -oE '^\[imapx:[A-Za-z]\] [A-Z]+' "$log" | awk '{print $2}' | sort | uniq -c | awk '{printf "%s%s×%s", s, $1, $2; s=", "}')), $(grep -c '^\[graph\]' "$log") Graph-Anfragen ($(grep -oE '^\[graph\] [A-Z]+' "$log" | sort | uniq -c | awk '{printf "%s%s×%s", s, $1, $3; s=", "}')), kein Schreibzugriff"
else
  echo "FEHLER  $name: Schreibzugriffe im Lauf ohne Bedienung: $schreibend $graph_schreibend"
fi | tee -a "$OUT/$name.txt"
echo "WERT    $name: SELECT/EXAMINE je Ordner: $(grep -oE '^\[imapx:[A-Za-z]\] (SELECT|EXAMINE) .*' "$log" | sed -E "s/ \(.*//; s/'$//; s/^\[imapx:([A-Za-z])\] [A-Z]+ /\1:/" | sort | uniq -c | awk '{printf "%s%s×%s", s, $1, $2; s=", "}')" | tee -a "$OUT/$name.txt"
if python3 "$APP_DIR/tests/testdaten.py" --kombiniert "$log" >"$OUT/$name-server.txt" 2>&1; then
  echo "OK      $name: Zählung je Konto = Server ($(grep -c '^KOMBINIERT' "$OUT/$name-server.txt") Konten; $(grep '^SUMME' "$OUT/$name-server.txt"))"
else
  echo "FEHLER  $name: Zählung weicht ab (siehe $OUT/$name-server.txt): $(grep ABWEICHUNG "$OUT/$name-server.txt" | tr '\n' ' ')"
fi | tee -a "$OUT/$name.txt"
FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))

# 2. Bedienung im Testkonto: Live-Mail vorher anlegen, danach löscht der Treiber sie am Server.
name=m4-351-hell
python3 "$APP_DIR/tests/testdaten.py" --server "$KONTO" weg INBOX >/dev/null 2>&1
python3 "$APP_DIR/tests/testdaten.py" --server "$KONTO" neu INBOX >"$OUT/$name-vorher.txt" 2>&1
lauf "$name" prefer-light | tee "$OUT/$name.txt"
rest="$(python3 "$APP_DIR/tests/testdaten.py" --live-flags "$KONTO" | grep -c '^FLAGS')"
if [ "$rest" = 0 ]; then echo "OK      $name: Ausgangszustand, keine Live-Mail mehr im Posteingang von $KONTO"
else echo "FEHLER  $name: $rest Live-Mails im Posteingang von $KONTO übrig"; fi | tee -a "$OUT/$name.txt"
FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))

echo "Ergebnis: $FEHLER Fehler"
[ "$FEHLER" = 0 ]
