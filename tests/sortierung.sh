#!/usr/bin/env bash
# Konversationsliste und Lesebereich bei Live-Änderungen (#418, #419), echtes Testkonto.
#
# Aufruf: tests/sortierung.sh [AUSGABEORDNER]   (Standard BUILD/m4-418)
#   BUILD     Build-Ordner, Standard build/ im Repo (vorher meson compile)
#   KONTO     Testkonto (IMAP, nicht privat@), Standard KONTO_IMAP_2@example.invalid
#   LAEUFE    Durchgänge, Standard 3
#   TEILE     Standard „oben lesen nicht-oben“
#   TIMEOUT   Sekunden je App-Start bis „vollständig“, Standard 300
#   NACHLAUF  Sekunden nach der Live-Änderung bis zur Aufnahme, Standard 8 (Teil lesen: 25, Gelesen-Timer)
#   OHNE      Konten-UIDs, die die App nicht lädt (REVOLUTION_SKIP_ACCOUNTS, #415), Standard das nur lesend
#             freigegebene NUR_LESEND_3@example.invalid
#
# Je Teil und Durchgang legt tests/testdaten.py --sortierung KONTO basis „Pruef418 Faden“ (ungelesen, Datum
# 20.10.2026, eine Nachricht) und „Pruef418 Oben“ (Datum jetzt) in den Posteingang; danach kommen alle
# Pruef418-Mails in den Papierkorb (UID MOVE). Die App läuft in der Konversationsansicht. Sobald das Log
# „<Konto> INBOX vollständig“ zeigt (Cache- und Server-Durchgang fertig), legt --sortierung KONTO neu bei
# laufender App eine Antwort auf den Faden (Datum jetzt) und eine neue Konversation „Pruef418 Neu“ (eine
# Sekunde davor) ab; beide kommen als Live-Änderung (IDLE) in die Liste („Änderung (Konversationen) N neu“).
#   oben (#418)   erster App-Start lädt Faden und Oben in den Cache (~/.cache/revolution wie die übrigen
#                 Tests); zweiter Start mit Cache, gewählt „Pruef418 Oben“ (oberste Zeile; REVOLUTION_MESSAGE,
#                 mark-seen=false) wie im Beleg aus #412. Geprüft (Log src/mail/mail-view.c, Bild):
#                 „… erste Zeile nach … (Cache)“; jede Zeile „Reihenfolge: … 0 nicht nach Datum“, die letzte
#                 nennt oben Faden, Neu, Oben; gewählt bleibt Oben; im Bild m4-418-N.png steht die gewählte
#                 Zeile (Akzentfarbe) an dritter Stelle, Faden und Neu darüber sind sichtbar (Zeilenhöhe 101 px
#                 bei 1200x760)
#   lesen (#419)  gewählt „Pruef418 Faden“ (eine Nachricht: Lesebereich zeigt die Einzelnachricht), mark-seen
#                 an mit mark-seen-timeout 15000 ms (der Gelesen-Timer der Zeile läuft noch, wenn die Antwort
#                 kommt). Geprüft im Log: „Konversation: <Faden> jetzt mit 2 Nachrichten“, danach „… mit 2
#                 Nachrichten angezeigt“, die erste Nachricht darin (oben) ist die Antwort; gewählt bleibt der
#                 Faden; die Antwort wird erst gelesen (#353: „Flags: … <Antwort> gelesen (automatisch)“) nach
#                 „Konversation: <Antwort> aufgeklappt“ und „Gelesen: … <Antwort> in …“. Bild m4-419-lesen-N.png
#   nicht-oben (#419)  Treiber tests/sortierung-treiber.py: Klick in die vierte Zeile, fünfmal Pfeil ab (die
#                 neunte Zeile gewählt, Liste nicht oben), dann die Live-Änderung (Neu und Faden kommen über
#                 die sichtbaren Zeilen): kein Sprung, die gewählte Zeile bleibt an ihrer Stelle und gewählt
#                 (Bilder m4-419-nicht-oben-N-vorher.png, m4-419-nicht-oben-N.png)
#   Ende          jeder Start: Beenden Exit 0, App nicht mehr auf dem Bus
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
TEILE="${TEILE:-oben lesen nicht-oben}"
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
LESEN="$OUT/schema-lesen"
mkdir -p "$LESEN"
cp "$APP_DIR/data/"*.gschema.xml "$LESEN/"
printf '[de.freiraum_medien.Revolution]\nconversations=true\nmark-seen=true\nmark-seen-timeout=15000\n' \
  >"$LESEN/90_test.gschema.override"
