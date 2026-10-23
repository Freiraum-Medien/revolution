#!/usr/bin/env bash
# Erstes Öffnen eines Ordners ohne Cache (#410): keine doppelten Zeilen, Anzahl = Server.
#
# Aufruf: tests/erstes-oeffnen.sh KONTO-UID/ORDNER [AUSGABEORDNER]   (Standard BUILD/m4-410)
#   KONTO-UID/ORDNER  IMAP-Ordner wie REVOLUTION_FOLDER, z. B. <UID>/INBOX/Zander; nur gelesen
#                     (die App wählt keine Nachricht, setzt also keine Flags)
#   BUILD             Build-Ordner, Standard build/ im Repo (vorher meson compile)
#   LAEUFE            App-Starts, Standard 3
#   TIMEOUT           Sekunden je Start bis „vollständig“, Standard 300
#   NACHLAUF          Sekunden nach „vollständig“ für spätere Live-Änderungen, Standard 20
#
# Je Start ein frisches XDG_CACHE_HOME (Ordner im Ausgabeordner, danach gelöscht), also ohne
# Camel-Summary (~/.cache/revolution bleibt unberührt). Start über scripts/screenshot.sh (eigener
# Xvfb, echter Session-Bus, Einstellungen im Speicher) mit G_MESSAGES_DEBUG=revolution,
# G_DEBUG=fatal-criticals und REVOLUTION_FOLDER. Geprüft je Start:
#   Duplikate  jede Zeile „Nachrichten: … vollständig N … M verschiedene UIDs“ und „… Änderung …,
#              Liste N, M verschiedene UIDs“ (src/mail/mail-view.c) hat N = M
#   Anzahl     letzter Stand der Liste (letzte dieser Zeilen) = Server laut
#              tests/testdaten.py --anzahl (EXAMINE, UID SEARCH, nur lesend), vor und nach dem Start
#   Log        nur DEBUG-Zeilen (wie tests/smoke.sh (d)); Beenden Exit 0, App nicht mehr auf dem Bus
# Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“, dazu „WERT …“; Exit 0 ohne Fehler.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/src/revolution"
ORDNER="${1:-}"
OUT="${2:-$BUILD/m4-410}"
LAEUFE="${LAEUFE:-3}"
TIMEOUT="${TIMEOUT:-300}"
NACHLAUF="${NACHLAUF:-20}"
FEHLER=0

ok () { echo "OK      $*"; }
fehler () { echo "FEHLER  $*"; FEHLER=$((FEHLER + 1)); }
wert () { echo "WERT    $*"; }

EGL_RAUSCHEN='libEGL warning: DRI3 error: Could not get DRI3 device
libEGL warning: Ensure your X server supports DRI3 to get accelerated rendering'

