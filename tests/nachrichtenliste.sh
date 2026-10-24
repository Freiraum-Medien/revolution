#!/usr/bin/env bash
# Lasttest der Nachrichtenliste (#352) gegen das echte Testkonto KONTO_IMAP_1@example.invalid.
#
# Aufruf: tests/nachrichtenliste.sh [AUSGABEORDNER]   (Standard BUILD/m4-352)
#   BUILD         Build-Ordner, Standard build/ im Repo (vorher meson compile)
#   LAST_ANZAHL   Mindestzahl Mails im Ordner Revolution-Last, Standard 3000
#   LAST_TIMEOUT  Sekunden je App-Start bis zur vollständig geladenen Liste, Standard 300
#   KONVERSATIONEN 1: Liste als Konversationen (#357, conversations=true per .gschema.override; Ordner
#                 mit Gesendet von privat@ gemischt). Die Lastmails haben keine References, jede ist
#                 eine eigene Konversation: Anzahl der Zeilen = Mails im Ordner wie flach.
#
# Vorher: tests/testdaten.py --last LAST_ANZAHL ergänzt fehlende Lastmails (APPEND, idempotent)
# und nennt Konto-UID und Ordner. Dann zwei App-Starts über scripts/screenshot.sh (eigener Xvfb,
# echter Session-Bus, Einstellungen im Speicher), je mit G_MESSAGES_DEBUG=revolution,
# G_DEBUG=fatal-criticals, REVOLUTION_FOLDER (wählt den Ordner) und REVOLUTION_FRAME_LOG (Dauer
# je Frame im Log, src/mail/mail-view.c):
#   oben   Liste geladen, Screenshot m4-352-oben.png
#   unten  Liste geladen, Klick in die erste Zeile, 480× Page_Down im Abstand von 20 ms (wie
#          Tastenwiederholung, bis ans Ende der 3.000 Zeilen) und End, Screenshot m4-352-unten.png
# Daneben fragt ein Prüfer alle 20 ms über den Session-Bus org.gtk.Actions.List bei der App an;
# GApplication beantwortet das im UI-Thread, die Antwortzeit zeigt also Blockaden des UI-Threads.
# Prüfungen je Start: Anzahl geladen ≥ LAST_ANZAHL und = Ordner laut testdaten.py; Log nur
# DEBUG-Zeilen (wie tests/smoke.sh (d)); Beenden Exit 0. Messwerte: Ladezeiten bis erste Zeile und
# bis vollständig; je Abschnitt (Start, Laden, danach = Ruhe bzw. Scrollen) Frames (Anzahl, Median,
# 95 %, Maximum, über 16,7 ms) und längste Antwortzeit des UI-Threads.
# Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“, dazu „WERT …“ für Messwerte; Exit 0 ohne Fehler.
# #353: Die Starts wählen Zeilen (Klick, Bild ab) ohne Gelesen zu prüfen. Daher mark-seen=false
# (eigenes Schema mit .gschema.override, SCHEMA_DIR für scripts/screenshot.sh) und Vergleich der
# ungelesenen UIDs am Server vorher/nachher (tests/testdaten.py --ungelesen); bei Abweichung FEHLER
# und Rückstellung auf den Stand vorher (--soll).
# #411: Jeder Abbruch eines Teilschritts (Traceback, Exit ≠ 0 von testdaten.py oder einer Auswertung,
# Zeitlimit) ist ein FEHLER (tests/abbruch.sh).
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m4-352}"
ANZAHL="${LAST_ANZAHL:-3000}"
TIMEOUT="${LAST_TIMEOUT:-300}"
FEHLER=0

ok () { echo "OK      $*"; }
fehler () { echo "FEHLER  $*"; FEHLER=$((FEHLER + 1)); }
wert () { echo "WERT    $*"; }

# Wie tests/smoke.sh: zwei feste Zeilen Mesa-Rauschen unter Xvfb.
EGL_RAUSCHEN='libEGL warning: DRI3 error: Could not get DRI3 device
libEGL warning: Ensure your X server supports DRI3 to get accelerated rendering'

