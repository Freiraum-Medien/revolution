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
# #377 (TEILE=377; Standard TEILE=376, je Teil ein eigener Lauf wegen ≤ 10 Mails über outlook.com): eigene Einladungen aus dem Editor (tests/kalender-treiber.py MODUS=einladung)
#   M  Termin in „Kalender“ (outlook.com) mit Gmail als Teilnehmer: Exchange lädt ein (Server plant, keine Mail der
#      App), Gmail nimmt über die #376-Karte an (Kalender „Persönlich“), Status „Angenommen“ beim Organisator (Graph GET,
#      Python-GI ECal, Editor mit Bild hell/dunkel), Ändern (Beginn 10:00, Graph = Editor) → Aktualisierung kommt an,
#      Löschen → Absage (Backend: Graph cancel) kommt an.
#      TEILE=377m bzw. 377l: nur M bzw. nur L.
#   L  Termin in „Persönlich“ (ohne eigene Adresse), Organisator-Wahl Gmail, Teilnehmer outlook.com: Revolution
#      verschickt REQUEST, Aktualisierung und CANCEL selbst über den Postausgang des Gmail-Kontos; Ankunft und
#      Kopie bei outlook.com (Graph, Beginn, abgesagt).
# #449 (TEILE=449, eigener Lauf): Aktualisierung nur bei Änderungen für Teilnehmer, Absage an entfernte Teilnehmer
#   P  wie L (Persönlich, Organisator Gmail, Teilnehmer outlook.com und seine Plus-Adresse): nur Erinnerung → keine
#      Mail, SEQUENCE gleich (Python-GI), bei outlook.com nichts Neues; Titel → genau 1 Aktualisierung; Plus-Adresse
#      entfernen → CANCEL nur an sie, REQUEST an den Verbliebenen (Postausgang; Graph, MIME-Empfänger).
#   TEILE=449g: Fall G ohne outlook.com (Ausnahme PM 30.10.2026): Einladung per SMTP vom IMAP-Testkonto an Gmail,
#      Annehmen im Gmail-Kalender, HTTP 400 → Bannertext. Kommt kein 400: Termin über die gemerkte UID löschen.
#   Q  wie M (Kalender outlook.com, Teilnehmer Gmail und seine Plus-Adresse): Plus-Adresse entfernen → App sendet
#      nichts („Server plant“), Absage und Aktualisierung von Exchange bei Gmail (IMAP). Den letzten Teilnehmer
#      entfernen scheitert am Backend (Graph „attendees: null“, evolution-ews e-cal-backend-m365-utils.c:1254-1255).
# #452 (TEILE=452, eigener Lauf, nichts über outlook.com): Serie in „Persönlich“, Organisator Gmail, Teilnehmer IMAP-Testkonto
#   S  Löschen „Nachfolgende“ am dritten Vorkommen: CANCEL mit RECURRENCE-ID;RANGE=THISANDFUTURE; Ändern „Nachfolgende“ am
#      zweiten: REQUEST ebenso, EDS teilt die Serie. Beleg: gesendete und empfangene Mail per IMAP (--einladung serie).
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
TEILE="${TEILE:-376}"
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

for fall in A B C D G M L P Q; do rm -f "$OUT/einladung-$fall.json"; done
if [[ " $TEILE " == *" 376 "* ]]; then
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

# G: keine Antwort beim Organisator (Graph GET, Status unverändert).
[ -f "$OUT/einladung-G.json" ] && soll G "$KAL_GOOGLE" NEEDS-ACTION "(0 -|1 .*)" "(0 -|1 .*)"
fi