glib-compile-schemas --strict "$LESEN" || { echo "FEHLER  Schema: glib-compile-schemas $LESEN"; exit 1; }

# App-Start über scripts/screenshot.sh (Schema SCHEMA_START, Standard SCHEMA). Aufnahme NACHLAUF Sekunden nach
# „<Konto> INBOX vollständig“; mit NEU (Datei) legt der Wächter dann erst die beiden Mails am Server ab
# (--sortierung KONTO neu) und wartet auf die Live-Änderung, die sie in die Liste bringt („Änderung
# (Konversationen) N neu“, N ≥ 1).
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
  timeout -k 10 "$((2 * TIMEOUT + NACHLAUF + 120))" env BUILD="$BUILD" OUT_DIR="$OUT" SCHEMA_DIR="${SCHEMA_START:-$SCHEMA}" \
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

basis () {
  td --sortierung "$KONTO" basis >"$OUT/$1.txt" 2>&1
  KONTO_UID="$(sed -n 's/^ORDNER \([^/]*\)\/INBOX$/\1/p' "$OUT/$1.txt")"
  [ -n "$KONTO_UID" ] || fehler "Testdaten basis ($OUT/$1.txt)"
  [ -n "$KONTO_UID" ]
}

# IMAP-UIDs aus der Ausgabe von --sortierung neu: Faden, Antwort, Neu, Oben.
uids () {
  faden="$(sed -n 's/^SORTIERUNG Faden \([0-9]*\) .*/\1/p' "$1" | tail -1)"
  antwort="$(sed -n 's/^SORTIERUNG Antwort \([0-9]*\) .*/\1/p' "$1" | tail -1)"
  neu="$(sed -n 's/^SORTIERUNG Neu \([0-9]*\) .*/\1/p' "$1" | tail -1)"
  oben="$(sed -n 's/^SORTIERUNG Oben \([0-9]*\) .*/\1/p' "$1" | tail -1)"
}

# Erste Zeilennummer im Log ab Zeile $2, die auf $3 passt (0, wenn keine).
zeile_ab () { awk -v ab="$2" -v rx="$3" 'NR > ab && $0 ~ rx { print NR; f = 1; exit } END { if (!f) print 0 }' "$1"; }