# #411: LAST_ANZAHL ab 1 (testdaten.py --last 0 lehnt ab, fiel früher in den Standardlauf mit Versand).
[[ "$ANZAHL" =~ ^[1-9][0-9]*$ && "$TIMEOUT" =~ ^[0-9]+$ ]] \
  || { echo "FEHLER  LAST_ANZAHL als Zahl ab 1, LAST_TIMEOUT als Zahl"; exit 2; }
mkdir -p "$OUT"
. "$APP_DIR/tests/abbruch.sh"
rm -f "$OUT"/m4-352-{oben,unten}{.log,.out,.png,.ready,.quit,-ping.txt,-rest.txt} "$OUT/testdaten.txt"

[ -x "$BIN" ] || { fehler "Build: $BIN fehlt (meson compile)"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
[ "$(bus_hat_app)" = "(false,)" ] || { fehler "Session-Bus: $APP_ID läuft schon ($(bus_hat_app))"; exit 1; }

if TD_TIMEOUT=3000 td --last "$ANZAHL" >"$OUT/testdaten.txt" 2>&1; then
  ok "Testdaten: $(grep -v '^LAST ' "$OUT/testdaten.txt" | tail -1 | sed 's/^ *//')"
else
  abbruch_bilanz; echo "Ergebnis: $FEHLER Fehler (siehe $OUT/testdaten.txt)"; exit 1
fi
# #353: Schema mit mark-seen=false, ungelesene UIDs am Server vor den Starts.
SCHEMA="$OUT/schema"
mkdir -p "$SCHEMA"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA/"
printf '[de.freiraum_medien.Revolution]\nmark-seen=false\n%s' "$([ "${KONVERSATIONEN:-0}" = 1 ] && printf 'conversations=true\n')" \
  >"$SCHEMA/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA" || { fehler "Schema: glib-compile-schemas $SCHEMA"; exit 1; }
td --ungelesen KONTO_IMAP_1@example.invalid Revolution-Last >"$OUT/ungelesen-vorher.txt" 2>&1 \
  || { abbruch_bilanz; echo "Ergebnis: $FEHLER Fehler (siehe $OUT/ungelesen-vorher.txt)"; exit 1; }

# „LAST <UID>/<voller Ordnername> <Anzahl>“
ORDNER="$(sed -n 's/^LAST \(.*\) [0-9]*$/\1/p' "$OUT/testdaten.txt")"
SOLL="$(sed -n 's/^LAST .* \([0-9]*\)$/\1/p' "$OUT/testdaten.txt")"
LOGNAME_="${ORDNER/\// }"
[ -n "$ORDNER" ] && [ "${SOLL:-0}" -ge "$ANZAHL" ] \
  || { fehler "Testdaten: Ordner $ORDNER hat ${SOLL:-?} < $ANZAHL Mails"; exit 1; }

debug () { sed -nE 's/^\(revolution:[0-9]+\): revolution-DEBUG: [0-9:.]+: //p' "$1"; }

warte () {
  local log="$1" ready="$2"
  for _ in $(seq "$TIMEOUT"); do
    if [ -f "$log" ] && debug "$log" | grep -qF "Nachrichten: $LOGNAME_ vollständig"; then
      echo fertig >"$ready"; return
    fi
    sleep 1
  done
  echo Zeitlimit >"$ready"
}

# Antwortzeit des UI-Threads: org.gtk.Actions.List am Objektpfad der App, alle 20 ms, bis die App
# den Bus verlässt. Je Anfrage „HH:MM:SS.mmm Antwortzeit-in-ms“ (Uhrzeit wie im Log der App).
pinger () {
  APP_ID="$APP_ID" python3 -I - >"$1" 2>&1 <<'EOF'
import datetime, os, time
from gi.repository import Gio, GLib
app = os.environ['APP_ID']
path = '/' + app.replace('.', '/')
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
def da():
    return bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                         'NameHasOwner', GLib.Variant('(s)', (app,)), None, 0, -1, None)[0]
start = time.monotonic()
while not da() and time.monotonic() - start < 30:
    time.sleep(0.05)
while da():
    jetzt = datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]
    t = time.monotonic()
    try:
        bus.call_sync(app, path, 'org.gtk.Actions', 'List', None, None, 0, 10000, None)
    except GLib.Error:
        break
    print('%s %.1f' % (jetzt, (time.monotonic() - t) * 1000), flush=True)
    time.sleep(0.02)