# #377: App-Lauf mit dem Kalender-Treiber (wie tests/kalender.sh lauf).
MONAT="$(date -d "$(date +%Y-%m-01) +1 month" +%Y-%m)"
kal_lauf () {  # NAME SKIP-KONTEN SCHEMA, weitere Umgebung über env
  local name="$1" skip="$2" schema="$3" rundir confdir rc=0
  bus_frei || { echo "FEHLER  $name: Session-Bus: $APP_ID läuft schon"; return 1; }
  rundir="$(mktemp -d)"
  confdir="$OUT/$name-config"
  rm -rf "$confdir"
  mkdir -p "$OUT/$name-schema"
  cp "$APP_DIR/data/"*.gschema.xml "$OUT/$name-schema/"
  printf '[de.freiraum_medien.Revolution]\nwindow-width=1200\nwindow-height=820\n' >"$OUT/$name-schema/90_test.gschema.override"
  glib-compile-schemas --strict "$OUT/$name-schema" || { echo "FEHLER  $name: Schema"; return 1; }
  timeout -k 10 "$LAUF_TIMEOUT" env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK -u M365_DEBUG \
    XDG_RUNTIME_DIR="$rundir" XDG_CONFIG_HOME="$confdir" GSETTINGS_SCHEMA_DIR="$OUT/$name-schema" GSETTINGS_BACKEND=keyfile \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    ADW_DEBUG_COLOR_SCHEME="$schema" G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals \
    REVOLUTION_TEST_HOOKS=1 REVOLUTION_SKIP_ACCOUNTS="$skip" \
    APP_ID="$APP_ID" BIN="$BIN" OUT="$OUT" NAME="$name" MODUS=einladung VERGLEICH="$OUT/kalender.txt" MONAT="$MONAT" \
    SCHREIBBAR="$KAL_GOOGLE $KAL_M365 $KAL_LOKAL" TERMINE="$OUT" TESTDATEN="$APP_DIR/tests/testdaten.py" \
    xvfb-run -a -s "-screen 0 1200x820x24" python3 -I "$APP_DIR/tests/kalender-treiber.py" || rc=$?
  rm -rf "$rundir"
  for _ in $(seq 10); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  return "$rc"
}
kal_teil () {  # NAME SKIP SCHEMA (Umgebung SCHRITT KAL KAL_NAME TEILNEHMER ART TITEL TAG …)
  local name="$1"; shift
  kal_lauf "$name" "$@" | tee "$OUT/$name.txt"
  teilschritt "$name" "${PIPESTATUS[0]}" "$OUT/$name.txt"
  grep '^SCHREIBVORGANG' "$OUT/$name.txt" | sed "s/(App:/(App $name:/" >>"$PROTOKOLL"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))
}
pruefe () {  # TEXT DATEI MUSTER
  if grep -q -E "$3" "$2"; then echo "OK      $1: $(grep -E "$3" "$2" | head -1)"
  else echo "FEHLER  $1: erwartet „$3“, ist: $(tr '\n' ' ' <"$2")"; FEHLER=$((FEHLER + 1)); fi
}

if [[ " $TEILE " == *" 377"* ]]; then
  TAG7="$(date -d '+13 days' +%Y-%m-%d)"
  titel7 () { echo "Revolution-Test 377 $LAUF $1"; }
  # Anzeigetext der Gmail-Identität in der Organisator-Wahl („Name <Adresse>“, Registry, nicht im Repo).
  ORG_GOOGLE="$(python3 -c '