teil_oben () {
  local n="$1" log="$OUT/m4-418-$1.log" falsch letzte gewaehlt zeile
  basis "basis-$n" || return
  start "m4-418-$n-cache"
  start "m4-418-$n" "Pruef418 Oben" "$OUT/neu-$n.txt"
  td --sortierung "$KONTO" papierkorb >"$OUT/papierkorb-$n.txt" 2>&1
  grep '^SORTIERUNG' "$OUT/neu-$n.txt" | sed 's/^/WERT    oben '"$n"': /'
  uids "$OUT/neu-$n.txt"

  if grep -q "INBOX erste Zeile nach .* (Cache)" "$log"; then ok "oben $n: Start mit Cache"; else fehler "oben $n: kein Cache-Durchgang"; fi
  wert "oben $n: $(grep -c 'Reihenfolge:' "$log") Stände der Liste, letzter: $(grep 'Reihenfolge:' "$log" | tail -1 | sed 's/.*Reihenfolge: //')"
  falsch="$(grep 'Reihenfolge:' "$log" | grep -vc ' 0 nicht nach Datum')"
  [ "$(grep -c 'Reihenfolge:' "$log")" -gt 0 ] && [ "$falsch" = 0 ] \
    && ok "oben $n: jeder Stand nach neuester Nachricht absteigend" || fehler "oben $n: $falsch Stände nicht nach Datum"
  letzte="$(grep 'Reihenfolge:' "$log" | tail -1)"
  # Konversations-UIDs im Suchordner: 8 Zeichen Präfix + UID im Quellordner (EDS camel-vee-message-info.c:574).
  if [[ -n "$faden" && -n "$neu" && -n "$oben" && "$letzte" =~ oben\ .{8}$faden\ .{8}$neu\ .{8}$oben, ]]; then
    ok "oben $n: oben Faden ($faden), Neu ($neu), Oben ($oben)"
  else fehler "oben $n: oben nicht Faden ($faden), Neu ($neu), Oben ($oben): $letzte"; fi
  gewaehlt="$(grep 'Reihenfolge:' "$log" | sed -n 's/.* gewählt \(.*\)$/\1/p' | sort -u)"
  if [[ -n "$oben" && "$gewaehlt" =~ ^.{8}$oben$ ]]; then ok "oben $n: Auswahl bleibt auf Oben"
  else fehler "oben $n: Auswahl „$(echo $gewaehlt)“ statt Oben ($oben)"; fi
  # Erste Bildzeile (x = 300, Avatar-Spalte der Liste) in Akzentfarbe ab y = 59 (unter der Kopfleiste).
  zeile="$(python3 -I -c '
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
ys = [y for y in range(59, im.size[1]) if (lambda p: p[0] > 180 and p[1] < 110 and p[2] < 60)(im.getpixel((300, y)))]
print(round((ys[0] - 59) / 101) + 1 if ys else 0)' "$OUT/m4-418-$n.png" 2>&1)"
  if [ "$zeile" = 3 ]; then ok "oben $n: Bild: gewählte Zeile an dritter Stelle, Faden und Neu sichtbar"
  else fehler "oben $n: Bild: gewählte Zeile an Stelle „$zeile“ statt 3 ($OUT/m4-418-$n.png)"; fi
}

teil_lesen () {
  local n="$1" name="m4-419-lesen-$1" log="$OUT/m4-419-lesen-$1.log" wechsel gezeigt erste offen timer gelesen gewaehlt
  basis "basis-lesen-$n" || return
  SCHEMA_START="$LESEN" NACHLAUF="${NACHLAUF_LESEN:-25}" start "$name" "Pruef418 Faden" "$OUT/neu-lesen-$n.txt"
  td --sortierung "$KONTO" papierkorb >"$OUT/papierkorb-lesen-$n.txt" 2>&1
  grep '^SORTIERUNG' "$OUT/neu-lesen-$n.txt" | sed 's/^/WERT    lesen '"$n"': /'
  uids "$OUT/neu-lesen-$n.txt"
  [ -n "$faden" ] && [ -n "$antwort" ] || { fehler "lesen $n: UIDs Faden/Antwort fehlen ($OUT/neu-lesen-$n.txt)"; return; }

  wechsel="$(zeile_ab "$log" 0 "Konversation: .{8}$faden jetzt mit 2 Nachrichten")"
  gezeigt="$(zeile_ab "$log" "$wechsel" "Konversation: .{8}$faden mit 2 Nachrichten angezeigt")"
  erste="$(awk -v ab="$wechsel" 'NR > ab && /Konversation: Nachricht / { print $(NF - 3); exit }' "$log")"
  if [ "$wechsel" -gt 0 ] && [ "$gezeigt" -gt 0 ]; then ok "lesen $n: Lesebereich wechselt zur Konversation mit 2 Nachrichten (Log-Zeilen $wechsel, $gezeigt)"
  else fehler "lesen $n: kein Wechsel zur Konversation (jetzt mit 2: Zeile $wechsel, angezeigt: Zeile $gezeigt)"; fi
  if [[ "$erste" =~ ^.{8}$antwort$ ]]; then ok "lesen $n: neue Nachricht oben ($erste)"
  else fehler "lesen $n: oben „$erste“ statt Antwort ($antwort)"; fi
  # Ab der Auswahl (REVOLUTION_MESSAGE wählt den Faden, sobald er in der Liste steht).
  gewaehlt="$(grep 'Reihenfolge:' "$log" | sed -n 's/.* gewählt \(.*\)$/\1/p' | awk '$0 != "-" { f = 1 } f' | sort -u)"
  if [[ "$gewaehlt" =~ ^.{8}$faden$ ]]; then ok "lesen $n: Auswahl bleibt auf dem Faden"
  else fehler "lesen $n: Auswahl „$(echo $gewaehlt)“ statt Faden ($faden)"; fi
  offen="$(zeile_ab "$log" 0 "Konversation: .{8}$antwort aufgeklappt")"
  timer="$(zeile_ab "$log" 0 "Gelesen: .* .{8}$antwort in [0-9]+ ms")"
  gelesen="$(zeile_ab "$log" 0 "Flags: .* .{8}$antwort gelesen")"
  wert "lesen $n: Antwort aufgeklappt Zeile $offen, Gelesen-Timer Zeile $timer, gelesen Zeile $gelesen"
  if [ "$offen" -gt 0 ] && [ "$timer" -gt "$offen" ] && { [ "$gelesen" = 0 ] || [ "$gelesen" -gt "$timer" ]; }; then
    ok "lesen $n: Antwort erst nach Anzeige gelesen (#353)"
  else fehler "lesen $n: Antwort gelesen ohne Anzeige bzw. vor ihrem Timer ($log)"; fi
}

