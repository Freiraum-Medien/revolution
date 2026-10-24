#!/usr/bin/env bash
# Online/Offline nach Netzstatus (#408) gegen die echten Konten, Netz zur Laufzeit ohne root.
#
# Aufruf: tests/netz.sh [AUSGABEORDNER]   (Standard BUILD/m4-408)
#   BUILD    Build-Ordner, Standard build/ im Repo (vorher meson compile)
#   KONTO    Konto mit Ordner Revolution-Test für Liste und Nachricht aus dem Cache, Standard
#            KONTO_M365@example.invalid (microsoft365, Kernpunkt aus #348)
#   KONTO2   zweites Konto für Netz weg beim Verbinden und Markieren ohne Netz, Standard
#            KONTO_GOOGLE@example.invalid (imapx)
#   ORDNER_KONTO  IMAP-Testkonto für Ordner anlegen/löschen ohne Netz, Standard KONTO_IMAP_2@example.invalid
#   PHASEN, PHASEN2  Schritte der Läufe m4-408 bzw. m4-408-gmail (tests/netz-treiber.py)
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
#                       bzw. gelöscht ohne Netz, danach im Baum; Netz weg 0,1/0,2 s nach Klick auf
#                       den Posteingang von NUR_LESEND_3@example.invalid (microsoft365, nur lesend), je 4×,
#                       danach lädt er ohne Abbruch, F5 fertig (tests/netz-treiber.py)
#   m4-408-gmail        ohne Netz starten, Netz weg beim Verbinden, Markieren ohne Netz (KONTO2)
# Serverstand (Flags, Testordner) prüft tests/testdaten.py im normalen Netz über das Relais.
# Vergleichsliste der Ordner: tests/testdaten.py --ordner (nur lesend, im normalen Netz).
# Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“; Exit 0 ohne Fehler.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m4-408}"
KONTO="${KONTO:-KONTO_M365@example.invalid}"
KONTO2="${KONTO2:-KONTO_GOOGLE@example.invalid}"
ORDNER_KONTO="${ORDNER_KONTO:-KONTO_IMAP_2@example.invalid}"
FEHLER=0

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
for tool in xvfb-run gdbus python3 unshare setpriv ip nft; do command -v "$tool" >/dev/null || { echo "FEHLER  $tool fehlt"; exit 1; }; done
python3 -c 'import Xlib, PIL' 2>/dev/null || { echo "FEHLER  python3-xlib/python3-pil fehlt"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
[ "$(bus_hat_app)" = "(false,)" ] || { echo "FEHLER  Session-Bus: $APP_ID läuft schon"; exit 1; }
mkdir -p "$OUT"

SCHEMA="$OUT/schema"
mkdir -p "$SCHEMA"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA/"
printf '[de.freiraum_medien.Revolution]\nmark-seen=false\n' >"$SCHEMA/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA" || { echo "FEHLER  Schema: glib-compile-schemas $SCHEMA"; exit 1; }

if ! timeout 300 python3 "$APP_DIR/tests/testdaten.py" --ordner >"$OUT/ordner-testdaten.txt" 2>&1; then
  echo "FEHLER  Vergleichsliste: tests/testdaten.py --ordner (siehe $OUT/ordner-testdaten.txt)"; exit 1
fi
echo "OK      Vergleichsliste: tests/testdaten.py --ordner, $(grep -c '^ANZAHL ' "$OUT/ordner-testdaten.txt") Konten"
revolution_test () { timeout 120 python3 "$APP_DIR/tests/testdaten.py" --flags "$1" | sed -n 's/^ORDNER //p'; }
ordner="$(revolution_test "$KONTO")"
[ -n "$ordner" ] || { echo "FEHLER  $KONTO: Ordner Revolution-Test nicht gefunden (tests/testdaten.py --flags)"; exit 1; }
ordner2="$(revolution_test "$KONTO2")"
[ -n "$ordner2" ] || { echo "FEHLER  $KONTO2: Ordner Revolution-Test nicht gefunden (tests/testdaten.py --flags)"; exit 1; }
ordner_uid="$(revolution_test "$ORDNER_KONTO" | cut -d/ -f1)"
[ -n "$ordner_uid" ] || { echo "FEHLER  $ORDNER_KONTO: Konto nicht gefunden (tests/testdaten.py --flags)"; exit 1; }

# Unix-Sockets brauchen kurze Pfade (108 Byte), daher eigener Ordner unter /tmp.
SOCKDIR="$(mktemp -d)"
RELAIS_LOG="$OUT/relais.log"
: >"$RELAIS_LOG"
python3 "$APP_DIR/tests/netz-relay.py" außen "$SOCKDIR/r.sock" "$RELAIS_LOG" &
AUSSEN=$!
trap 'kill $AUSSEN 2>/dev/null; rm -rf "$SOCKDIR"' EXIT
for _ in $(seq 50); do [ -S "$SOCKDIR/r.sock" ] && break; sleep 0.1; done

lauf () {
  local name="$1" zustand="$2" konto="$3" ordner="$4" phasen="$5" rundir rc=0
  rundir="$(mktemp -d)"
  unshare -n --map-current-user --keep-caps \
    python3 "$APP_DIR/tests/netz-relay.py" innen "$SOCKDIR/r.sock" "$SOCKDIR/c-$name.sock" "$zustand" -- \
    env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$SCHEMA" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    GTK_A11Y=none NO_AT_BRIDGE=1 ADW_DEBUG_COLOR_SCHEME=prefer-light GIO_USE_NETWORK_MONITOR=netlink \
    G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals REVOLUTION_FOLDER="$ordner" REVOLUTION_MESSAGE=Einladung \
    BIN="$BIN" OUT="$OUT" NAME="$name" APP_DIR="$APP_DIR" CTRL="$SOCKDIR/c-$name.sock" \
    RELAIS_LOG="$RELAIS_LOG" RELAIS_SOCK="$SOCKDIR/r.sock" REF="$OUT/ordner-testdaten.txt" \
    KONTO="$konto" PHASEN="$phasen" ORDNER_KONTO="$ORDNER_KONTO $ordner_uid" \
    xvfb-run -a -s "-screen 0 1200x1100x24" python3 -I "$APP_DIR/tests/netz-treiber.py" || rc=$?
  rm -rf "$rundir"
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  return "$rc"
}

lauf m4-408-vorbereiten up "$KONTO" "$ordner" "" | tee "$OUT/m4-408-vorbereiten.txt"
FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/m4-408-vorbereiten.txt")))
cp "$RELAIS_LOG" "$OUT/relais-vorbereiten.log"
: >"$RELAIS_LOG"
lauf m4-408 down "$KONTO" "$ordner" "${PHASEN:-start,abbruch,m365,flag,ordner}" | tee "$OUT/m4-408.txt"
FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/m4-408.txt")))
cp "$RELAIS_LOG" "$OUT/relais-m4-408.log"
: >"$RELAIS_LOG"
# Zweites Konto (imapx): Netz weg beim Verbinden und Markieren ohne Netz.
lauf m4-408-gmail down "$KONTO2" "$ordner2" "${PHASEN2:-abbruch,flag}" | tee "$OUT/m4-408-gmail.txt"
FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/m4-408-gmail.txt")))

echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
