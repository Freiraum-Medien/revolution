#!/usr/bin/env bash
# Banner „Keine Verbindung“ je Konto (#420) gegen die echten Konten, Verbindung zur Laufzeit gekappt.
#
# Aufruf: tests/banner.sh [AUSGABEORDNER]   (Standard BUILD/m4-420)
#   BUILD     Build-Ordner, Standard build-devel/ im Repo (#439, Profil development) (vorher meson compile)
#   LAEUFE    Zahl der Läufe, Standard 3
#   IMAP      IMAP-Testkonto, Standard KONTO_IMAP_2 (Server sslin.df.eu:993)
#   M365      microsoft365-Testkonto, Standard KONTO_M365 (Graph über 443)
#   LAUF_TIMEOUT  Sekunden je Treiber-Lauf, Standard 600
#
# Netz wie tests/netz.sh: die App läuft im eigenen Netz-Namensraum, tests/netz-relay.py leitet TCP und
# DNS zu den echten Servern weiter. tests/banner-treiber.py („netz“) hält das Netz dabei „up“ (der
# Netzmonitor meldet keinen Wechsel) und kappt nur einzelne Verbindungen per nft im Namensraum:
#   m4-420-erste-N   erste Verbindung zu 993 nach dem Aufbau zurückgesetzt, EDS verbindet beim Laden
#                    der Ordnerliste neu (Fall info@ aus Abnahme #358): kein Banner übrig
#   m4-420-sperre-N  993 und 443 abgewiesen: Banner „Keine Verbindung (2 Konten)“; 993 frei, F5: nur der
#                    Fehler von IMAP weg, Banner „Keine Verbindung“; 443 frei, F5: Banner weg
# Die App lädt nur IMAP und M365; alle anderen Konten (auch NUR_LESEND_3, nur lesend, und die
# übrigen Konten auf sslin.df.eu) schließt REVOLUTION_SKIP_ACCOUNTS aus (REVOLUTION_TEST_HOOKS=1).
# Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“; Exit 0 ohne Fehler. Abbrüche zählen als FEHLER
# (tests/abbruch.sh, #411).
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "$APP_DIR/tests/testkonten.sh"
BUILD="${BUILD:-$APP_DIR/build-devel}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m4-420}"
IMAP="${IMAP:-${KONTO_IMAP_2}}"
M365="${M365:-${KONTO_M365}}"
LAUF_TIMEOUT="${LAUF_TIMEOUT:-600}"
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
. "$APP_DIR/tests/abbruch.sh"

SCHEMA="$OUT/schema"
mkdir -p "$SCHEMA"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA/"
printf '[de.freiraum_medien.Revolution]\nmark-seen=false\nconversations=false\n' >"$SCHEMA/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA" || { echo "FEHLER  Schema: glib-compile-schemas $SCHEMA"; exit 1; }

# UIDs aus der Registry (nur lesend, ohne Anmeldung): IMAP, M365 und alle anderen Mail-Konten zum Ausschluss.
read -r IMAP_UID M365_UID SKIP < <(PYTHONPATH="$APP_DIR/tests" timeout 60 python3 -c 'import sys, testdaten as t
from gi.repository import EDataServer
r = EDataServer.SourceRegistry.new_sync(None)
k = t.konten_finden(r)
imap, m365 = k[sys.argv[1]].account.get_uid(), k[sys.argv[2]].account.get_uid()
rest = sorted(s.get_uid() for s in r.list_sources(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT)
              if s.get_uid() not in (imap, m365))
print(imap, m365, ",".join(rest))' "$IMAP" "$M365")
[ -n "${SKIP:-}" ] || { echo "FEHLER  Konten: UIDs nicht aus GOA/Registry"; exit 1; }
echo "WERT    Konten: IMAP $IMAP $IMAP_UID, M365 $M365 $M365_UID, ausgeschlossen $SKIP"

# Unix-Sockets brauchen kurze Pfade (108 Byte), daher eigener Ordner unter /tmp.
SOCKDIR="$(mktemp -d)"
RELAIS_LOG="$OUT/relais.log"
: >"$RELAIS_LOG"
python3 "$APP_DIR/tests/netz-relay.py" außen "$SOCKDIR/r.sock" "$RELAIS_LOG" &
AUSSEN=$!
trap 'kill $AUSSEN 2>/dev/null; rm -rf "$SOCKDIR"' EXIT
for _ in $(seq 50); do [ -S "$SOCKDIR/r.sock" ] && break; sleep 0.1; done

lauf () {
  local name="$1" phase="$2" rundir rc=0
  rundir="$(mktemp -d)"
  timeout -k 10 "$LAUF_TIMEOUT" unshare -n --map-current-user --keep-caps \
    python3 "$APP_DIR/tests/banner-treiber.py" netz "$SOCKDIR/r.sock" "$SOCKDIR/c-$name.sock" "$SOCKDIR/k-$name.sock" -- \
    env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK -u M365_DEBUG \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$SCHEMA" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    GTK_A11Y=none NO_AT_BRIDGE=1 ADW_DEBUG_COLOR_SCHEME=prefer-light GIO_USE_NETWORK_MONITOR=netlink \
    G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals REVOLUTION_FOLDER="$IMAP_UID/INBOX" \
    BIN="$BIN" OUT="$OUT" NAME="$name" PHASE="$phase" KAPP="$SOCKDIR/k-$name.sock" \
    IMAP_UID="$IMAP_UID" M365_UID="$M365_UID" REVOLUTION_TEST_HOOKS=1 REVOLUTION_SKIP_ACCOUNTS="$SKIP" \
    xvfb-run -a -s "-screen 0 1200x900x24" python3 -I "$APP_DIR/tests/banner-treiber.py" || rc=$?
  rm -rf "$rundir"
  for _ in $(seq 10); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  return "$rc"
}

for n in $(seq "${LAEUFE:-3}"); do
  for phase in erste sperre; do
    name="m4-420-$phase-$n"
    lauf "$name" "$phase" | tee "$OUT/$name.txt"
    teilschritt "$name" "${PIPESTATUS[0]}" "$OUT/$name.txt"
    FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))
    cp "$RELAIS_LOG" "$OUT/relais-$name.log"
    : >"$RELAIS_LOG"
  done
done

abbruch_bilanz
echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
