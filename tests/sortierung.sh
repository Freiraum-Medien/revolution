#!/usr/bin/env bash
# Konversationsliste nach dem Server-Durchgang nach neuester Nachricht sortiert (#418), echtes Testkonto.
#
# Aufruf: tests/sortierung.sh [AUSGABEORDNER]   (Standard BUILD/m4-418)
#   BUILD     Build-Ordner, Standard build/ im Repo (vorher meson compile)
#   KONTO     Testkonto (IMAP, nicht privat@), Standard KONTO_IMAP_2@example.invalid
#   LAEUFE    Durchgänge, Standard 3
#   TIMEOUT   Sekunden je App-Start bis „vollständig“, Standard 300
#   NACHLAUF  Sekunden nach „vollständig“ bis zur Aufnahme (Live-Änderungen des Suchordners), Standard 8
#   OHNE      Konten-UIDs, die die App nicht lädt (REVOLUTION_SKIP_ACCOUNTS, #415), Standard das nur lesend
#             freigegebene NUR_LESEND_3@example.invalid
#
# Je Durchgang: tests/testdaten.py --sortierung KONTO basis legt „Pruef418 Faden“ (Datum 20.10.2026) und
# „Pruef418 Oben“ (Datum jetzt) in den Posteingang; ein erster App-Start lädt sie in den Cache
# (~/.cache/revolution wie die übrigen Tests). Dann bei beendeter App --sortierung KONTO neu: Antwort auf
# den Faden (Datum jetzt) und neue Konversation „Pruef418 Neu“ (eine Sekunde davor). Zweiter App-Start mit
# Cache, Konversationsansicht, gewählt „Pruef418 Oben“ (oberste Zeile im Cache; REVOLUTION_MESSAGE,
# mark-seen=false) wie im Beleg aus #412: der Server-Durchgang bringt beide Mails darüber. Geprüft im Log
# des zweiten Starts (src/mail/mail-view.c):
#   Cache      „… erste Zeile nach … (Cache)“: Start mit Cache
#   Reihenfolge  jede Zeile „Reihenfolge: … 0 nicht nach Datum“, die letzte nennt oben Faden, Neu, Oben
#   Auswahl    gewählt bleibt Oben (UID in jeder Zeile „Reihenfolge“)
#   Ansicht    im Bild AUSGABEORDNER/m4-418-N.png steht die gewählte Zeile (Akzentfarbe) an dritter Stelle,
#              Faden und Neu darüber sind sichtbar (Beleg #412: gewählte Zeile oben, die neueren darüber
#              außerhalb des sichtbaren Teils); Zeilenhöhe 101 px bei 1200x760
#   Ende       Beenden Exit 0, App nicht mehr auf dem Bus
# Danach alle Pruef418-Mails in den Papierkorb (UID MOVE).
# Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“, dazu „WERT …“; Exit 0 ohne Fehler. #411: Abbruch eines
# Teilschritts zählt als FEHLER (tests/abbruch.sh).
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m4-418}"
KONTO="${KONTO:-KONTO_IMAP_2@example.invalid}"
LAEUFE="${LAEUFE:-3}"
TIMEOUT="${TIMEOUT:-300}"
NACHLAUF="${NACHLAUF:-8}"
OHNE="${OHNE:-332762f8d2b105ea5f2d32ef30a7ac32d4ce684e}"
FEHLER=0

ok () { echo "OK      $*"; }
fehler () { echo "FEHLER  $*"; FEHLER=$((FEHLER + 1)); }
wert () { echo "WERT    $*"; }

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
[ "$(bus_hat_app)" = "(false,)" ] || { echo "FEHLER  Session-Bus: $APP_ID läuft schon"; exit 1; }
mkdir -p "$OUT"
. "$APP_DIR/tests/abbruch.sh"

SCHEMA="$OUT/schema"
mkdir -p "$SCHEMA"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA/"
printf '[de.freiraum_medien.Revolution]\nconversations=true\nmark-seen=false\n' >"$SCHEMA/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA" || { echo "FEHLER  Schema: glib-compile-schemas $SCHEMA"; exit 1; }

# App-Start über scripts/screenshot.sh. Aufnahme NACHLAUF Sekunden nach „<Konto> INBOX vollständig“; mit
# NEU (Datei) legt der Wächter dann erst die beiden Mails am Server ab (--sortierung KONTO neu) und wartet
# auf die Live-Änderung, die sie in die Liste bringt („Änderung (Konversationen) N neu“, N ≥ 1).
start () {
  local name="$1" neu_txt="${3:-}" log="$OUT/$1.log" ready="$OUT/$1.ready" quit="$OUT/$1.quit" rc=0
  rm -f "$ready" "$quit"
  : >"$log"
  ( neue () { grep -c 'INBOX Änderung (Konversationen) [1-9][0-9]* neu' "$log"; }
    for _ in $(seq "$TIMEOUT"); do grep -q "Nachrichten: $KONTO_UID INBOX vollständig" "$log" && break; sleep 1; done
    grep -q "Nachrichten: $KONTO_UID INBOX vollständig" "$log" || exit
    if [ -n "$neu_txt" ]; then
      vorher="$(neue)"
      td --sortierung "$KONTO" neu >"$neu_txt" 2>&1
      for _ in $(seq "$TIMEOUT"); do [ "$(neue)" -gt "$vorher" ] && break; sleep 1; done
      [ "$(neue)" -gt "$vorher" ] || exit
    fi
    sleep "$NACHLAUF"
    touch "$ready" ) &
  timeout -k 10 "$((2 * TIMEOUT + NACHLAUF + 120))" env BUILD="$BUILD" OUT_DIR="$OUT" SCHEMA_DIR="$SCHEMA" \
    LOG="$log" READY="$ready" QUIT="$quit" WAIT="$((2 * TIMEOUT + NACHLAUF))" \
    G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals REVOLUTION_FOLDER="$KONTO_UID/INBOX" \
    REVOLUTION_TEST_HOOKS=1 REVOLUTION_SKIP_ACCOUNTS="$OHNE" ${2:+REVOLUTION_MESSAGE="$2"} \
    "$APP_DIR/scripts/screenshot.sh" "$name" prefer-light >"$OUT/$name.screenshot.txt" 2>&1 || rc=$?
  wait
  [ -e "$ready" ] || fehler "$name: „vollständig“${neu_txt:+ bzw. neue Konversation} nicht in $TIMEOUT s ($log)"
  [ "$rc" = 0 ] || fehler "$name: scripts/screenshot.sh Exit $rc ($OUT/$name.screenshot.txt)"
  grep -q ' 0$' "$quit" 2>/dev/null || fehler "$name: Beenden $(cat "$quit" 2>/dev/null)"
  [ "$(bus_hat_app)" = "(false,)" ] || fehler "$name: App nach dem Lauf noch auf dem Bus"
}

