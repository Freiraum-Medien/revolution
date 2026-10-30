#!/usr/bin/env bash
# Kalenderquellen und Monatsansicht (#373), Woche und Tag (#374), Termine anlegen/bearbeiten/löschen (#375) gegen die
# echten Kalender aus GOA/EDS.
#
# Aufruf: tests/kalender.sh [AUSGABEORDNER]   (Standard BUILD/m7-375)
#   BUILD    Build-Ordner, Standard build-devel/ im Repo (#439, Profil development) (vorher meson compile)
#   MONAT    Testmonat JJJJ-MM, Standard der nächste Monat (Testtermine nur 2026-10-01 … heute + 90 Tage, Zusatz M7)
#   TEILE    Läufe, Standard „alle woche termine dunkel schmal“ (zum Eingrenzen, z. B. TEILE=dunkel)
#   LAUF_TIMEOUT  Sekunden je Treiber-Lauf, Standard 900; TD_TIMEOUT je tests/testdaten.py, Standard 300
#
# Vergleich unabhängig von der App: tests/testdaten.py --kalender, --kalender-termine (eigener Prozess, Python-GI
# ECal.Client, dieselbe Schicht wie GNOME Calendar; GNOME Calendar selbst öffnete ein Fenster) und --kalender-server
# (Graph GET calendarView bzw. CalDAV REPORT, ohne EDS). App-Starts im eigenen Xvfb (echter Session-Bus, AT-SPI),
# Treiber tests/kalender-treiber.py:
#   alle    hell: Seitenleiste = Registry (14 Kalender: UID, Konto, Name, Farbe, nur lesend; Kopfzeilen je Konto), Tastatur
#           (Strg+2, Bild auf/ab, Strg+T, Alt+Rechts, Pos1, a11y-Namen); Testtermine von außen in den freigegebenen
#           Kalendern (Gmail-CalDAV mit wöchentlicher Wiederholung, outlook.com „Kalender“, „Persönlich“), Monat MONAT je
#           Kalender = Python-GI und = Server; zwei fremde Kalender (nur lesend) Anzahl = Python-GI; live anlegen/ändern/
#           löschen in Gmail und outlook.com ≤ 5 s; Ein/Aus des outlook.com-Kalenders, Neustart (Einstellungen in Datei),
#           bleibt aus, wieder an.
#           #374: „+N“-Popover öffnen/schließen, Monat per Mausrad scrollen.
#   woche   hell (#374): Testwoche (Woche nach heute) im Gmail-Kalender: drei überlappende, ein ganztägiger, ein
#           mehrtägiger Termin; Lage in Woche und Tag per AT-SPI, Tag = Wochenspalte, Heute-Linie, Navigation und
#           Kürzel, live anlegen/ändern/löschen ≤ 5 s in der Woche (Einzelheiten tests/kalender-treiber.py).
#   termine hell (#375): Einfachklick auf leere Fläche legt nicht an, Doppelklick öffnet den Editor (Raster, Monat);
#           Editor (Strg+N, „+“): Kalenderwahl nur schreibbare Kalender, Escape; anlegen lokal ganztägig
#           (Tastatur), Gmail wöchentlich (Maus), outlook.com Zeitfenster (Tastatur) mit Ort, Notiz, Erinnerung; ändern
#           (Titel, Zeit), Wiederholung „nur dieser“/„alle“/„Nachfolgende“, Ziehen in Raster, Kopfleiste und Monat, Kalender
#           im Editor wechseln (outlook.com → lokal); je Schritt zweiter Prozess
#           (tests/testdaten.py --termin-details, Python-GI) und Server ohne EDS (--termin-server); nur lesende Kalender
#           ohne Bearbeiten/Löschen; Banner bei Schreibfehler (Termin von außen gelöscht, Editor offen), „Schließen“; Löschen mit
#           Toast, Rückgängig ohne Schreiben, nach Ablauf weg (Server 404), Fenster schließen löscht sofort; Erinnerung:
#           ein Testtermin (lokal, 5 min vorher) löst über evolution-alarm-notify eine Benachrichtigung aus
#           (dbus-monitor nur lesend, höchstens 1 je Lauf).
#   dunkel  dunkel, Monat MONAT, Woche, Tag, Bilder.   schmal  hell, 900×700 (Seitenleiste überlagert), Bilder.
#   serie   nur mit TEILE=serie (#452, TERMIN_TASK 452): Serie im Gmail-Kalender von außen; geänderte Ausnahme löschen
#           (Entf, Editor): Serie bleibt, Ausnahme und Vorkommen weg, EXDATE (Python-GI, CalDAV); von der Ausnahme aus „Alle
#           Termine“: Serie weg; Kalenderwahl gesperrt bei Vorkommen und Ausnahme (AT-SPI); mehrtägige ganztägige Serie
#           (lokal): nur Titel, „Alle Termine“, Dauer bleibt, Fenster sofort geschlossen (Änderung kommt an, g_application_hold).
# Kalender-UIDs nur in ~/.config/revolution/testkonten.ini [kalender] (nicht im Repo). Schreiben nur dort, nur Testtermine
# „Revolution-Test …“ über die gespeicherte UID; höchstens 20 Schreibvorgänge je Lauf und Kalender über alle Teile
# (#375: Zähler der vorigen Teile als SCHREIB_VORHER an den Treiber, Abbruch vor dem 21.).
# Am Ende: kein Testtermin übrig (--termin-live pruefen), fremde Kalender unverändert (--kalender-hash vorher = nachher),
# ~/.config/evolution/sources unverändert (sha256 je Datei), Einstellungen der App nie in der ESource.
# Zeilen „OK …“/„FEHLER …“, Exit 0 ohne Fehler. #411: Abbruch eines Teilschritts zählt als FEHLER (tests/abbruch.sh).
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "$APP_DIR/tests/testkonten.sh"
BUILD="${BUILD:-$APP_DIR/build-devel}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m7-375}"
LAUF_TIMEOUT="${LAUF_TIMEOUT:-900}"
MONAT="${MONAT:-$(date -d "$(date +%Y-%m-01) +1 month" +%Y-%m)}"
FEHLER=0

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
for tool in xvfb-run gdbus python3 sha256sum; do command -v "$tool" >/dev/null || { echo "FEHLER  $tool fehlt"; exit 1; }; done
python3 -c 'import Xlib, PIL' 2>/dev/null || { echo "FEHLER  python3-xlib/python3-pil fehlt"; exit 1; }
python3 -c 'import gi; gi.require_version("Atspi", "2.0"); gi.require_version("ECal", "2.0")' 2>/dev/null \
  || { echo "FEHLER  gir1.2-atspi-2.0/gir1.2-ecal-2.0 fehlt"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
bus_frei () { for _ in $(seq 30); do [ "$(bus_hat_app)" = "(false,)" ] && return 0; sleep 1; done; [ "$(bus_hat_app)" = "(false,)" ]; }
bus_frei || { echo "FEHLER  Session-Bus: $APP_ID läuft schon"; exit 1; }
mkdir -p "$OUT"
. "$APP_DIR/tests/abbruch.sh"

# Kalender-UIDs aus testkonten.ini (Abschnitt [kalender]).
read -r KAL_M365 KAL_GOOGLE KAL_LOKAL VERGLEICH_LESEND < <(python3 -c '
import sys; sys.path.insert(0, sys.argv[1] + "/tests")
from testkonten import KALENDER as k
print(k["schreibbar_m365"], k["schreibbar_google"], k["schreibbar_lokal"], k["lesend_vergleich"])' "$APP_DIR") \
  || { echo "FEHLER  testkonten.ini: Abschnitt [kalender] fehlt (Vorlage tests/testkonten.ini.example)"; exit 1; }
SCHREIBBAR="$KAL_GOOGLE $KAL_M365 $KAL_LOKAL"

if td --kalender >"$OUT/kalender.txt" 2>&1; then
  echo "OK      Vergleich: $(grep -c '^KALENDER ' "$OUT/kalender.txt") Kalender (tests/testdaten.py --kalender)"
else
  echo "FEHLER  Vergleich: tests/testdaten.py --kalender (siehe $OUT/kalender.txt)"; exit 1
fi
# Fremde Kalender (alle außer den drei freigegebenen): Prüfsumme über (UID, RECURRENCE-ID, LAST-MODIFIED) vorher/nachher.
FREMD="$(awk '$1 == "KALENDER" { print $2 }' "$OUT/kalender.txt" | grep -v -x -F -e "$KAL_M365" -e "$KAL_GOOGLE" -e "$KAL_LOKAL" | tr '\n' ' ')"
td --kalender-hash $FREMD >"$OUT/hash-vorher.txt" 2>&1 || { echo "FEHLER  --kalender-hash vorher"; exit 1; }
SOURCES="$HOME/.config/evolution/sources"
(cd "$SOURCES" && sha256sum -- *) >"$OUT/sources-vorher.txt"
# #375: dieselben Prüfsummen ohne „LastNotified=“: evolution-alarm-notify (EDS) schreibt nach einer Erinnerung die Zeit
# in [Alarms] der ESource des Kalenders (e-reminder-watcher.c:405-434 client_set_last_notification_time), nicht die App.
ohne_last_notified () { (cd "$SOURCES" && for f in *; do printf '%s  %s\n' "$(grep -v '^LastNotified=' -- "$f" | sha256sum | cut -d' ' -f1)" "$f"; done); }
ohne_last_notified >"$OUT/sources-vorher-ohne-lastnotified.txt"

# Mail-Konten laden nicht (der Test betrifft nur den Kalender; wie tests/kontakte.sh).
ALLE_MAILKONTEN="$(python3 -c '
from gi.repository import EDataServer
r = EDataServer.SourceRegistry.new_sync(None)
print(",".join(sorted(s.get_uid() for s in r.list_sources(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT))))')"

lauf () {
  local name="$1" modus="$2" schema="$3" breite="$4" hoehe="$5" rundir confdir rc=0
  bus_frei || { echo "FEHLER  $name: Session-Bus: $APP_ID läuft schon"; return 1; }
  rundir="$(mktemp -d)"
  # Einstellungen in einer Datei im eigenen XDG_CONFIG_HOME: Ein/Aus übersteht den Neustart im Treiber und berührt
  # weder dconf noch die Einstellungen der installierten App (#439).
  confdir="$OUT/$name-config"
  rm -rf "$confdir"
  mkdir -p "$OUT/$name-schema"
  cp "$APP_DIR/data/"*.gschema.xml "$OUT/$name-schema/"
  printf '[de.freiraum_medien.Revolution]\nwindow-width=%s\nwindow-height=%s\n' "$breite" "$hoehe" \
    >"$OUT/$name-schema/90_test.gschema.override"
  glib-compile-schemas --strict "$OUT/$name-schema" || { echo "FEHLER  $name: Schema"; return 1; }
  timeout -k 10 "$LAUF_TIMEOUT" env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK -u M365_DEBUG \
    XDG_RUNTIME_DIR="$rundir" XDG_CONFIG_HOME="$confdir" GSETTINGS_SCHEMA_DIR="$OUT/$name-schema" GSETTINGS_BACKEND=keyfile \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    ADW_DEBUG_COLOR_SCHEME="$schema" G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals \
    REVOLUTION_TEST_HOOKS=1 REVOLUTION_SKIP_ACCOUNTS="$ALLE_MAILKONTEN" \
    APP_ID="$APP_ID" TERMIN_TASK="${TERMIN_TASK:-375}" BIN="$BIN" OUT="$OUT" NAME="m7-375-$name" MODUS="$modus" SCHREIB_VORHER="$(schreib_vorher)" VERGLEICH="$OUT/kalender.txt" MONAT="$MONAT" \
    KALENDER_TERMINE="$SCHREIBBAR $VERGLEICH_LESEND" SCHREIBBAR="$SCHREIBBAR" LIVE_KALENDER="$KAL_GOOGLE $KAL_M365" \
    AUS_KALENDER="$KAL_M365" TERMINE="$OUT" TESTDATEN="$APP_DIR/tests/testdaten.py" \
    xvfb-run -a -s "-screen 0 ${breite}x${hoehe}x24" python3 -I "$APP_DIR/tests/kalender-treiber.py" || rc=$?
  rm -rf "$rundir"
  for _ in $(seq 10); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  return "$rc"
}

# #375: Schreibvorgänge je Kalender in den schon gelaufenen Teilen dieses Laufs („UID=n …“).
FERTIGE_TEILE=""
schreib_vorher () {
  local kal n t
  for kal in $SCHREIBBAR; do
    n=0
    for t in $FERTIGE_TEILE; do n=$((n + $(grep -c "^SCHREIBVORGANG $kal" "$OUT/$t.txt"))); done
    printf '%s=%s ' "$kal" "$n"
  done
}

# Aufräumen über die gespeicherten UIDs; danach Beleg „VORHANDEN 0“ (sonst FEHLER).
aufraeumen () {
  local name="$1" datei kal
  : >"$OUT/$name-aufraeumen.txt"
  for datei in "$OUT"/termin-*.txt; do
    [ -s "$datei" ] || continue
    kal="$(cut -d' ' -f2 "$datei")"
    td --termin-live pruefen "$kal" "$datei" >>"$OUT/$name-aufraeumen.txt" 2>&1
    if [ "$(tail -1 "$OUT/$name-aufraeumen.txt" | cut -d' ' -f1-2)" = "VORHANDEN 1" ]; then
      td --termin-live loeschen "$kal" "$datei" >>"$OUT/$name-aufraeumen.txt" 2>&1
      td --termin-live pruefen "$kal" "$datei" >>"$OUT/$name-aufraeumen.txt" 2>&1
    fi
    if [ "$(tail -1 "$OUT/$name-aufraeumen.txt" | cut -d' ' -f1-2)" = "VORHANDEN 0" ]; then
      echo "OK      $name: Testtermin $(cut -d' ' -f3 "$datei" | cut -c1-40) nicht mehr in ${kal:0:8}"
      rm -f "$datei"
    else
      echo "FEHLER  $name: Testtermin $(cut -d' ' -f3 "$datei") noch in ${kal:0:8}"; FEHLER=$((FEHLER + 1))
    fi
  done
  grep '^SCHREIBVORGANG' "$OUT/$name-aufraeumen.txt" >>"$OUT/$name.txt"
  local n t
  FERTIGE_TEILE="$FERTIGE_TEILE $name"
  for kal in $SCHREIBBAR; do
    n=0
    for t in $FERTIGE_TEILE; do n=$((n + $(grep -c "^SCHREIBVORGANG $kal" "$OUT/$t.txt"))); done
    if [ "$n" -le 20 ]; then echo "OK      $name: $n Schreibvorgänge in ${kal:0:8} im Lauf bisher (≤ 20)"
    else echo "FEHLER  $name: $n Schreibvorgänge in ${kal:0:8} im Lauf bisher (> 20)"; FEHLER=$((FEHLER + 1)); fi
  done
}

teil () {
  local name="$1"; shift
  rm -f "$OUT"/termin-*.txt
  lauf "$name" "$@" | tee "$OUT/$name.txt"
  teilschritt "$name" "${PIPESTATUS[0]}" "$OUT/$name.txt"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))
  aufraeumen "$name"
}

