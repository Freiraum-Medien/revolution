#!/usr/bin/env bash
# Echo-Schutz bei Verschieben und Papierkorb (#416) gegen KONTO_M365@example.invalid (microsoft365).
#
# Aufruf: flock build/.run.lock tests/verschieben.sh [AUSGABEORDNER]   (Standard BUILD/m5-416)
#   BUILD   Build-Ordner, Standard build/ im Repo (vorher meson compile)
#   LAEUFE  Anzahl Läufe, Standard 3
#   ROH     1: Gegenprobe ohne Echo-Schutz (tests/verschieben --roh); dann sind Echos erwartet
#
# Je Schritt ein Aufruf von tests/verschieben (ohne Oberfläche, Camel-Daten der App, daher unter
# .run.lock und nur, wenn die App nicht läuft). Graph-Anfragen (M365_DEBUG=1) nur als
# „[graph] METHODE Pfad“ bzw. „[graph-batch] …“ ins Log; Rümpfe schreibender Anfragen ohne
# Kopfzeilen (kein Token) nach verschieben-rumpf.log. Vor jedem Schritt mit Echo-Gefahr ändert
# tests/testdaten.py --server die Einladung am Server (importance high), danach zurück:
#   Papierkorb: Einladung Revolution-Test → Papierkorb; Änderung im Papierkorb; Papierkorb holen
#               (get_trash gleicht ihn ab, camel-m365-store.c:1459); Einladung zurück.
#   Ordner:     Änderung an der Einladung in Revolution-Test; „Anzeige Inline-Bild (cid)“
#               Posteingang → Revolution-Test (transfer gleicht das Ziel ab,
#               camel-m365-folder.c:1553) und zurück.
# Nach jedem Schritt synchronisiert tests/verschieben alle offenen Ordner (wie der Zeitgeber
# store-changes). Prüfung je Schritt: schreibend nur POST …/move, kein PATCH (Echo). Das nur
# lesend freigegebene NUR_LESEND_3@example.invalid lädt die Session gar nicht (REVOLUTION_SKIP_ACCOUNTS).
# Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“; Exit 0 ohne Fehler.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/tests/verschieben"
OUT="${1:-$BUILD/m5-416}"
LAEUFE="${LAEUFE:-3}"
ROH="${ROH:-0}"
KONTO=KONTO_M365@example.invalid
GEOPS=332762f8d2b105ea5f2d32ef30a7ac32d4ce684e
EINLADUNG='[Revolution-Test] Einladung: Abstimmung Testdaten'
MAIL='[Revolution-Test] Anzeige Inline-Bild (cid)'
FEHLER=0

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
[ "$(gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
      -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null)" = "(false,)" ] ||
  { echo "FEHLER  Session-Bus: $APP_ID läuft"; exit 1; }
mkdir -p "$OUT"
LOG="$OUT/verschieben.log"
RUMPF="$OUT/verschieben-rumpf.log"
: >"$LOG"
: >"$RUMPF"

# Ausgabe von tests/verschieben ohne Anmeldedaten: Graph-Anfragen als Methode und Pfad, Rümpfe
# schreibender Anfragen ohne Kopfzeilen nach RUMPF (Muster tests/konversationen-treiber.py).
FILTER='
import re, sys
rumpf = open(sys.argv[1], "a")
schreibend = False
for zeile in sys.stdin:
    g = re.match(r"^> (GET|POST|PATCH|PUT|DELETE|HEAD) (\S+)", zeile)
    if g:
        schreibend = g.group(1) not in ("GET", "HEAD")
        print("[graph] %s %s" % (g.group(1), g.group(2).split("?")[0]))
        if schreibend:
            rumpf.write(zeile)
        continue
    if zeile.startswith("<") or not zeile.startswith(">"):
        schreibend = False
    if zeile.startswith("> {") and schreibend:
        rumpf.write(re.sub(r"Bearer \S+", "Bearer REDACTED", zeile))
        for methode, url in re.findall(r"\"method\"\s*:\s*\"([A-Z]+)\"\s*,\s*\"url\"\s*:\s*\"([^\"?]*)", zeile):
            print("[graph-batch] %s %s" % (methode, url))
    elif zeile.strip() and not zeile.startswith((">", "<")) and "Bearer" not in zeile:
        print(zeile, end="")
