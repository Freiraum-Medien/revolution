#!/usr/bin/env bash
# Löschen mit Rückgängig, Archivieren, Spam, Verschieben (#363) gegen die echten Testkonten.
#
# Aufruf: flock build/.run.lock tests/aktionen.sh [AUSGABEORDNER]   (Standard BUILD/m5-363)
#   BUILD    Build-Ordner, Standard build-devel/ im Repo (#439, Profil development) (vorher meson compile)
#   KONTEN   Standard „KONTO_IMAP_2 KONTO_M365 KONTO_GOOGLE“
#   LAEUFE   Durchgänge, Standard 3
#   LAUF_TIMEOUT  Sekunden je Treiber-Lauf, Standard 900; TD_TIMEOUT je tests/testdaten.py, Standard 300
#
# Je Durchgang und Konto ein App-Start im eigenen Xvfb (echter Session-Bus, Einstellungen im Speicher,
# mark-seen=false) im eigenen Ordner Revolution-Aktionen-<Uhrzeit> (daneben …-Ziel) und der Aktions-Mail darin:
# tests/testdaten.py --ort KONTO aktion anlegen, am Ende des Durchgangs und bei jedem Abbruch (trap)
# … aufraeumen (Mail endgültig, nur ihre eigene UID bzw. ID, und beide Ordner weg; PM 28.10.: nicht in
# Revolution-Test, dort arbeiten andere Tests). Auswahl über REVOLUTION_MESSAGE. Treiber
# tests/aktionen-treiber.py: Entf, Umschalt+Strg+J, Umschalt+Strg+M mit Menüeintrag, Ziehen auf „…-Ziel“
# der Seitenleiste, Entf mit Knopf „Rückgängig“, Strg+Alt+A (Gmail ohne Archivordner: Banner); nach jeder
# Aktion Ziel am Server (tests/testdaten.py --ort, IMAP bzw. Graph) und nächste Nachricht gewählt, dann
# Rückgängig (Strg+Z bzw. Knopf im Hinweis) und Stand am Server wie vorher. geops@ und outlook.com
# zusätzlich: Konversation im Posteingang mit ⌫ löschen und zurück (nur ihre eingehenden Mails).
# Die App lädt nur das Konto des Laufs (REVOLUTION_SKIP_ACCOUNTS: alle anderen, damit auch die nur
# lesend freigegebenen). microsoft365: M365_DEBUG=1, im Log nur „[graph] METHODE Pfad“; geprüft: kein
# PATCH. Gmail-Lauf dunkel (Screenshot des Hinweises hell und dunkel). Am Ende jedes Laufs
# --ort … zurueck (Mail wieder im Ausgangsordner, falls ein Schritt scheiterte).
# Bekannte Grenze EDS imapx (Geisterzeilen nach schnellem Verschieben/Rückgängig, docs/ENTWICKLUNG.md „#363“) meldet der
# Treiber als „WERT … BEKANNT …“, nicht als FEHLER und nicht stillschweigend.
# Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“; Exit 0 ohne Fehler (#411: Abbruch = FEHLER).
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "$APP_DIR/tests/testkonten.sh"
BUILD="${BUILD:-$APP_DIR/build-devel}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m5-363}"
KONTEN="${KONTEN:-${KONTO_IMAP_2} ${KONTO_M365} ${KONTO_GOOGLE}}"
LAEUFE="${LAEUFE:-3}"
LAUF_TIMEOUT="${LAUF_TIMEOUT:-900}"
FEHLER=0

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
python3 -c 'import Xlib, PIL' 2>/dev/null || { echo "FEHLER  python3-xlib/python3-pil fehlt"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
mkdir -p "$OUT"
. "$APP_DIR/tests/abbruch.sh"

SCHEMA="$OUT/schema"
mkdir -p "$SCHEMA"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA/"
printf '[de.freiraum_medien.Revolution]\nmark-seen=false\n' >"$SCHEMA/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA" || { echo "FEHLER  Schema: glib-compile-schemas $SCHEMA"; exit 1; }

# UID des Kontos und alle übrigen Mail-Konten (ausgeschlossen).
konten () {
  python3 -c '
import sys; sys.path.insert(0, sys.argv[1] + "/tests")
import testdaten as t
from gi.repository import EDataServer
r = EDataServer.SourceRegistry.new_sync(None)
uid = t.konten_finden(r)[sys.argv[2]].account.get_uid()
print(uid, ",".join(sorted(s.get_uid() for s in r.list_sources(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT) if s.get_uid() != uid)))' "$APP_DIR" "$1"
}

lauf () {
  local name="$1" konto="$2" ordner="$3" was="$4" skip="$5" schema="$6" rundir rc=0
  # Innerhalb der run.lock: die vorige Instanz gibt den Namen verzögert frei (bis 30 s).
  for _ in $(seq 30); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || { echo "FEHLER  $name: $APP_ID läuft schon"; return 1; }
  rundir="$(mktemp -d)"
  timeout -k 10 "$LAUF_TIMEOUT" env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$SCHEMA" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG="no-portals${GDK_DEBUG_ZUSATZ:-}" ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    ADW_DEBUG_COLOR_SCHEME="$schema" G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals \
    REVOLUTION_FOLDER="$ordner" REVOLUTION_MESSAGE="$([ "$was" = aktion ] && echo 'Aktionen 363' || echo 'Konversation Marktstand')" \
    REVOLUTION_TEST_HOOKS=1 REVOLUTION_SKIP_ACCOUNTS="$skip" $([ "${konto#*outlook}" != "$konto" ] && echo M365_DEBUG=1) \
    BIN="$BIN" OUT="$OUT" NAME="$name" KONTO="$konto" ORDNER="$ordner" WAS="$was" APP_DIR="$APP_DIR" \
    xvfb-run -a -s "-screen 0 1200x900x24" python3 -I "$APP_DIR/tests/aktionen-treiber.py" || rc=$?
  rm -rf "$rundir"
  sed -i -E 's/(Bearer|TOKEN=)\S*/\1REDACTED/g' "$OUT/$name.log" 2>/dev/null
  return "$rc"
}

aufraeumen () {
  for konto in $KONTEN; do td --ort "$konto" aktion aufraeumen >>"$OUT/aufraeumen.txt" 2>&1; done
}
trap aufraeumen EXIT
trap 'exit 1' INT TERM

for n in $(seq 1 "$LAEUFE"); do
  export AKTION_ORDNER="Revolution-Aktionen-$(date +%H%M%S)" AKTION_IDS="$OUT/$n-ordner-ids.txt"
  for konto in $KONTEN; do
    read -r uid skip < <(konten "$konto") || { echo "FEHLER  $konto: UID nicht aus GOA/Registry"; FEHLER=$((FEHLER + 1)); continue; }
    schema=prefer-light
    [ "${konto#*gmail}" != "$konto" ] && schema=prefer-dark
    case "$konto" in
      "$KONTO_M365") eingang=Posteingang ;;
      "$KONTO_GOOGLE") eingang= ;;
      *) eingang=INBOX ;;
    esac
    td --ort "$konto" aktion anlegen >"$OUT/$n-${konto%%@*}-anlegen.txt" 2>&1
    heim="$(sed -n 's/^HEIM //p' "$OUT/$n-${konto%%@*}-anlegen.txt")"
    for was in aktion konversation; do
      [ "$was" = konversation ] && [ -z "$eingang" ] && continue
      name="$n-${konto%%@*}-$was"
      ordner="$uid/$([ "$was" = aktion ] && echo "$heim" || echo "$eingang")"
      lauf "$name" "$konto" "$ordner" "$was" "$skip" "$schema" | tee "$OUT/$name.txt"
      teilschritt "$name" "${PIPESTATUS[0]}" "$OUT/$name.txt"
      td --ort "$konto" "$was" zurueck >"$OUT/$name-zurueck.txt" 2>&1
      FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))
    done
  done
  aufraeumen
done
trap - EXIT

abbruch_bilanz
echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