[[ "$ORDNER" == */* ]] || { sed -n '4,10p' "$0" >&2; exit 2; }
[[ "$LAEUFE" =~ ^[0-9]+$ && "$TIMEOUT" =~ ^[0-9]+$ && "$NACHLAUF" =~ ^[0-9]+$ ]] \
  || { echo "LAEUFE/TIMEOUT/NACHLAUF als Zahl" >&2; exit 2; }
KONTO="${ORDNER%%/*}"
NAME_="${ORDNER#*/}"
KEY="$KONTO $NAME_"
mkdir -p "$OUT"

[ -x "$BIN" ] || { fehler "Build: $BIN fehlt (meson compile)"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}

server () {
  timeout 120 python3 "$APP_DIR/tests/testdaten.py" --anzahl "$KONTO" "$NAME_" 2>&1 \
    | sed -nE 's#^ANZAHL .* ([0-9]+) EXISTS [0-9]+$#\1#p'
}

debug () { sed -nE 's/^\(revolution:[0-9]+\): revolution-DEBUG: [0-9:.]+: //p' "$1"; }

warte () {
  local log="$1" ready="$2"
  for _ in $(seq "$TIMEOUT"); do
    if [ -f "$log" ] && debug "$log" | grep -qF "Nachrichten: $KEY vollständig"; then
      sleep "$NACHLAUF"; echo fertig >"$ready"; return
    fi
    sleep 1
  done
  echo Zeitlimit >"$ready"
}

lauf () {
  local name="$1" log="$OUT/$1.log" cache="$OUT/$1-cache" watcher vorher nachher
  rm -rf "$cache" "$OUT/$name".{log,out,png,ready,quit} "$OUT/$name-rest.txt"
  mkdir -p "$cache"
  [ "$(bus_hat_app)" = "(false,)" ] || { fehler "$name Session-Bus: $APP_ID läuft schon"; return; }
  vorher="$(server)"
  warte "$log" "$OUT/$name.ready" & watcher=$!
  env XDG_CACHE_HOME="$cache" G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals REVOLUTION_FOLDER="$ORDNER" \
    BUILD="$BUILD" OUT_DIR="$OUT" LOG="$log" READY="$OUT/$name.ready" QUIT="$OUT/$name.quit" \
    WAIT=$((TIMEOUT + NACHLAUF + 5)) \
    "$APP_DIR/scripts/screenshot.sh" "$name" prefer-light 1200x760 >"$OUT/$name.out" 2>&1 \
    || fehler "$name: scripts/screenshot.sh Exit $? (siehe $OUT/$name.out)"
  kill "$watcher" 2>/dev/null; wait "$watcher" 2>/dev/null
  nachher="$(server)"

  # „vollständig N nach … ms, M verschiedene UIDs“ bzw. „Änderung …, Liste N, M verschiedene UIDs“
  local staende n_zeilen doppelt letzt
  staende="$(debug "$log" | sed -nE "s#^Nachrichten: $KEY (vollständig ([0-9]+) nach [^,]*|Änderung .*, Liste ([0-9]+)), ([0-9]+) verschiedene UIDs\$#\2\3 \4#p")"
  n_zeilen="$(printf '%s\n' "$staende" | grep -c .)"
  doppelt="$(printf '%s\n' "$staende" | awk 'NF == 2 && $1 != $2 { print $1 - $2 }' | paste -sd, -)"
  letzt="$(printf '%s\n' "$staende" | tail -1 | cut -d' ' -f1)"
  wert "$name Durchgänge: $(debug "$log" | grep -cE "^Nachrichten: $KEY (Cache|Server) ") Cache/Server, $(debug "$log" | grep -cE "^Nachrichten: $KEY Änderung ") Änderungen, Listenstände $n_zeilen"
  debug "$log" | grep -E "^Nachrichten: $KEY (Cache|Server|vollständig|Abgleich|Änderung) " | sed "s/^/WERT    $name Log: /"
  if [ "$n_zeilen" -gt 0 ] && [ -z "$doppelt" ]; then
    ok "$name Duplikate: 0 doppelte UIDs in $n_zeilen Listenständen"
  else
    fehler "$name Duplikate: ${doppelt:-keine Listenstände im Log} doppelte UIDs"
  fi
  if [ -n "$letzt" ] && [ "$letzt" = "$vorher" ] && [ "$letzt" = "$nachher" ]; then
    ok "$name Anzahl: Liste $letzt = Server $vorher (vorher) / $nachher (nachher)"
  else
    fehler "$name Anzahl: Liste ${letzt:-?}, Server vorher ${vorher:-?}, nachher ${nachher:-?}"
  fi

  grep -vE '^\(revolution:[0-9]+\): revolution-DEBUG: ' "$log" | grep -vxF "$EGL_RAUSCHEN" >"$OUT/$name-rest.txt"
  if [ -s "$OUT/$name-rest.txt" ]; then
    fehler "$name Log: $(wc -l <"$OUT/$name-rest.txt") unerwartete Zeilen (siehe $OUT/$name-rest.txt)"
  else
    ok "$name Log: nur DEBUG-Zeilen"
  fi
  local pid rc
  read -r pid rc 2>/dev/null <"$OUT/$name.quit" || rc="keine QUIT-Datei"
  if [ "$rc" = 0 ] && [ "$(bus_hat_app)" = "(false,)" ]; then
    ok "$name Beenden: Exit 0, NameHasOwner = false"
  else
    fehler "$name Beenden: Exit ${rc:-?}, Bus $(bus_hat_app)"
  fi
  wert "$name Cache: $(du -sh "$cache" | cut -f1) in $cache (gelöscht)"
  rm -rf "$cache"
}

for i in $(seq "$LAEUFE"); do
  lauf "m4-410-lauf$i"
done

echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
