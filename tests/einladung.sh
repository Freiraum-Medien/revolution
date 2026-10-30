#!/usr/bin/env bash
# Einladungen aus Mails anzeigen, zusagen, absagen (#376) gegen die echten Konten (nur KONTO_M365 ↔ KONTO_GOOGLE,
# Briefing Zusatz M7).
#
# Aufruf: flock App/build/.run.lock tests/einladung.sh [AUSGABEORDNER]   (Standard BUILD/m7-376)
#   BUILD    Build-Ordner, Standard build-devel/ (vorher meson compile)
#   LAUF_TIMEOUT  Sekunden je App-Lauf, Standard 600; TD_TIMEOUT je tests/testdaten.py, Standard 700 (Warten auf Mails)
#
# Ablauf (Einladungen am Server des Organisators, tests/testdaten.py --einladung, Titel „Revolution-Test 376 <Lauf> X“):
#   A  outlook.com lädt Gmail ein (Graph POST /me/events). Revolution (Gmail-Konto) zeigt die Karte, Kalender
#      „Persönlich“ in der Kalenderwahl gewählt (Tastatur), Annehmen per Zugriffstaste (Alt+A): Termin lokal mit PARTSTAT
#      ACCEPTED (Python-GI ECal.Client), Antwort über den Postausgang des Gmail-Kontos, Mail beim Organisator, Status
#      „accepted“ im Termin des Organisators (Graph GET, Gegenprobe am Server). Bilder hell.
#      Nicht Gmail-CalDAV (PM 30.10.2026): Google übernimmt die Einladung aus dem Posteingang selbst (erst versteckt,
#      später sichtbar mit NEEDS-ACTION); ein PUT mit ihrer UID scheitert mit HTTP 400 (#447, siehe G). Googles Kopie
#      fasst der Test nie an (Aufräumen meldet sie als UEBRIG).
#   B  wie A, Ablehnen per Klick, dunkel: kein Termin in „Persönlich“ (EDS legt abgelehnte nicht an,
#      e-cal-meta-backend.c:2626-2646), Antwort per Mail, Status „declined“.
#   G  wie A, aber Kalender Gmail-CalDAV: Schreiben scheitert (HTTP 400), Banner per AT-SPI, keine Antwort im Postausgang,
#      Status beim Organisator unverändert; Bild banner.
#   C  Gmail lädt outlook.com ein (CalDAV PUT, Google verschickt). Revolution (outlook.com) Kalender „Kalender“
#      (Exchange hat den Termin schon vorläufig eingetragen), Annehmen: Exchange antwortet selbst (Graph-Antwort,
#      keine Mail von Revolution), Status ACCEPTED im Gmail-Termin (CalDAV REPORT), Antwort-Mail in Gmail.
#   D  wie C, Ablehnen.
#   E  CANCEL: outlook.com sagt A ab (Graph DELETE), Revolution (Gmail) „Aus Kalender entfernen“: Termin aus „Persönlich“ weg.
#   F  REPLY (vor E): Revolution (Gmail, Organisator von C und D) öffnet die Antworten von outlook.com, „Teilnehmerstatus
#      aktualisieren“: PARTSTAT ACCEPTED bzw. DECLINED im Gmail-Termin (Gmail übernimmt sie nicht selbst, Spam).
# Grenzen: Schreibvorgänge je Kalender ≤ 20 (gezählt „SCHREIBVORGANG“), Mails über outlook.com ≤ 10 („MAIL-OUTLOOK“).
# Am Ende: Termine und Mails des Laufs über die gemerkten IDs gelöscht (--einladung aufraeumen), fremde Kalender
# unverändert (--kalender-hash), ~/.config/evolution/sources unverändert. Zeilen „OK …“/„FEHLER …“, Exit 0 ohne Fehler.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "$APP_DIR/tests/testkonten.sh"
BUILD="${BUILD:-$APP_DIR/build-devel}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m7-376}"
LAUF_TIMEOUT="${LAUF_TIMEOUT:-600}"
export TD_TIMEOUT="${TD_TIMEOUT:-700}"
FEHLER=0

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
for tool in xvfb-run gdbus python3 sha256sum; do command -v "$tool" >/dev/null || { echo "FEHLER  $tool fehlt"; exit 1; }; done
python3 -c 'import gi; gi.require_version("Atspi", "2.0"); gi.require_version("ECal", "2.0"); import Xlib, PIL' 2>/dev/null \
  || { echo "FEHLER  gir1.2-atspi-2.0/gir1.2-ecal-2.0/python3-xlib/python3-pil fehlt"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
bus_frei () { for _ in $(seq 30); do [ "$(bus_hat_app)" = "(false,)" ] && return 0; sleep 1; done; [ "$(bus_hat_app)" = "(false,)" ]; }
mkdir -p "$OUT"
. "$APP_DIR/tests/abbruch.sh"

# Kalender-UIDs aus testkonten.ini, Konten-UIDs und Anzeigenamen aus der Registry (nicht im Repo).
read -r KAL_M365 KAL_GOOGLE KAL_LOKAL UID_M365 UID_GOOGLE < <(python3 -c '
import sys; sys.path.insert(0, sys.argv[1] + "/tests")
import gi; gi.require_version("EDataServer", "1.2")
from gi.repository import EDataServer
import testdaten as t
r = EDataServer.SourceRegistry.new_sync(None)
k = t.konten_finden(r)
print(*t.kalender_schreibbar(), k[t.KONTO_M365].account.get_uid(), k[t.KONTO_GOOGLE].account.get_uid())' "$APP_DIR") \
  || { echo "FEHLER  testkonten.ini/Registry: Kalender oder Konten fehlen"; exit 1; }
name_von () { python3 -c '
import sys, gi; gi.require_version("EDataServer", "1.2")
from gi.repository import EDataServer
print(EDataServer.SourceRegistry.new_sync(None).ref_source(sys.argv[1]).get_display_name())' "$1"; }
NAME_GOOGLE="$(name_von "$KAL_GOOGLE")"
NAME_M365="$(name_von "$KAL_M365")"
NAME_LOKAL="$(name_von "$KAL_LOKAL")"
ALLE_MAILKONTEN="$(python3 -c '
import gi; gi.require_version("EDataServer", "1.2")
from gi.repository import EDataServer
r = EDataServer.SourceRegistry.new_sync(None)
print(",".join(sorted(s.get_uid() for s in r.list_sources(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT))))')"
ohne () { echo "$ALLE_MAILKONTEN" | tr ',' '\n' | grep -v -x -F "$1" | paste -sd, -; }

# Fremde Kalender und Quellen vorher (wie tests/kalender.sh).
td --kalender >"$OUT/kalender.txt" 2>&1 || { echo "FEHLER  tests/testdaten.py --kalender"; exit 1; }
FREMD="$(awk '$1 == "KALENDER" { print $2 }' "$OUT/kalender.txt" | grep -v -x -F -e "$KAL_M365" -e "$KAL_GOOGLE" -e "$KAL_LOKAL" | tr '\n' ' ')"
td --kalender-hash $FREMD >"$OUT/hash-vorher.txt" 2>&1 || { echo "FEHLER  --kalender-hash vorher"; exit 1; }
SOURCES="$HOME/.config/evolution/sources"
(cd "$SOURCES" && sha256sum -- *) >"$OUT/sources-vorher.txt"

PROTOKOLL="$OUT/protokoll.txt"
: >"$PROTOKOLL"
tdp () { td "$@" 2>&1 | tee -a "$PROTOKOLL"; return "${PIPESTATUS[0]}"; }

# lauf NAME KONTO-UID ORDNER BETREFF ANTWORT METHODE KALENDER KALENDERNAME MAIL SCHEMA [TASTE [FEHLER_ERWARTET]]
lauf () {
  local name="$1" konto="$2" ordner="$3" betreff="$4" antwort="$5" methode="$6" kal="$7" kalname="$8" mail="$9"
  local schema="${10}" taste="${11:-0}" fehler_erwartet="${12:-0}" rundir rc=0
  bus_frei || { echo "FEHLER  $name: Session-Bus: $APP_ID läuft schon"; return 1; }
  rundir="$(mktemp -d)"
  mkdir -p "$OUT/$name-schema"
  cp "$APP_DIR/data/"*.gschema.xml "$OUT/$name-schema/"
  # Flach (wie tests/anhaenge.sh), damit REVOLUTION_MESSAGE die Mail als Zeile wählt.
  printf '[de.freiraum_medien.Revolution]\nconversations=false\nwindow-width=1200\nwindow-height=820\n' \
    >"$OUT/$name-schema/90_test.gschema.override"
  glib-compile-schemas --strict "$OUT/$name-schema" || { echo "FEHLER  $name: Schema"; return 1; }
  timeout -k 10 "$LAUF_TIMEOUT" env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK -u M365_DEBUG \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$OUT/$name-schema" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    ADW_DEBUG_COLOR_SCHEME="$schema" G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals \
    REVOLUTION_TEST_HOOKS=1 REVOLUTION_SKIP_ACCOUNTS="$(ohne "$konto")" \
    REVOLUTION_FOLDER="$konto/$ordner" REVOLUTION_MESSAGE="$betreff" \
    BIN="$BIN" OUT="$OUT" NAME="$name" ANTWORT="$antwort" METHODE="$methode" TITEL="$TITEL_JETZT" \
    KALENDER="$kal" KALENDER_NAME="$kalname" MAIL="$mail" TASTE="$taste" FEHLER_ERWARTET="$fehler_erwartet" \
    xvfb-run -a -s "-screen 0 1200x820x24" python3 -I "$APP_DIR/tests/einladung-treiber.py" || rc=$?
  rm -rf "$rundir"
  for _ in $(seq 10); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  return "$rc"
}

teil () {
  local name="$1"; shift
  lauf "$name" "$@" | tee "$OUT/$name.txt"
  teilschritt "$name" "${PIPESTATUS[0]}" "$OUT/$name.txt"
  grep '^SCHREIBVORGANG' "$OUT/$name.txt" >>"$PROTOKOLL"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))
}

# soll DATEI KALENDER ORGANISATOR TEILNEHMER-SERVER TEILNEHMER-EDS  (Muster für grep -E, z. B. „ACCEPTED“, „0 -“)
soll () {
  local fall="$1" kal="$2" org="$3" server="$4" eds="$5" datei="$OUT/pruefen-$1.txt" n
  for n in 1 2 3 4 5 6; do
    td --einladung pruefen "$OUT/einladung-$fall.json" "$kal" >"$datei" 2>&1
    grep -q -E "^ORGANISATOR $org\$" "$datei" && grep -q -E "^TEILNEHMER-SERVER $server\$" "$datei" && break
    sleep 30
  done
  sed "s/^/        /" "$datei"
  for z in "ORGANISATOR $org" "TEILNEHMER-SERVER $server" "TEILNEHMER-EDS $eds"; do
    if grep -q -E "^$z\$" "$datei"; then echo "OK      $fall: $(grep -E "^$z\$" "$datei") (Organisator am Server, Teilnehmer am Server ohne EDS, Python-GI ECal)"
    else echo "FEHLER  $fall: erwartet „$z“, ist: $(grep "^${z%% *}" "$datei")"; FEHLER=$((FEHLER + 1)); fi
  done
}

LAUF="$(date +%m%d%H%M)"
TAG="$(date -d '+12 days' +%Y-%m-%d)"
titel () { echo "Revolution-Test 376 $LAUF $1"; }
anlegen () {  # FALL ORGANISATOR TEILNEHMER STUNDE
  tdp --einladung anlegen "$2" "$3" "$OUT/einladung-$1.json" "$(titel "$1")" "${TAG}T$4:00" >/dev/null \
    || { echo "FEHLER  $1: Einladung nicht angelegt"; FEHLER=$((FEHLER + 1)); return 1; }
  echo "OK      $1: Einladung „$(titel "$1")“ von ${2%%@*}… angelegt"
}
warten () {  # ART FALL
  local zeile
  zeile="$(tdp --einladung warten "$1" "$OUT/einladung-$2.json" | grep '^MAIL ')"
  if [ -n "$zeile" ]; then echo "OK      $2: Mail $1 angekommen ($(echo "$zeile" | cut -d' ' -f4))"; BETREFF="$(echo "$zeile" | cut -d' ' -f5-)"
  else echo "FEHLER  $2: Mail $1 nicht angekommen"; FEHLER=$((FEHLER + 1)); BETREFF=""; return 1; fi
}

for fall in A B C D G; do rm -f "$OUT/einladung-$fall.json"; done
anlegen A "$KONTO_M365" "$KONTO_GOOGLE" 09
anlegen B "$KONTO_M365" "$KONTO_GOOGLE" 10
anlegen C "$KONTO_GOOGLE" "$KONTO_M365" 11
anlegen D "$KONTO_GOOGLE" "$KONTO_M365" 12
anlegen G "$KONTO_M365" "$KONTO_GOOGLE" 14

# A, B: Gmail nimmt an bzw. lehnt ab (Kalender Gmail-CalDAV, Antwort über den Postausgang).
if warten einladung A; then TITEL_JETZT="$(titel A)"
  teil m7-376-a "$UID_GOOGLE" INBOX "$BETREFF" annehmen REQUEST "$KAL_LOKAL" "$NAME_LOKAL" ja prefer-light 1; fi
if warten einladung B; then TITEL_JETZT="$(titel B)"
  teil m7-376-b "$UID_GOOGLE" INBOX "$BETREFF" ablehnen REQUEST "$KAL_LOKAL" "$NAME_LOKAL" ja prefer-dark; fi
# G (#376 R2): Annehmen im Gmail-Kalender scheitert an Googles versteckter Kopie (HTTP 400, #447): Banner, keine Mail.
if warten einladung G; then TITEL_JETZT="$(titel G)"
  teil m7-376-g "$UID_GOOGLE" INBOX "$BETREFF" annehmen REQUEST "$KAL_GOOGLE" "$NAME_GOOGLE" nein prefer-light 0 1; fi
# C, D: outlook.com nimmt an bzw. lehnt ab (Kalender „Kalender“, Exchange antwortet).
if warten einladung C; then TITEL_JETZT="$(titel C)"
  teil m7-376-c "$UID_M365" Posteingang "$BETREFF" annehmen REQUEST "$KAL_M365" "$NAME_M365" nein prefer-light; fi
if warten einladung D; then TITEL_JETZT="$(titel D)"
  teil m7-376-d "$UID_M365" Posteingang "$BETREFF" ablehnen REQUEST "$KAL_M365" "$NAME_M365" nein prefer-light; fi

# Antworten beim Organisator: Mail und PARTSTAT im Termin des Organisators. outlook.com verarbeitet die Antwort
# selbst; Gmail legt die Antworten von Exchange in den Spam und übernimmt sie nicht (gemessen 30.10., Lauf 0150):
# dort übernimmt Revolution sie (F, G: REPLY, „Teilnehmerstatus aktualisieren“).
# Teilnehmer-Server (Gmail-CalDAV) bei A/B nur als Wert: Google übernimmt die Einladung selbst (versteckt bzw.
# NEEDS-ACTION), das ist Googles Kopie, nicht die der App (PM 30.10.: Gegenprobe am Server = Graph GET beim Organisator).
warten antwort A && soll A "$KAL_LOKAL" ACCEPTED "(0 -|1 .*)" "1 ACCEPTED STATUS .*"
warten antwort B && soll B "$KAL_LOKAL" DECLINED "(0 -|1 .*)" "0 -"
warten antwort C && BETREFF_C="$BETREFF"
warten antwort D && BETREFF_D="$BETREFF"
for fall in C D; do
  betreff="BETREFF_$fall"
  [ -n "${!betreff:-}" ] || continue
  TITEL_JETZT="$(titel $fall)"
  teil "m7-376-${fall,,}-antwort" "$UID_GOOGLE" INBOX "${!betreff}" status REPLY "$KAL_GOOGLE" "$NAME_GOOGLE" nein prefer-light
done
soll C "$KAL_M365" ACCEPTED "1 ACCEPTED" "1 .* STATUS CONFIRMED"
# D: Google schickt nach der Absage eine „Aktualisierte Einladung“ (Lauf 0207), Exchange legt daraufhin wieder eine
# Kopie ohne Antwort an: beim Teilnehmer daher auch NEEDS-ACTION zulässig, entscheidend ist der Organisator.
soll D "$KAL_M365" DECLINED "(0 -|1 DECLINED|1 NEEDS-ACTION)" "(0 -|1 .*)"

# E: Absage von outlook.com an Gmail, Revolution entfernt den Termin aus Gmail.
if tdp --einladung absagen "$OUT/einladung-A.json" >/dev/null && warten absage A; then TITEL_JETZT="$(titel A)"
  teil m7-376-e "$UID_GOOGLE" INBOX "$BETREFF" entfernen CANCEL "$KAL_LOKAL" "$NAME_LOKAL" nein prefer-light
  td --einladung pruefen "$OUT/einladung-A.json" "$KAL_LOKAL" >"$OUT/pruefen-E.txt" 2>&1
  if grep -q '^TEILNEHMER-SERVER 0 -$' "$OUT/pruefen-E.txt" && grep -q '^TEILNEHMER-EDS 0 -$' "$OUT/pruefen-E.txt"; then
    echo "OK      E: Termin nach „Aus Kalender entfernen“ weg aus „Persönlich“ (Python-GI ECal 0)"
  else echo "FEHLER  E: Termin noch da: $(tr '\n' ' ' <"$OUT/pruefen-E.txt")"; FEHLER=$((FEHLER + 1)); fi
fi

# Aufräumen über die gemerkten IDs.
# G: keine Antwort beim Organisator (Graph GET, Status unverändert).
[ -f "$OUT/einladung-G.json" ] && soll G "$KAL_GOOGLE" NEEDS-ACTION "(0 -|1 .*)" "(0 -|1 .*)"

for fall in A B C D G; do
  [ -f "$OUT/einladung-$fall.json" ] || continue
  tdp --einladung gesendet "$OUT/einladung-$fall.json" >/dev/null
  # Absagen nach dem Löschen des Organisator-Termins (Exchange und Google schicken sie) ebenfalls merken und löschen.
  if tdp --einladung aufraeumen "$OUT/einladung-$fall.json" >"$OUT/aufraeumen-$fall.txt" &&
     { [ "$fall" = A ] || WARTEN=300 tdp --einladung warten absage "$OUT/einladung-$fall.json" >/dev/null; } &&
     tdp --einladung aufraeumen "$OUT/einladung-$fall.json" >>"$OUT/aufraeumen-$fall.txt" &&
     tdp --einladung rest "$OUT/einladung-$fall.json" >/dev/null &&
     tdp --einladung aufraeumen "$OUT/einladung-$fall.json" >>"$OUT/aufraeumen-$fall.txt"; then
    echo "OK      $fall: aufgeräumt ($(grep -c '^AUFGERAEUMT' "$OUT/aufraeumen-$fall.txt") Mails, $(grep -c '^SCHREIBVORGANG' "$OUT/aufraeumen-$fall.txt") Termine)"
    grep '^UEBRIG' "$OUT/aufraeumen-$fall.txt" | sort -u | sed "s/^/WERT    $fall: /"
  else echo "FEHLER  $fall: Aufräumen"; FEHLER=$((FEHLER + 1)); fi
done

# Grenzen je Lauf.
for kal in "$KAL_M365" "$KAL_GOOGLE" "$KAL_LOKAL"; do
  n="$(grep -c "^SCHREIBVORGANG $kal" "$PROTOKOLL")"
  if [ "$n" -le 20 ]; then echo "OK      $n Schreibvorgänge in ${kal:0:8} (≤ 20)"
  else echo "FEHLER  $n Schreibvorgänge in ${kal:0:8} (> 20)"; FEHLER=$((FEHLER + 1)); fi
done
n="$(grep -c '^MAIL-OUTLOOK' "$PROTOKOLL")"
# Exchange antwortet in C und D selbst (je eine Mail über outlook.com).
n=$((n + 2))
if [ "$n" -le 10 ]; then echo "OK      $n Mails über outlook.com (Einladungen, Absagen, 2 Antworten von Exchange; ≤ 10)"
else echo "FEHLER  $n Mails über outlook.com (> 10)"; FEHLER=$((FEHLER + 1)); fi

td --kalender-hash $FREMD >"$OUT/hash-nachher.txt" 2>&1
if diff -q "$OUT/hash-vorher.txt" "$OUT/hash-nachher.txt" >/dev/null; then
  echo "OK      fremde Kalender unverändert: $(grep -c '^KALENDERHASH' "$OUT/hash-vorher.txt") Prüfsummen wie vorher"
else
  echo "FEHLER  fremde Kalender geändert: $(diff "$OUT/hash-vorher.txt" "$OUT/hash-nachher.txt" | tr '\n' ' ')"
  FEHLER=$((FEHLER + 1))
fi
(cd "$SOURCES" && sha256sum -- *) >"$OUT/sources-nachher.txt"
if diff -q "$OUT/sources-vorher.txt" "$OUT/sources-nachher.txt" >/dev/null; then
  echo "OK      ~/.config/evolution/sources unverändert ($(wc -l <"$OUT/sources-vorher.txt") Dateien, sha256)"
else
  echo "FEHLER  ~/.config/evolution/sources geändert: $(diff "$OUT/sources-vorher.txt" "$OUT/sources-nachher.txt" | tr '\n' ' ')"
  FEHLER=$((FEHLER + 1))
fi

abbruch_bilanz
echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