import sys, gi; gi.require_version("EDataServer", "1.2")
from gi.repository import EDataServer
r = EDataServer.SourceRegistry.new_sync(None)
for s in r.list_enabled(EDataServer.SOURCE_EXTENSION_MAIL_IDENTITY):
    e = s.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_IDENTITY)
    if (e.get_address() or "").lower() == sys.argv[1]:
        print("%s <%s>" % (e.get_name(), e.get_address()) if e.get_name() else e.get_address()); break' "$KONTO_GOOGLE")"

  # M: Organisator outlook.com (microsoft365, Server plant).
  # Grenze vor dem Versand (PM R2): M verschickt über outlook.com 3 Mails (Einladung, Aktualisierung, Absage).
  schon="$(grep -c '^MAIL-OUTLOOK' "$PROTOKOLL")"
  [[ " $TEILE " == *" 376 "* ]] && schon=$((schon + 2))
  if [[ " $TEILE " != *" 377l "* ]] && [ $((schon + 3)) -gt 10 ]; then
    echo "FEHLER  M: übersprungen, $schon + 3 Mails über outlook.com > 10 (TEILE=377 als eigenen Lauf)"; FEHLER=$((FEHLER + 1))
  elif [[ " $TEILE " != *" 377l "* ]]; then
  export SCHRITT=anlegen KAL="$KAL_M365" KAL_NAME="$NAME_M365" TEILNEHMER="$KONTO_GOOGLE" ART=m TITEL="$(titel7 M)" TAG="$TAG7"
  kal_teil m7-377-m-anlegen "$ALLE_MAILKONTEN" prefer-light
  if [ -s "$OUT/eigene-m.uid" ] && tdp --einladung eigene "$OUT/einladung-M.json" "$KAL_M365" "$(cat "$OUT/eigene-m.uid")" \
       "$KONTO_M365" "$KONTO_GOOGLE" "$TITEL" >/dev/null; then
    echo "MAIL-OUTLOOK 1 Einladung M (Exchange)" >>"$PROTOKOLL"
    pruefe "M: App schickt keine eigene Mail (Server plant)" "$OUT/m7-377-m-anlegen.log" "Einladung: REQUEST Server plant"
    if warten einladung M; then
      TITEL_JETZT="$TITEL"
      teil m7-377-m-annehmen "$UID_GOOGLE" INBOX "$BETREFF" annehmen REQUEST "$KAL_LOKAL" "$NAME_LOKAL" ja prefer-light
      warten antwort M && soll M "$KAL_LOKAL" ACCEPTED "(0 -|1 .*)" "1 ACCEPTED STATUS .*"
      td --einladung eds "$OUT/einladung-M.json" >"$OUT/eds-M.txt" 2>&1
      pruefe "M: Status beim Organisator, zweiter EDS-Leser (Python-GI ECal)" "$OUT/eds-M.txt" "^ORGANISATOR-EDS ACCEPTED$"
    fi
    export SCHRITT=aendern STATUS_SOLL=Angenommen
    kal_teil m7-377-m-aendern "$ALLE_MAILKONTEN" prefer-light
    unset STATUS_SOLL
    echo "MAIL-OUTLOOK 1 Aktualisierung M (Exchange)" >>"$PROTOKOLL"
    warten aktualisierung M
    export SCHRITT=loeschen
    kal_teil m7-377-m-loeschen "$ALLE_MAILKONTEN" prefer-light
    echo "MAIL-OUTLOOK 1 Absage M (Exchange)" >>"$PROTOKOLL"
    pruefe "M: Absage über das Backend (Graph cancel), keine Mail der App" "$OUT/m7-377-m-loeschen.log" "Einladung: CANCEL vom Server verschickt"
    if grep -q '^OK      Löschen nach der Absage' "$OUT/m7-377-m-loeschen.txt"; then
      python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); d["abgesagt"]=True; json.dump(d,open(sys.argv[1],"w"),indent=1)' "$OUT/einladung-M.json"
    fi
    warten absage M
  else echo "FEHLER  M: Termin nicht angelegt/gemerkt"; FEHLER=$((FEHLER + 1)); fi
  fi

  if [[ " $TEILE " != *" 377m "* ]]; then
  # L: Organisator Gmail-Identität in „Persönlich“ (kein Server-Scheduling): Mails der App über den Postausgang.
  export SCHRITT=anlegen KAL="$KAL_LOKAL" KAL_NAME="$NAME_LOKAL" TEILNEHMER="$KONTO_M365" ART=l TITEL="$(titel7 L)" TAG="$TAG7" ORGANISATOR="$ORG_GOOGLE"
  kal_teil m7-377-l-anlegen "$(ohne "$UID_GOOGLE")" prefer-light
  unset ORGANISATOR
  if [ -s "$OUT/eigene-l.uid" ] && tdp --einladung eigene "$OUT/einladung-L.json" "$KAL_LOKAL" "$(cat "$OUT/eigene-l.uid")" \
       "$KONTO_GOOGLE" "$KONTO_M365" "$TITEL" >/dev/null; then
    pruefe "L: REQUEST über den Postausgang" "$OUT/m7-377-l-anlegen.log" "Einladung: REQUEST an 1 Teilnehmer über den Postausgang"
    warten einladung L
    sleep 30
    td --einladung kopie "$OUT/einladung-L.json" >"$OUT/kopie-L1.txt" 2>&1
    # Exchange trägt die Einladung der App bisher nicht in den Kalender ein (Messung 30.10., Folgepunkt): nur Wert.
    grep -E "^(KOPIE|MAILTYP|MIME)" "$OUT/kopie-L1.txt" | sed "s/^/WERT    L: /"
    export SCHRITT=aendern
    kal_teil m7-377-l-aendern "$(ohne "$UID_GOOGLE")" prefer-light
    pruefe "L: Aktualisierung über den Postausgang" "$OUT/m7-377-l-aendern.log" "Einladung: REQUEST an 1 Teilnehmer über den Postausgang"
    warten aktualisierung L
    sleep 30
    td --einladung kopie "$OUT/einladung-L.json" >"$OUT/kopie-L2.txt" 2>&1
    grep -E "^(KOPIE|MAILTYP|MIME)" "$OUT/kopie-L2.txt" | sed "s/^/WERT    L: /"
    # Kopie bei outlook.com vor der Absage über die gemerkte UID löschen (PM R3), sonst benennt Exchange sie um.
    tdp --einladung kopie-loeschen "$OUT/einladung-L.json" >/dev/null
    # Rückgängig beim Löschen: keine Absage (PM Runde 2).
    export SCHRITT=rueckgaengig
    kal_teil m7-377-l-rueckgaengig "$(ohne "$UID_GOOGLE")" prefer-light
    export SCHRITT=loeschen
    kal_teil m7-377-l-loeschen "$(ohne "$UID_GOOGLE")" prefer-light
    pruefe "L: CANCEL über den Postausgang" "$OUT/m7-377-l-loeschen.log" "Einladung: CANCEL an 1 Teilnehmer über den Postausgang"
    warten absage L
    sleep 30
    td --einladung kopie "$OUT/einladung-L.json" >"$OUT/kopie-L3.txt" 2>&1
    pruefe "L: Absage bei outlook.com mit Betreff „Abgesagt: …“ (Graph)" "$OUT/kopie-L3.txt" "^MAILTYP inbox .* „Abgesagt: "
    grep -E "^(KOPIE|MIME)" "$OUT/kopie-L3.txt" | sed "s/^/WERT    L: /"
  else echo "FEHLER  L: Termin nicht angelegt/gemerkt"; FEHLER=$((FEHLER + 1)); fi
  fi
  unset SCHRITT KAL KAL_NAME TEILNEHMER ART TITEL TAG