teil_nicht_oben () {
  local n="$1" name="m4-419-nicht-oben-$1" rc=0 rundir gewaehlt
  basis "basis-nicht-oben-$n" || return
  rundir="$(mktemp -d)"
  timeout -k 10 "$((2 * TIMEOUT + 180))" env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$SCHEMA" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    GTK_A11Y=none NO_AT_BRIDGE=1 ADW_DEBUG_COLOR_SCHEME=prefer-light \
    G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals REVOLUTION_FOLDER="$KONTO_UID/INBOX" \
    REVOLUTION_TEST_HOOKS=1 REVOLUTION_SKIP_ACCOUNTS="$OHNE" \
    BIN="$BIN" OUT="$OUT" NAME="$name" KONTO="$KONTO" KONTO_UID="$KONTO_UID" APP_DIR="$APP_DIR" \
    NEU="$OUT/neu-nicht-oben-$n.txt" TIMEOUT="$TIMEOUT" \
    xvfb-run -a -s "-screen 0 1200x760x24" python3 -I "$APP_DIR/tests/sortierung-treiber.py" >"$OUT/$name.txt" 2>&1 || rc=$?
  rm -rf "$rundir"
  td --sortierung "$KONTO" papierkorb >"$OUT/papierkorb-nicht-oben-$n.txt" 2>&1
  gewaehlt="$(grep 'Reihenfolge:' "$OUT/$name.log" | sed -n 's/.* gewählt \(.*\)$/\1/p' | awk '$0 != "-" { f = 1 } f' | sort -u)"
  if [ -n "$gewaehlt" ] && [ "$(echo "$gewaehlt" | wc -l)" = 1 ] && [ "$gewaehlt" != - ]; then
    echo "OK      $name: Auswahl bleibt ($gewaehlt)" >>"$OUT/$name.txt"
  else echo "FEHLER  $name: Auswahl „$(echo $gewaehlt)“ nicht gleich geblieben" >>"$OUT/$name.txt"; fi
  cat "$OUT/$name.txt"
  teilschritt "$name" "$rc" "$OUT/$name.txt"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))
  [ "$(bus_hat_app)" = "(false,)" ] || fehler "$name: App nach dem Lauf noch auf dem Bus"
}

for n in $(seq "$LAEUFE"); do
  for teil in $TEILE; do
    case "$teil" in
      oben) teil_oben "$n" ;;
      lesen) teil_lesen "$n" ;;
      nicht-oben) teil_nicht_oben "$n" ;;
      *) fehler "unbekannter Teil $teil" ;;
    esac
  done
done

abbruch_bilanz
echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