EOF
}

lauf () {
  local name="$1" click="$2" keys="$3" log="$OUT/$1.log" watcher ping
  warte "$log" "$OUT/$name.ready" & watcher=$!
  pinger "$OUT/$name-ping.txt" & ping=$!
  env G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals REVOLUTION_FOLDER="$ORDNER" REVOLUTION_FRAME_LOG=1 \
    BUILD="$BUILD" SCHEMA_DIR="$SCHEMA" OUT_DIR="$OUT" LOG="$log" READY="$OUT/$name.ready" QUIT="$OUT/$name.quit" \
    WAIT=$((TIMEOUT + 5)) CLICK="$click" AFTER_KEYS="$keys" KEY_DELAY=0.02 \
    timeout -k 10 $((TIMEOUT + 180)) "$APP_DIR/scripts/screenshot.sh" "$name" prefer-light 1200x760 >"$OUT/$name.out" 2>&1 \
    || fehler "$name: scripts/screenshot.sh Exit $? (siehe $OUT/$name.out)"
  kill "$watcher" 2>/dev/null; wait "$watcher" 2>/dev/null
  wait "$ping" 2>/dev/null || fehler "$name: Prüfer der Antwortzeit abgebrochen (siehe $OUT/$name-ping.txt)"

  local n erste voll rc pid
  n="$(debug "$log" | sed -nE "s#^Nachrichten: $LOGNAME_ vollständig ([0-9]+) nach .*#\1#p" | tail -1)"
  if [ -n "$n" ] && [ "$n" -ge "$ANZAHL" ] && [ "$n" = "$SOLL" ]; then
    ok "$name Anzahl: $n Nachrichten geladen = Ordner $ORDNER ($SOLL)"
  else
    fehler "$name Anzahl: App ${n:-?}, Ordner $SOLL, mindestens $ANZAHL"
  fi
  erste="$(debug "$log" | sed -nE "s#^Nachrichten: $LOGNAME_ erste Zeile nach (.*)\$#\1#p" | tail -1)"
  voll="$(debug "$log" | sed -nE "s#^Nachrichten: $LOGNAME_ vollständig [0-9]+ nach (.*)\$#\1#p" | tail -1)"
  wert "$name Ladezeit: erste Zeile nach ${erste:-?}, vollständig nach ${voll:-?}"
  debug "$log" | grep -E "^Nachrichten: $LOGNAME_ (Cache|Server) " | sed "s/^/WERT    $name Durchgang: /"
  debug "$log" | grep -E "^Konversationen: " | sed "s/^/WERT    $name /"

  # Frames (Log „Frame: …“) und Antwortzeiten des UI-Threads je Abschnitt, nach der Uhrzeit im Log:
  # Start (bis „lädt“), Laden (bis „vollständig“), danach (Ruhe bzw. Scrollen).
  NAME="$name" KEY="$LOGNAME_" python3 -I - "$log" "$OUT/$name-ping.txt" <<'EOF'
import os, re, sys
name, key = os.environ['NAME'], os.environ['KEY']
zeile = re.compile(r'^\(revolution:\d+\): revolution-DEBUG: (\d\d:\d\d:\d\d\.\d+): (.*)$')
grenzen, frames = [], []
for l in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = zeile.match(l.rstrip('\n'))
    if not m:
        continue
    t, text = m.groups()
    if text == 'Nachrichten: %s lädt' % key or text.startswith('Nachrichten: %s vollständig' % key):
        grenzen.append(t)
    # Dezimalzeichen nach Gebietsschema der App (de: Komma).
    f = re.match(r'^Frame: ([0-9]+[.,][0-9]+) ms$', text)
    if f:
        frames.append((t, float(f.group(1).replace(',', '.'))))
pings = [(t, float(v)) for t, v in (l.split() for l in open(sys.argv[2]) if len(l.split()) == 2)]
def abschnitt(t):
    return 'Start' if len(grenzen) < 1 or t < grenzen[0] else 'Laden' if len(grenzen) < 2 or t < grenzen[1] else 'danach'
for teil in ('Start', 'Laden', 'danach'):
    v = sorted(d for t, d in frames if abschnitt(t) == teil)
    p = [d for t, d in pings if abschnitt(t) == teil]
    text = 'Frames %d' % len(v)
    if v:
        text += ', Median %.2f ms, 95 %% %.2f ms, Maximum %.2f ms, über 16,7 ms: %d' % (
            v[(len(v) - 1) // 2], v[min(len(v) - 1, int(len(v) * 0.95))], v[-1], sum(d > 16.7 for d in v))
    text += '; UI-Thread %d Anfragen, längste Antwort %s' % (len(p), '%.1f ms' % max(p) if p else '-')
    print('WERT    %s %s: %s' % (name, teil, text))
EOF
  [ "$?" = 0 ] || fehler "$name: Auswertung der Frames abgebrochen"

  grep -vE '^\(revolution:[0-9]+\): revolution-DEBUG: ' "$log" | grep -vxF "$EGL_RAUSCHEN" >"$OUT/$name-rest.txt"
  if [ -s "$OUT/$name-rest.txt" ]; then
    fehler "$name Log: $(wc -l <"$OUT/$name-rest.txt") unerwartete Zeilen (siehe $OUT/$name-rest.txt)"
  else
    ok "$name Log: nur DEBUG-Zeilen"
  fi
  read -r pid rc 2>/dev/null <"$OUT/$name.quit" || rc="keine QUIT-Datei"
  if [ "$rc" = 0 ] && [ "$(bus_hat_app)" = "(false,)" ]; then
    ok "$name Beenden: Exit 0, NameHasOwner = false"
  else
    fehler "$name Beenden: Exit ${rc:-?}, Bus $(bus_hat_app)"
  fi
  if [ -s "$OUT/$name.png" ]; then ok "$name Screenshot: $OUT/$name.png"; else fehler "$name Screenshot fehlt"; fi
}

# Klick auf die erste Zeile der Liste (Fenster 1200×760, Liste ab x≈270, erste Zeile y≈110).
PAGE_DOWN="$(printf 'Page_Down %.0s' $(seq 480))"
lauf m4-352-oben "" ""
lauf m4-352-unten "420,110" "${PAGE_DOWN}End"

# #353: mark-seen=false wirksam (kein Gelesen-Timer im Log)?
if debug "$OUT/m4-352-oben.log" "$OUT/m4-352-unten.log" 2>/dev/null | grep -q '^Gelesen: '; then
  fehler "mark-seen=false unwirksam: „Gelesen: …“ im Log"
else
  ok "mark-seen=false: kein Gelesen-Timer im Log"
fi

# #353: Ausgangszustand am Server (ungelesene UIDs) unverändert?
td --ungelesen KONTO_IMAP_1@example.invalid Revolution-Last >"$OUT/ungelesen-nachher.txt" 2>&1
if [ -s "$OUT/ungelesen-nachher.txt" ] && cmp -s "$OUT/ungelesen-vorher.txt" "$OUT/ungelesen-nachher.txt"; then
  ok "Server: ungelesene Mails in Revolution-Last unverändert ($(cut -d' ' -f2 "$OUT/ungelesen-vorher.txt"))"
else
  fehler "Server: ungelesene Mails geändert (vorher $(cut -d' ' -f2 "$OUT/ungelesen-vorher.txt"), nachher $(cut -d' ' -f2 "$OUT/ungelesen-nachher.txt")); Rückstellung:"
  td --ungelesen KONTO_IMAP_1@example.invalid Revolution-Last --soll "$OUT/ungelesen-vorher.txt" | sed 's/^/        /'
fi

abbruch_bilanz
echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