TEILE="${TEILE:-alle woche termine dunkel schmal}"

# #375 Runde 3: Benachrichtigungen über den ganzen Lauf mitschneiden (nur lesend, dbus-monitor), am Ende nur die Blöcke mit
# „Revolution-Test“ behalten und zählen: genau 1 Testerinnerung je Lauf (Teil termine), sonst keine.
NOTIFY_ROH="$(mktemp)"
dbus-monitor --session "interface='org.freedesktop.Notifications',member='Notify'" \
  "interface='org.gtk.Notifications',member='AddNotification'" >"$NOTIFY_ROH" 2>&1 &
NOTIFY_PID=$!
for t in $TEILE; do
  case "$t" in
    alle) teil alle alle prefer-light 1400 900 ;;
    woche) teil woche woche prefer-light 1400 900 ;;
    termine) teil termine termine prefer-light 1400 900 ;;
    serie) TERMIN_TASK=452 teil serie serie prefer-light 1400 900 ;;
    dunkel) teil dunkel bild prefer-dark 1400 900 ;;
    schmal) teil schmal bild prefer-light 900 700 ;;
    *) echo "FEHLER  TEILE: $t unbekannt"; FEHLER=$((FEHLER + 1)) ;;
  esac
done

td --kalender-hash $FREMD >"$OUT/hash-nachher.txt" 2>&1
if diff -q "$OUT/hash-vorher.txt" "$OUT/hash-nachher.txt" >/dev/null; then
  echo "OK      fremde Kalender unverändert: $(grep -c '^KALENDERHASH' "$OUT/hash-vorher.txt") Prüfsummen (UID, LAST-MODIFIED) wie vorher"
