#!/usr/bin/env bash
# Online/Offline nach Netzstatus (#408) gegen die echten Konten, Netz zur Laufzeit ohne root.
#
# Aufruf: tests/netz.sh [AUSGABEORDNER]   (Standard BUILD/m4-408)
#   BUILD    Build-Ordner, Standard build-devel/ im Repo (#439, Profil development) (vorher meson compile)
#   KONTO    Konto mit Ordner Revolution-Test für Liste und Nachricht aus dem Cache, Standard
#            KONTO_M365 (microsoft365, Kernpunkt aus #348)
#   KONTO2   zweites Konto für Netz weg beim Verbinden und Markieren ohne Netz, Standard
#            KONTO_GOOGLE (imapx)
#   ORDNER_KONTO  IMAP-Testkonto für Ordner anlegen/löschen ohne Netz, Standard KONTO_IMAP_2
#   PHASEN, PHASEN2  Schritte der Läufe m4-408 bzw. m4-408-gmail (tests/netz-treiber.py)
#   GMAIL    1 (Standard): Lauf m4-408-gmail; 0 lässt ihn aus (Gmail schonen, #411)
#   M365_DEBUG wird für die App nicht weitergegeben (Logprüfung: nur DEBUG-Zeilen, #409)
#   LAUF_TIMEOUT  Sekunden je Treiber-Lauf, Standard 2400; TD_TIMEOUT je tests/testdaten.py, Standard 300
#
# Netz: die App läuft in einem eigenen Netz-Namensraum (unshare -n --map-current-user --keep-caps,
# Session-Bus und Registry bleiben über Unix-Sockets erreichbar). tests/netz-relay.py gibt ihr
# echtes Netz über ein Relais nach außen und nimmt es wieder weg (Standardroute, Verbindungen,
# DNS), ohne sudo und ohne den Rest des Rechners zu berühren. Der Netzmonitor der App ist der von
# GIO für Linux ohne NetworkManager (GIO_USE_NETWORK_MONITOR=netlink): er liest die Routen des
# Namensraums. Der NetworkManager-Monitor (Standard auf dem Desktop) meldete im Namensraum den
# Zustand des ganzen Rechners (gemessen: GNetworkMonitorNM, network-available TRUE ohne Netz).
#
# Zwei App-Starts im eigenen Xvfb (wie scripts/screenshot.sh: echter Session-Bus, Einstellungen im
# Speicher, X11, mark-seen=false):
#   m4-408-vorbereiten  mit Netz: Revolution-Test von KONTO und die Einladung laden (Cache)
#   m4-408              ohne Netz starten, dann Netz zurück, Netz weg, Netz zurück; Netz weg 0,2/0,4/1 s
#                       nach Beginn des Verbindens und zurück (alle Konten müssen verbinden);
#                       Markieren ohne Netz, nach Netz zurück am Server; Ordner am Server angelegt
#                       bzw. gelöscht ohne Netz, danach im Baum; Netz weg 0,1/0,2 s nach Wahl des
#                       Posteingangs von NUR_LESEND_3 (microsoft365, nur lesend), je 4×,
#                       danach lädt er ohne Abbruch, F5 fertig (tests/netz-treiber.py)
#   m4-408-gmail        ohne Netz starten, Netz weg 0,4 s beim Verbinden, Markieren ohne Netz (KONTO2)
#   m4-421              Netzmonitor „online“, aber jede Verbindung scheitert (Relais „kaputt“): Ordner,
#                       Liste, Nachricht und Zähler aus dem Cache, Banner; Verbindung zurück mit
#                       Netzwechsel: Konten verbunden, Ordner und Liste vom Server. Ohne
#                       NUR_LESEND_3 und bei GMAIL=0 ohne KONTO2 (REVOLUTION_SKIP_ACCOUNTS)
#   m4-421-f5           wie m4-421, Verbindung zurück ohne Netzwechsel: F5 verbindet
#   PAUSE    Sekunden ohne Netz je Zyklus, Standard 20 (Gmail zählt getrennte Verbindungen einige
#            Minuten weiter, ab 15 „Too many simultaneous connections“)
# Nach jedem „down“ prüft der Treiber per ss, dass die App keine Verbindung nach außen hält.
# Im Lauf m4-408 nimmt der Treiber nach dem Start ohne Netz den Store von KONTO2 aus der Session
# (Test-Aktion app.test-store, REVOLUTION_TEST_HOOKS=1): Gmail lehnt nach etwa 15 getrennten
# Verbindungen in kurzer Zeit jede Anmeldung ab (gemessen #408 R2), KONTO2 hat den eigenen Lauf.
# Serverstand (Flags, Testordner) prüft tests/testdaten.py im normalen Netz über das Relais.
# Vergleichsliste der Ordner: tests/testdaten.py --ordner (nur lesend, im normalen Netz).
# #431: Ordner der Seitenleiste wählt der Treiber per Name über den Barrierefreiheitsbaum (AT-SPI,
# Bus org.a11y.Bus der Sitzung), nicht über feste Koordinaten; daher ohne GTK_A11Y=none.
# Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“; Exit 0 ohne Fehler.
# #411: Jeder Abbruch eines Teilschritts (Traceback, Exit ≠ 0 von Treiber oder testdaten.py,
# Zeitlimit) ist ein FEHLER (tests/abbruch.sh).
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "$APP_DIR/tests/testkonten.sh"
BUILD="${BUILD:-$APP_DIR/build-devel}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m4-408}"
KONTO="${KONTO:-${KONTO_M365}}"
KONTO2="${KONTO2:-${KONTO_GOOGLE}}"
ORDNER_KONTO="${ORDNER_KONTO:-${KONTO_IMAP_2}}"
LAUF_TIMEOUT="${LAUF_TIMEOUT:-2400}"
FEHLER=0

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
for tool in xvfb-run gdbus python3 unshare setpriv ip nft; do command -v "$tool" >/dev/null || { echo "FEHLER  $tool fehlt"; exit 1; }; done
python3 -c 'import Xlib, PIL' 2>/dev/null || { echo "FEHLER  python3-xlib/python3-pil fehlt"; exit 1; }
python3 -c 'import gi; gi.require_version("Atspi", "2.0")' 2>/dev/null || { echo "FEHLER  gir1.2-atspi-2.0 fehlt"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
[ "$(bus_hat_app)" = "(false,)" ] || { echo "FEHLER  Session-Bus: $APP_ID läuft schon"; exit 1; }
mkdir -p "$OUT"
. "$APP_DIR/tests/abbruch.sh"
# Abbruch vor den Läufen, Exit 1. Ist die Ursache schon als Abbruch von testdaten.py vermerkt
# (tests/abbruch.sh), zählt sie nur dort.
abbrechen () {
  if [ -s "$ABBRUECHE" ]; then echo "        $*"; else echo "FEHLER  $*"; FEHLER=$((FEHLER + 1)); fi
  abbruch_bilanz; echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"; exit 1
}

SCHEMA="$OUT/schema"
mkdir -p "$SCHEMA"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA/"
# #357: Standard ist die Konversationsansicht; dieser Test (#408) prüft Laden und Live-Änderungen je
# Nachricht beim Netzwechsel wie vor #357, daher flach (conversations=false).
printf '[de.freiraum_medien.Revolution]\nmark-seen=false\nconversations=false\n' >"$SCHEMA/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA" || { echo "FEHLER  Schema: glib-compile-schemas $SCHEMA"; exit 1; }

if ! td --ordner >"$OUT/ordner-testdaten.txt" 2>&1; then
  abbrechen "Vergleichsliste fehlt (siehe $OUT/ordner-testdaten.txt)"
fi
echo "OK      Vergleichsliste: tests/testdaten.py --ordner, $(grep -c '^ANZAHL ' "$OUT/ordner-testdaten.txt") Konten"
revolution_test () { td --flags "$1" | sed -n 's/^ORDNER //p'; }
ordner="$(revolution_test "$KONTO")"
[ -n "$ordner" ] || abbrechen "$KONTO: Ordner Revolution-Test nicht gefunden (tests/testdaten.py --flags)"
if [ "${GMAIL:-1}" = 1 ]; then
  ordner2="$(revolution_test "$KONTO2")"
  [ -n "$ordner2" ] || abbrechen "$KONTO2: Ordner Revolution-Test nicht gefunden (tests/testdaten.py --flags)"
else
  # #411: ohne Gmail-Lauf nur die Konto-UID (für OHNE), ohne Anmeldung aus GOA und Registry.
  ordner2="$(PYTHONPATH="$APP_DIR/tests" timeout 60 python3 -c 'import sys, testdaten as t
from gi.repository import EDataServer
print(t.konten_finden(EDataServer.SourceRegistry.new_sync(None))[sys.argv[1]].account.get_uid())' "$KONTO2")/" \
    || abbrechen "$KONTO2: Konto-UID nicht in GOA/Registry"
fi
ordner_uid="$(revolution_test "$ORDNER_KONTO" | cut -d/ -f1)"
[ -n "$ordner_uid" ] || abbrechen "$ORDNER_KONTO: Konto nicht gefunden (tests/testdaten.py --flags)"

# Unix-Sockets brauchen kurze Pfade (108 Byte), daher eigener Ordner unter /tmp.
SOCKDIR="$(mktemp -d)"
RELAIS_LOG="$OUT/relais.log"
: >"$RELAIS_LOG"
python3 "$APP_DIR/tests/netz-relay.py" außen "$SOCKDIR/r.sock" "$RELAIS_LOG" &
AUSSEN=$!
trap 'kill $AUSSEN 2>/dev/null; rm -rf "$SOCKDIR"' EXIT
for _ in $(seq 50); do [ -S "$SOCKDIR/r.sock" ] && break; sleep 0.1; done

lauf () {
  local name="$1" zustand="$2" konto="$3" ordner="$4" phasen="$5" ohne="${6:-}" skip="${7:-}" vorher="${8:-}" rundir rc=0
  rundir="$(mktemp -d)"
  timeout -k 10 "$LAUF_TIMEOUT" unshare -n --map-current-user --keep-caps \
    python3 "$APP_DIR/tests/netz-relay.py" innen "$SOCKDIR/r.sock" "$SOCKDIR/c-$name.sock" "$zustand" -- \
    env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK -u M365_DEBUG \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$SCHEMA" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    ADW_DEBUG_COLOR_SCHEME=prefer-light GIO_USE_NETWORK_MONITOR=netlink \
    G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals REVOLUTION_FOLDER="$ordner" REVOLUTION_MESSAGE=Einladung \
    BIN="$BIN" OUT="$OUT" NAME="$name" APP_DIR="$APP_DIR" CTRL="$SOCKDIR/c-$name.sock" \
    RELAIS_LOG="$RELAIS_LOG" RELAIS_SOCK="$SOCKDIR/r.sock" REF="$OUT/ordner-testdaten.txt" \
    KONTO="$konto" PHASEN="$phasen" ORDNER_KONTO="$ORDNER_KONTO $ordner_uid" \
    OHNE="$ohne" APP_ID="$APP_ID" REVOLUTION_TEST_HOOKS=1 ${skip:+REVOLUTION_SKIP_ACCOUNTS=$skip} VORHER="$vorher" \
    xvfb-run -a -s "-screen 0 1200x1100x24" python3 -I "$APP_DIR/tests/netz-treiber.py" || rc=$?
  rm -rf "$rundir"
  # #411: bis 10 s auf die Freigabe des Namens warten (nach Zeitlimit endet die App per Signal).
  for _ in $(seq 10); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  return "$rc"
}

lauf m4-408-vorbereiten up "$KONTO" "$ordner" "" | tee "$OUT/m4-408-vorbereiten.txt"
teilschritt m4-408-vorbereiten "${PIPESTATUS[0]}" "$OUT/m4-408-vorbereiten.txt"
FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/m4-408-vorbereiten.txt")))
cp "$RELAIS_LOG" "$OUT/relais-vorbereiten.log"
: >"$RELAIS_LOG"
lauf m4-408 down "$KONTO" "$ordner" "${PHASEN:-start,abbruch,m365,flag,ordner}" "${ordner2%%/*}" | tee "$OUT/m4-408.txt"
teilschritt m4-408 "${PIPESTATUS[0]}" "$OUT/m4-408.txt"
FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/m4-408.txt")))
cp "$RELAIS_LOG" "$OUT/relais-m4-408.log"
: >"$RELAIS_LOG"
# Zweites Konto (imapx): Netz weg beim Verbinden und Markieren ohne Netz.
if [ "${GMAIL:-1}" = 1 ]; then
  lauf m4-408-gmail down "$KONTO2" "$ordner2" "${PHASEN2:-abbruch1,flag}" | tee "$OUT/m4-408-gmail.txt"
  teilschritt m4-408-gmail "${PIPESTATUS[0]}" "$OUT/m4-408-gmail.txt"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/m4-408-gmail.txt")))
else
  echo "WERT    m4-408-gmail ausgelassen (GMAIL=0)"
fi

# #421: Netzmonitor „online“, Verbindung scheitert; Zähler gegen das Ende des vorigen Laufs.
vorher="$OUT/m4-408.log"
skip="$(awk -v konto="$NUR_LESEND_3" '$1 == "ANZAHL" && $4 == konto {print $2}' "$OUT/ordner-testdaten.txt")"
if [ "${GMAIL:-1}" = 1 ]; then vorher="$OUT/m4-408-gmail.log"; else skip="$skip,${ordner2%%/*}"; fi
lauf m4-421 kaputt "$KONTO" "$ordner" kaputt "" "$skip" "$vorher" | tee "$OUT/m4-421.txt"
teilschritt m4-421 "${PIPESTATUS[0]}" "$OUT/m4-421.txt"
FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/m4-421.txt")))
cp "$RELAIS_LOG" "$OUT/relais-m4-421.log"
: >"$RELAIS_LOG"
lauf m4-421-f5 kaputt "$KONTO" "$ordner" kaputt-f5 "" "$skip" "$OUT/m4-421.log" | tee "$OUT/m4-421-f5.txt"
teilschritt m4-421-f5 "${PIPESTATUS[0]}" "$OUT/m4-421-f5.txt"
FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/m4-421-f5.txt")))
cp "$RELAIS_LOG" "$OUT/relais-m4-421-f5.log"

abbruch_bilanz
echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