'

# schritt NAME [--roh] ARGUMENTE…: ein Aufruf; prüft Exit und schreibende Graph-Anfragen.
schritt () {
  local name="$1" start rc schreib echo_ moves
  shift
  start=$(wc -l <"$LOG")
  echo "== $name" | tee -a "$RUMPF" >>"$LOG"
  G_MESSAGES_DEBUG=revolution M365_DEBUG=1 REVOLUTION_TEST_HOOKS=1 REVOLUTION_SKIP_ACCOUNTS="$GEOPS" \
    timeout -k 10 300 "$BIN" "$@" 2>&1 | python3 -I -c "$FILTER" "$RUMPF" >>"$LOG"
  rc=${PIPESTATUS[0]}
  schreib="$(tail -n +"$((start + 1))" "$LOG" | grep -E '^\[graph(-batch)?\] (POST|PATCH|PUT|DELETE) ' |
             grep -vE '^\[graph\] POST \S*/\$batch$')"
  moves="$(grep -cE ' POST \S*/messages/\S+/move$' <<<"$schreib")"
  echo_="$(grep -vE ' POST \S*/messages/\S+/move$' <<<"$schreib" | grep -v '^$')"
  if [ "$rc" != 0 ]; then
    echo "FEHLER  $name: Exit $rc ($(tail -n +"$((start + 1))" "$LOG" | grep -E '^Fehler:' | head -1))"
    FEHLER=1
  elif [ -n "$echo_" ]; then
    echo "FEHLER  $name: Echo: $(sed -E 's#/messages/(\S{8})\S*#/messages/\1…#' <<<"$echo_" | sort | uniq -c | tr -s ' ' | paste -sd ';')"
    FEHLER=1
  else
    echo "OK      $name: $moves× POST …/move, kein Echo ($(tail -n +"$((start + 1))" "$LOG" | grep -c '^\[graph\]') Graph-Anfragen)"
  fi
  tail -n +"$((start + 1))" "$LOG" | grep -E 'Server-Änderungen nicht zurückgeschrieben' | sed -E 's/^.*DEBUG: [0-9:.]+: /        /'
}

td () {
  timeout -k 10 300 python3 -I "$APP_DIR/tests/testdaten.py" "$@" >>"$LOG" 2>&1 ||
    { echo "FEHLER  testdaten.py $*"; FEHLER=1; }
}

roh=()
[ "$ROH" = 1 ] && roh=(--roh)

# Ausgangsstand mit Echo-Schutz (sonst gingen frühere Server-Änderungen in die Zählung ein).
schritt "Ausgang Posteingang" "$KONTO" abgleich Posteingang
schritt "Ausgang Revolution-Test" "$KONTO" abgleich Revolution-Test
schritt "Ausgang Papierkorb" "$KONTO" abgleich Papierkorb

for n in $(seq 1 "$LAEUFE"); do
  schritt "$n Papierkorb hin" "${roh[@]}" "$KONTO" Revolution-Test Papierkorb "$EINLADUNG"
  td --server "$KONTO" markiert Papierkorb
  schritt "$n Papierkorb holen" "${roh[@]}" "$KONTO" abgleich Papierkorb
  schritt "$n Papierkorb zurück" "${roh[@]}" "$KONTO" Papierkorb Revolution-Test "$EINLADUNG"
  td --server "$KONTO" unmarkiert
  schritt "$n Ausgang Revolution-Test" "$KONTO" abgleich Revolution-Test

  td --server "$KONTO" markiert
  schritt "$n Ordner hin" "${roh[@]}" "$KONTO" Posteingang Revolution-Test "$MAIL"
  schritt "$n Ordner zurück" "${roh[@]}" "$KONTO" Revolution-Test Posteingang "$MAIL"
  td --server "$KONTO" unmarkiert
  schritt "$n Ausgang Revolution-Test" "$KONTO" abgleich Revolution-Test
done

exit "$FEHLER"