else
  echo "FEHLER  fremde Kalender geändert: $(diff "$OUT/hash-vorher.txt" "$OUT/hash-nachher.txt" | tr '\n' ' ')"
  FEHLER=$((FEHLER + 1))
fi
(cd "$SOURCES" && sha256sum -- *) >"$OUT/sources-nachher.txt"
ohne_last_notified >"$OUT/sources-nachher-ohne-lastnotified.txt"
if diff -q "$OUT/sources-vorher.txt" "$OUT/sources-nachher.txt" >/dev/null; then
  echo "OK      ~/.config/evolution/sources unverändert ($(wc -l <"$OUT/sources-vorher.txt") Dateien, sha256)"
elif diff -q "$OUT/sources-vorher-ohne-lastnotified.txt" "$OUT/sources-nachher-ohne-lastnotified.txt" >/dev/null; then
  echo "OK      ~/.config/evolution/sources unverändert bis auf [Alarms] LastNotified (evolution-alarm-notify nach der" \
       "Testerinnerung): $(diff "$OUT/sources-vorher.txt" "$OUT/sources-nachher.txt" | grep '^>' | awk '{print $3}' | tr '\n' ' ')"
else
  echo "FEHLER  ~/.config/evolution/sources geändert: $(diff "$OUT/sources-vorher.txt" "$OUT/sources-nachher.txt" | tr '\n' ' ')"
  FEHLER=$((FEHLER + 1))