fi

if [[ " $TEILE " == *" 449 "* ]]; then
  TAG9="$(date -d '+14 days' +%Y-%m-%d)"
  titel9 () { echo "Revolution-Test 449 $LAUF $1"; }
  ORG_GOOGLE="$(python3 -c '
import sys, gi; gi.require_version("EDataServer", "1.2")
from gi.repository import EDataServer
r = EDataServer.SourceRegistry.new_sync(None)
for s in r.list_enabled(EDataServer.SOURCE_EXTENSION_MAIL_IDENTITY):
    e = s.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_IDENTITY)
    if (e.get_address() or "").lower() == sys.argv[1]:
        print("%s <%s>" % (e.get_name(), e.get_address()) if e.get_name() else e.get_address()); break' "$KONTO_GOOGLE")"
  # Erwartete Fehlanzeige (Exit 1) ohne Abbruch-Zählung von td.
  ohne_neues () { WARTEN=120 timeout -k 10 300 python3 "$APP_DIR/tests/testdaten.py" --einladung warten "$1" "$2" >"$3" 2>&1; true; }

  # P: Organisator Gmail-Identität in „Persönlich“, Teilnehmer outlook.com und seine Plus-Adresse (HILFS_TOM): Mails der
  # App über den Postausgang des Gmail-Kontos.
  export SCHRITT=anlegen KAL="$KAL_LOKAL" KAL_NAME="$NAME_LOKAL" TEILNEHMER="$KONTO_M365" TEILNEHMER2="$HILFS_TOM" ART=p \
    TITEL="$(titel9 P)" TAG="$TAG9" ORGANISATOR="$ORG_GOOGLE"
  kal_teil m7-449-p-anlegen "$(ohne "$UID_GOOGLE")" prefer-light
  unset ORGANISATOR
  if [ -s "$OUT/eigene-p.uid" ] && tdp --einladung eigene "$OUT/einladung-P.json" "$KAL_LOKAL" "$(cat "$OUT/eigene-p.uid")" \
       "$KONTO_GOOGLE" "$KONTO_M365" "$TITEL" >/dev/null; then
    warten einladung P
    export SCHRITT=erinnerung
    kal_teil m7-449-p-erinnerung "$(ohne "$UID_GOOGLE")" prefer-light
    ohne_neues aktualisierung "$OUT/einladung-P.json" "$OUT/warten-P-erinnerung.txt"
    pruefe "P: nur Erinnerung, bei outlook.com nichts Neues in 120 s (Graph)" "$OUT/warten-P-erinnerung.txt" "^MAIL-FEHLT "
    export SCHRITT=titel TITEL_NEU="$TITEL neu"
    kal_teil m7-449-p-titel "$(ohne "$UID_GOOGLE")" prefer-light
    warten aktualisierung P
    # Kopie bei outlook.com vor der Absage über die gemerkte UID löschen (wie L), sonst benennt Exchange sie um.
    tdp --einladung kopie-loeschen "$OUT/einladung-P.json" >/dev/null
    export SCHRITT=entfernen TITEL="$TITEL_NEU" TEILNEHMER_WEG="$HILFS_TOM" CANCEL_SOLL="CANCEL an 1 Teilnehmer über den Postausgang"
    kal_teil m7-449-p-entfernen "$(ohne "$UID_GOOGLE")" prefer-light
    warten abgesagt P
    warten aktualisiert P
    sleep 30
    td --einladung kopie "$OUT/einladung-P.json" >"$OUT/kopie-P.txt" 2>&1
    pruefe "P: Absage bei outlook.com „Abgesagt: …“ (Graph)" "$OUT/kopie-P.txt" "^MAILTYP inbox .* „Abgesagt: "
    pruefe "P: Absage nur an den Entfernten (Plus-Adresse), METHOD:CANCEL (MIME, Graph \$value)" "$OUT/kopie-P.txt" "^AN \S+ plus METHOD CANCEL$"
    pruefe "P: Aktualisierung nur an den Verbliebenen, METHOD:REQUEST" "$OUT/kopie-P.txt" "^AN \S+ haupt METHOD REQUEST$"
  else echo "FEHLER  P: Termin nicht angelegt/gemerkt"; FEHLER=$((FEHLER + 1)); fi
  unset TITEL_NEU CANCEL_SOLL TEILNEHMER_WEG

  # Q: Organisator outlook.com (microsoft365, Server plant), Teilnehmer Gmail und seine Plus-Adresse (HILFS_LENA);
  # Mails über outlook.com (Exchange): Einladung 1, nach dem Entfernen Aktualisierung + Absage 2.
  export SCHRITT=anlegen KAL="$KAL_M365" KAL_NAME="$NAME_M365" TEILNEHMER="$KONTO_GOOGLE" TEILNEHMER2="$HILFS_LENA" ART=q \
    TITEL="$(titel9 Q)" TAG="$TAG9"
  kal_teil m7-449-q-anlegen "$ALLE_MAILKONTEN" prefer-light
  if [ -s "$OUT/eigene-q.uid" ] && tdp --einladung eigene "$OUT/einladung-Q.json" "$KAL_M365" "$(cat "$OUT/eigene-q.uid")" \
       "$KONTO_M365" "$KONTO_GOOGLE" "$TITEL" >/dev/null; then
    echo "MAIL-OUTLOOK 1 Einladung Q (Exchange)" >>"$PROTOKOLL"
    warten einladung Q
    export SCHRITT=entfernen TEILNEHMER_WEG="$HILFS_LENA" CANCEL_SOLL="CANCEL an 1 entfernte Teilnehmer Server plant" SEQUENZ=0
    kal_teil m7-449-q-entfernen "$ALLE_MAILKONTEN" prefer-light
    echo "MAIL-OUTLOOK 1 Aktualisierung Q (Exchange)" >>"$PROTOKOLL"
    echo "MAIL-OUTLOOK 1 Absage Q (Exchange)" >>"$PROTOKOLL"
    warten abgesagt Q
    warten aktualisiert Q
  else echo "FEHLER  Q: Termin nicht angelegt/gemerkt"; FEHLER=$((FEHLER + 1)); fi
  unset SCHRITT KAL KAL_NAME TEILNEHMER TEILNEHMER2 TEILNEHMER_WEG ART TITEL TAG CANCEL_SOLL SEQUENZ