for n in $(seq "$LAEUFE"); do
  td --sortierung "$KONTO" basis >"$OUT/basis-$n.txt" 2>&1
  KONTO_UID="$(sed -n 's/^ORDNER \([^/]*\)\/INBOX$/\1/p' "$OUT/basis-$n.txt")"
  [ -n "$KONTO_UID" ] || { fehler "Lauf $n: Testdaten basis ($OUT/basis-$n.txt)"; continue; }
  start "m4-418-$n-cache"
  start "m4-418-$n" "Pruef418 Oben" "$OUT/neu-$n.txt"
  td --sortierung "$KONTO" papierkorb >"$OUT/papierkorb-$n.txt" 2>&1
  grep '^SORTIERUNG' "$OUT/neu-$n.txt" | sed 's/^/WERT    Lauf '"$n"': /'
  faden="$(sed -n 's/^SORTIERUNG Faden \([0-9]*\) .*/\1/p' "$OUT/neu-$n.txt" | tail -1)"
  neu="$(sed -n 's/^SORTIERUNG Neu \([0-9]*\) .*/\1/p' "$OUT/neu-$n.txt" | tail -1)"
  oben="$(sed -n 's/^SORTIERUNG Oben \([0-9]*\) .*/\1/p' "$OUT/neu-$n.txt" | tail -1)"
  log="$OUT/m4-418-$n.log"

  if grep -q "INBOX erste Zeile nach .* (Cache)" "$log"; then ok "Lauf $n: Start mit Cache"; else fehler "Lauf $n: kein Cache-Durchgang"; fi
  wert "Lauf $n: $(grep -c 'Reihenfolge:' "$log") Stände der Liste, letzter: $(grep 'Reihenfolge:' "$log" | tail -1 | sed 's/.*Reihenfolge: //')"
  falsch="$(grep 'Reihenfolge:' "$log" | grep -vc ' 0 nicht nach Datum')"
  [ "$(grep -c 'Reihenfolge:' "$log")" -gt 0 ] && [ "$falsch" = 0 ] \
    && ok "Lauf $n: jeder Stand nach neuester Nachricht absteigend" || fehler "Lauf $n: $falsch Stände nicht nach Datum"
  letzte="$(grep 'Reihenfolge:' "$log" | tail -1)"
  # Konversations-UIDs im Suchordner: 8 Zeichen Präfix + UID im Quellordner (EDS camel-vee-message-info.c:574).
  if [[ -n "$faden" && -n "$neu" && -n "$oben" && "$letzte" =~ oben\ .{8}$faden\ .{8}$neu\ .{8}$oben, ]]; then
    ok "Lauf $n: oben Faden ($faden), Neu ($neu), Oben ($oben)"
  else fehler "Lauf $n: oben nicht Faden ($faden), Neu ($neu), Oben ($oben): $letzte"; fi
  gewaehlt="$(grep 'Reihenfolge:' "$log" | sed -n 's/.* gewählt \(.*\)$/\1/p' | sort -u)"
  if [[ -n "$oben" && "$gewaehlt" =~ ^.{8}$oben$ ]]; then ok "Lauf $n: Auswahl bleibt auf Oben"
  else fehler "Lauf $n: Auswahl „$(echo $gewaehlt)“ statt Oben ($oben)"; fi
  # Erste Bildzeile (x = 300, Avatar-Spalte der Liste) in Akzentfarbe ab y = 59 (unter der Kopfleiste).
  zeile="$(python3 -I -c '
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
ys = [y for y in range(59, im.size[1]) if (lambda p: p[0] > 180 and p[1] < 110 and p[2] < 60)(im.getpixel((300, y)))]
print(round((ys[0] - 59) / 101) + 1 if ys else 0)' "$OUT/m4-418-$n.png" 2>&1)"
  if [ "$zeile" = 3 ]; then ok "Lauf $n: Bild: gewählte Zeile an dritter Stelle, Faden und Neu sichtbar"
  else fehler "Lauf $n: Bild: gewählte Zeile an Stelle „$zeile“ statt 3 ($OUT/m4-418-$n.png)"; fi
done

abbruch_bilanz
echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