fi

kill "$NOTIFY_PID" 2>/dev/null; wait "$NOTIFY_PID" 2>/dev/null
N_NOTIFY="$(python3 - "$NOTIFY_ROH" "$OUT/notify-gesamt.txt" <<'PY'
import re, sys
bloecke = re.split(r'\n(?=method call |signal |method return |error )', open(sys.argv[1], errors='replace').read())
test = [b for b in bloecke if 'Revolution-Test' in b and re.search(r'member=(Notify|AddNotification)', b)]
open(sys.argv[2], 'w').write('\n'.join(test) + '\n')
print(len(test))
PY
)"
rm -f "$NOTIFY_ROH"
SOLL_NOTIFY=0
case " $TEILE " in *" termine "*) SOLL_NOTIFY=1 ;; esac
if [ "$N_NOTIFY" = "$SOLL_NOTIFY" ]; then
  echo "OK      Benachrichtigungen mit „Revolution-Test“ im ganzen Lauf: $N_NOTIFY (soll $SOLL_NOTIFY, $OUT/notify-gesamt.txt)"
else
  echo "FEHLER  Benachrichtigungen mit „Revolution-Test“ im ganzen Lauf: $N_NOTIFY (soll $SOLL_NOTIFY)"; FEHLER=$((FEHLER + 1))
fi

abbruch_bilanz

echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