fi

if [[ " $TEILE " == *" 449g "* ]]; then
  if tdp --einladung privat "$OUT/einladung-G.json" "Revolution-Test 449 $LAUF G" "$(date -d '+14 days' +%Y-%m-%d)T14:00" >/dev/null; then
    if warten einladung G; then TITEL_JETZT="Revolution-Test 449 $LAUF G"
      teil m7-449-g "$UID_GOOGLE" INBOX "$BETREFF" annehmen REQUEST "$KAL_GOOGLE" "$NAME_GOOGLE" nein prefer-light 0 1
      grep -q 'Schreibfehler e-soup-session-error-quark 400, Google ja' "$OUT/m7-449-g.log" \
        || tdp --einladung privat-loeschen "$OUT/einladung-G.json" >/dev/null
    fi
  else echo "FEHLER  G: Einladung nicht gesendet"; FEHLER=$((FEHLER + 1)); fi
fi

if [[ " $TEILE " == *" 452 "* ]]; then
  ORG_GOOGLE="$(python3 -c '
import sys, gi; gi.require_version("EDataServer", "1.2")
from gi.repository import EDataServer
r = EDataServer.SourceRegistry.new_sync(None)
for s in r.list_enabled(EDataServer.SOURCE_EXTENSION_MAIL_IDENTITY):
    e = s.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_IDENTITY)
    if (e.get_address() or "").lower() == sys.argv[1]:
        print("%s <%s>" % (e.get_name(), e.get_address()) if e.get_name() else e.get_address()); break' "$KONTO_GOOGLE")"
  TITEL_S="Revolution-Test 452 $LAUF S"
  TAG_S="$(date -d '+14 days' +%Y-%m-%d)"
  export SCHRITT=anlegen KAL="$KAL_LOKAL" KAL_NAME="$NAME_LOKAL" TEILNEHMER="$KONTO_IMAP_1" ART=s TITEL="$TITEL_S" TAG="$TAG_S" \
    ORGANISATOR="$ORG_GOOGLE" WOECHENTLICH=1
  kal_teil m7-452-anlegen "$(ohne "$UID_GOOGLE")" prefer-light
  unset ORGANISATOR WOECHENTLICH
  if [ -s "$OUT/eigene-s.uid" ]; then
    export SCHRITT=folgende-loeschen TAG="$(date -d "$TAG_S +14 days" +%Y-%m-%d)"
    kal_teil m7-452-loeschen "$(ohne "$UID_GOOGLE")" prefer-light
    export SCHRITT=folgende-aendern TAG="$(date -d "$TAG_S +7 days" +%Y-%m-%d)" TITEL_NEU="$TITEL_S neu"
    kal_teil m7-452-aendern "$(ohne "$UID_GOOGLE")" prefer-light
    for _ in 1 2 3 4 5 6 7 8 9; do
      td --einladung serie "$OUT/serie-mails.json" "$TITEL_S" >"$OUT/serie-mails.txt" 2>&1
      [ "$(grep -c '^SERIE-MAIL eingang' "$OUT/serie-mails.txt")" -ge 3 ] && break
      sleep 20
    done
    pruefe "S: CANCEL „Nachfolgende“ gesendet" "$OUT/serie-mails.txt" '^SERIE-MAIL gesendet METHOD CANCEL RECURRENCE-ID .*RANGE=THISANDFUTURE'
    pruefe "S: CANCEL „Nachfolgende“ angekommen" "$OUT/serie-mails.txt" '^SERIE-MAIL eingang METHOD CANCEL RECURRENCE-ID .*RANGE=THISANDFUTURE'
    pruefe "S: REQUEST „Nachfolgende“ gesendet" "$OUT/serie-mails.txt" '^SERIE-MAIL gesendet METHOD REQUEST RECURRENCE-ID .*RANGE=THISANDFUTURE'
    pruefe "S: REQUEST „Nachfolgende“ angekommen" "$OUT/serie-mails.txt" '^SERIE-MAIL eingang METHOD REQUEST RECURRENCE-ID .*RANGE=THISANDFUTURE'
    pruefe "S: Einladung der Serie ohne RECURRENCE-ID" "$OUT/serie-mails.txt" '^SERIE-MAIL eingang METHOD REQUEST RECURRENCE-ID -$'
    tdp --einladung serie "$OUT/serie-mails.json" >"$OUT/serie-aufraeumen.txt"
    for f in s s-neu; do
      [ -s "$OUT/eigene-$f.uid" ] || continue
      echo "TERMIN $KAL_LOKAL $(cat "$OUT/eigene-$f.uid") $TITEL_S" >"$OUT/termin-$f.txt"
      tdp --termin-live loeschen "$KAL_LOKAL" "$OUT/termin-$f.txt" >/dev/null
      td --termin-live pruefen "$KAL_LOKAL" "$OUT/termin-$f.txt" | grep -q '^VORHANDEN 0' \
        && echo "OK      S: Termin $f gelöscht" || { echo "FEHLER  S: Termin $f noch da"; FEHLER=$((FEHLER + 1)); }
    done
  else echo "FEHLER  S: Termin nicht angelegt"; FEHLER=$((FEHLER + 1)); fi
  unset SCHRITT KAL KAL_NAME TEILNEHMER ART TITEL TAG TITEL_NEU
fi

# Aufräumen über die gemerkten IDs.
for fall in A B C D G M L P Q; do
  [ -f "$OUT/einladung-$fall.json" ] || continue
  tdp --einladung gesendet "$OUT/einladung-$fall.json" >/dev/null
  # Absagen nach dem Löschen des Organisator-Termins (Exchange und Google schicken sie) ebenfalls merken und löschen.
  if tdp --einladung aufraeumen "$OUT/einladung-$fall.json" >"$OUT/aufraeumen-$fall.txt" &&
     { [ "$fall" = A ] || [ "$fall" = M ] || [ "$fall" = L ] || [ "$fall" = P ] || [ "$fall" = Q ] || [[ " $TEILE " == *" 449g "* ]] || WARTEN=300 tdp --einladung warten absage "$OUT/einladung-$fall.json" >/dev/null; } &&
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
[[ " $TEILE " == *" 376 "* ]] && n=$((n + 2))
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
