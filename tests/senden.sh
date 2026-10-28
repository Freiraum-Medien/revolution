#!/usr/bin/env bash
# Postausgang, Senden, Fehlerfall (#362) gegen die echten Testkonten.
#
# Aufruf: tests/senden.sh [AUSGABEORDNER]   (Standard BUILD/m5-362)
#   BUILD     Build-Ordner, Standard build-devel/ im Repo (#439, Profil development) (vorher meson compile)
#   LAEUFE    Zahl der Läufe, Standard 3
#   PHASEN    Standard „senden netz kill kopie“ (tests/senden-treiber.py)
#   LAUF_TIMEOUT  Sekunden je Treiber-Lauf, Standard 1800
#
# Netz wie tests/banner.sh: die App läuft im eigenen Netz-Namensraum, tests/netz-relay.py leitet TCP und
# DNS zu den echten Servern weiter; tests/banner-treiber.py („netz“) schaltet das Netz („up“/„down“) und
# sperrt einzelne Ports per nft (SMTP 465 von sslout.df.eu). Serverstand prüft tests/testdaten.py
# (--zustellung, nur lesend) im normalen Netz über das Relais, am Ende löscht --aufraeumen nur die
# Testmails des Laufs. Absender und Empfänger nur die vier Testkonten. Die App lädt je Phase nur die
# sendenden Konten (senden: geops@, outlook.com, Gmail; netz, kill, kopie: geops@); alle anderen, auch die nur
# lesend freigegebenen, schließt REVOLUTION_SKIP_ACCOUNTS aus (REVOLUTION_TEST_HOOKS=1). Jeder Start hat
# einen eigenen Postausgang (XDG_DATA_HOME unter AUSGABEORDNER, danach gelöscht).
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
OUT="$(realpath -m "${1:-$BUILD/m5-362}")"
LAUF_TIMEOUT="${LAUF_TIMEOUT:-1800}"
PHASEN="${PHASEN:-senden netz kill kopie}"
FEHLER=0

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
for tool in xvfb-run gdbus python3 unshare setpriv ip nft; do command -v "$tool" >/dev/null || { echo "FEHLER  $tool fehlt"; exit 1; }; done
python3 -c 'import Xlib, PIL' 2>/dev/null || { echo "FEHLER  python3-xlib/python3-pil fehlt"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
# Warten, bis eine vorige Instanz (anderer Lauf unter derselben Sperre) den Bus verlassen hat.
for _ in $(seq 60); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
[ "$(bus_hat_app)" = "(false,)" ] || { echo "FEHLER  Session-Bus: $APP_ID läuft schon"; exit 1; }
mkdir -p "$OUT"
. "$APP_DIR/tests/abbruch.sh"

SCHEMA="$OUT/schema"
mkdir -p "$SCHEMA"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA/"
printf '[de.freiraum_medien.Revolution]\nmark-seen=false\nconversations=false\n' >"$SCHEMA/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA" || { echo "FEHLER  Schema: glib-compile-schemas $SCHEMA"; exit 1; }

# UIDs aus der Registry (nur lesend, ohne Anmeldung): je Testkonto Konto, Identität, Versanddienst.
KONTEN="$(PYTHONPATH="$APP_DIR/tests" timeout 60 python3 -c 'import testdaten as t
from gi.repository import EDataServer as E
k = t.konten_finden(E.SourceRegistry.new_sync(None))
print(";".join("%s=%s=%s=%s" % (a, x.account.get_uid(),
      x.account.get_extension(E.SOURCE_EXTENSION_MAIL_ACCOUNT).get_identity_uid(), x.transport.get_uid())
      for a, x in sorted(k.items())))')"
[ -n "$KONTEN" ] || { echo "FEHLER  Konten: UIDs nicht aus GOA/Registry"; exit 1; }
echo "WERT    Konten: $KONTEN"
uid_von () { tr ';' '\n' <<<"$KONTEN" | sed -n "s/^$1=\([^=]*\)=.*/\1/p"; }
skip_ausser () {
  PYTHONPATH="$APP_DIR/tests" timeout 60 python3 -c 'import sys
from gi.repository import EDataServer as E
r = E.SourceRegistry.new_sync(None)
print(",".join(sorted(s.get_uid() for s in r.list_sources(E.SOURCE_EXTENSION_MAIL_ACCOUNT)
                      if s.get_uid() not in sys.argv[1:])))' "$@"
}
SKIP_SENDEN="$(skip_ausser "$(uid_von "$KONTO_IMAP_2")" "$(uid_von "$KONTO_M365")" "$(uid_von "$KONTO_GOOGLE")")"
SKIP_GEOPS="$(skip_ausser "$(uid_von "$KONTO_IMAP_2")")"
[ -n "$SKIP_SENDEN" ] && [ -n "$SKIP_GEOPS" ] || { echo "FEHLER  Konten: Ausschlussliste leer"; exit 1; }

# Unix-Sockets brauchen kurze Pfade (108 Byte), daher eigener Ordner unter /tmp.
SOCKDIR="$(mktemp -d)"
RELAIS_LOG="$OUT/relais.log"
: >"$RELAIS_LOG"
python3 "$APP_DIR/tests/netz-relay.py" außen "$SOCKDIR/r.sock" "$RELAIS_LOG" &
AUSSEN=$!
trap 'kill $AUSSEN 2>/dev/null; rm -rf "$SOCKDIR"' EXIT
for _ in $(seq 50); do [ -S "$SOCKDIR/r.sock" ] && break; sleep 0.1; done

lauf () {
  local name="$1" phase="$2" skip="$3" rundir rc=0
  rundir="$(mktemp -d)"
  timeout -k 10 "$LAUF_TIMEOUT" unshare -n --map-current-user --keep-caps \
    python3 "$APP_DIR/tests/banner-treiber.py" netz "$SOCKDIR/r.sock" "$SOCKDIR/c-$name.sock" "$SOCKDIR/k-$name.sock" -- \
    env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK -u M365_DEBUG \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$SCHEMA" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    GTK_A11Y=none NO_AT_BRIDGE=1 ADW_DEBUG_COLOR_SCHEME=prefer-light GIO_USE_NETWORK_MONITOR=netlink \
    G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals \
    BIN="$BIN" OUT="$OUT" NAME="$name" PHASE="$phase" APP_ID="$APP_ID" KONTEN="$KONTEN" \
    KAPP="$SOCKDIR/k-$name.sock" CTRL="$SOCKDIR/c-$name.sock" RSOCK="$SOCKDIR/r.sock" \
    REVOLUTION_TEST_HOOKS=1 REVOLUTION_SKIP_ACCOUNTS="$skip" \
    xvfb-run -a -s "-screen 0 1200x900x24" python3 -I "$APP_DIR/tests/senden-treiber.py" || rc=$?
  rm -rf "$rundir"
  for _ in $(seq 10); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  return "$rc"
}

for n in $(seq "${LAEUFE:-3}"); do
  for phase in $PHASEN; do
    name="m5-362-$phase-$n"
    skip="$SKIP_GEOPS"
    [ "$phase" = senden ] && skip="$SKIP_SENDEN"
    lauf "$name" "$phase" "$skip" | tee "$OUT/$name.txt"
    teilschritt "$name" "${PIPESTATUS[0]}" "$OUT/$name.txt"
    FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))
    cp "$RELAIS_LOG" "$OUT/relais-$name.log"
    : >"$RELAIS_LOG"
  done
done

abbruch_bilanz
echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
