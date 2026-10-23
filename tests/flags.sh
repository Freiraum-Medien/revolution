#!/usr/bin/env bash
# Gelesen, Markieren, Aktualisieren (#353) gegen die echten Testkonten.
#
# Aufruf: tests/flags.sh [AUSGABEORDNER]   (Standard BUILD/m4-353)
#   BUILD    Build-Ordner, Standard build/ im Repo (vorher meson compile)
#   KONTEN   Testkonten, Standard „KONTO_IMAP_1@example.invalid KONTO_M365@example.invalid KONTO_GOOGLE@example.invalid“
#   LAST     1 (Standard): zusätzlich Lauf in Revolution-Last von privat@ (Auswahl/Scrollposition)
#   IDLE     1 (Standard): zusätzlich Lauf im Posteingang von privat@ (neue/gelöschte Mail per IDLE)
#   BEENDEN  1 (Standard): Strg+K und Umschalt+Strg+L, sofort schließen; beide am Server (privat@)
#   OFFLINE  1 (Standard): Markieren ohne Netz (unshare -n, Bus bleibt erreichbar) in privat@, danach
#            Start mit Netz: die Markierung erreicht den Server; Ausgangszustand wiederhergestellt
#
# Je Konto ein App-Start im eigenen Xvfb (wie scripts/screenshot.sh: echter Session-Bus,
# Einstellungen im Speicher, X11) mit REVOLUTION_FOLDER = Ordner Revolution-Test des Kontos
# (eine Mail: die Einladung aus tests/testdaten.py). Ein Python-Treiber bedient die App per XTest
# (python3-xlib; xdotool ist nicht installiert, gleicher Mechanismus) und prüft jede Änderung
# unabhängig direkt am Server: tests/testdaten.py --flags (IMAP EXAMINE + UID FETCH FLAGS bzw.
# Graph GET, nur lesend). Schritte: Auswahl (Gelesen nach Verzögerung), Strg+Umschalt+K, Strg+K,
# Umschalt+Strg+L (zweimal), Kontextmenü (Rechtsklick, Eingabe), Schnellfolge Strg+K/Umschalt+Strg+K, Auswahl neu (Gelesen nach
# 1,5 s), Wischen rechts/links (Maus-Ziehen), Stern-Knopf der Nachricht, serverseitige Änderung
# (tests/testdaten.py --server) mit IDLE bzw. F5, bei IMAP zusätzlich neue Mail (APPEND) und
# Löschen. Am Ende Ausgangszustand am Server wie vorher. Screenshots je Schritt in AUSGABEORDNER.
# Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“, „WERT …“ für Messwerte; Exit 0 ohne Fehler.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m4-353}"
KONTEN="${KONTEN:-KONTO_IMAP_1@example.invalid KONTO_M365@example.invalid KONTO_GOOGLE@example.invalid}"
FEHLER=0

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
for tool in xvfb-run gdbus python3; do command -v "$tool" >/dev/null || { echo "FEHLER  $tool fehlt"; exit 1; }; done
python3 -c 'import Xlib, PIL' 2>/dev/null || { echo "FEHLER  python3-xlib/python3-pil fehlt"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
[ "$(bus_hat_app)" = "(false,)" ] || { echo "FEHLER  Session-Bus: $APP_ID läuft schon"; exit 1; }
mkdir -p "$OUT"

# Schema mit mark-seen=false (.gschema.override) für Läufe, die Zeilen wählen, ohne Gelesen zu
# prüfen (ohne Netz, Revolution-Last).
SCHEMA="$OUT/schema"
mkdir -p "$SCHEMA"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA/"
printf '[de.freiraum_medien.Revolution]\nmark-seen=false\n' >"$SCHEMA/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA" || { echo "FEHLER  Schema: glib-compile-schemas $SCHEMA"; exit 1; }

lauf () {
  local name="$1" konto="$2" ordner="$3" schema="${4:-$BUILD/data}" rundir rc=0
  rundir="$(mktemp -d)"
  # shellcheck disable=SC2086 # PRAEFIX: z. B. „unshare -n --map-current-user“ (ohne Netz)
  ${PRAEFIX:-} env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$schema" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    GTK_A11Y=none NO_AT_BRIDGE=1 ADW_DEBUG_COLOR_SCHEME=prefer-light \
    G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals REVOLUTION_FOLDER="$ordner" \
    UNGELESEN_VORHER="$OUT/last-ungelesen-vorher.txt" BIN="$BIN" OUT="$OUT" NAME="$name" KONTO="$konto" ORDNER="$ordner" APP_DIR="$APP_DIR" \
    xvfb-run -a -s "-screen 0 1200x760x24" python3 -I "$APP_DIR/tests/flags-treiber.py" || rc=$?
  rm -rf "$rundir"
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  return "$rc"
}

for konto in $KONTEN; do
  name="m4-353-${konto%%@*}"
  if ! python3 "$APP_DIR/tests/testdaten.py" --flags "$konto" >"$OUT/$name-vorher.txt" 2>&1; then
    echo "FEHLER  $konto: tests/testdaten.py --flags (siehe $OUT/$name-vorher.txt)"; FEHLER=$((FEHLER + 1)); continue
  fi
  ordner="$(sed -n 's/^ORDNER //p' "$OUT/$name-vorher.txt")"
  lauf "$name" "$konto" "$ordner" | tee "$OUT/$name.txt"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))
done

# Ohne Netz markieren, mit Netz nachholen (privat@ Revolution-Test).
server_markiert () { python3 "$APP_DIR/tests/testdaten.py" --flags KONTO_IMAP_1@example.invalid | sed -n 's/^FLAGS [01] \([01]\) \[Revolution-Test\] Einladung.*/\1/p'; }
server_gelesen () { python3 "$APP_DIR/tests/testdaten.py" --flags KONTO_IMAP_1@example.invalid | sed -n 's/^FLAGS \([01]\) [01] \[Revolution-Test\] Einladung.*/\1/p'; }
if [ "${OFFLINE:-1}" = 1 ]; then
  ordner="$(python3 "$APP_DIR/tests/testdaten.py" --flags KONTO_IMAP_1@example.invalid | sed -n 's/^ORDNER //p')"
  gelesen_vorher="$(server_gelesen)"
  if [ "$(server_markiert)" = 0 ]; then
    # Erst mit Netz laden: imapx übergeht fremdes EXPUNGE außerhalb von IDLE (README), gelöschte
    # Live-Mails stünden sonst im Cache noch oben; das Laden mit Netz gleicht den Cache ab.
    lauf m4-353-vorbereiten KONTO_IMAP_1@example.invalid "$ordner" "$SCHEMA" >"$OUT/m4-353-vorbereiten.txt"
    PRAEFIX="unshare -n --map-current-user" lauf m4-353-offline KONTO_IMAP_1@example.invalid "$ordner" "$SCHEMA" | tee "$OUT/m4-353-offline.txt"
    m="$(server_markiert)"
    if [ "$m" = 0 ]; then echo "OK      m4-353-offline: Server nach dem Lauf ohne Netz unverändert (markiert=0)"
    else echo "FEHLER  m4-353-offline: Server markiert=$m ohne Netz"; fi | tee -a "$OUT/m4-353-offline.txt"
    lauf m4-353-nachholen KONTO_IMAP_1@example.invalid "$ordner" "$SCHEMA" | tee "$OUT/m4-353-nachholen.txt"
    m="$(server_markiert)"
    if [ "$m" = 1 ]; then echo "OK      m4-353-nachholen: Markierung ohne Netz nach Start mit Netz am Server (markiert=1)"
    else echo "FEHLER  m4-353-nachholen: Server markiert=$m, erwartet 1"; fi | tee -a "$OUT/m4-353-nachholen.txt"
    python3 "$APP_DIR/tests/testdaten.py" --server KONTO_IMAP_1@example.invalid unmarkiert >/dev/null
    m="$(server_markiert)"
    g="$(server_gelesen)"
    if [ "$m" = 0 ] && [ "$g" = "$gelesen_vorher" ]; then echo "OK      m4-353-nachholen: Ausgangszustand am Server (markiert=0, gelesen=$g wie vorher)"
    else echo "FEHLER  m4-353-nachholen: Ausgangszustand nicht erreicht (markiert=$m, gelesen=$g, vorher $gelesen_vorher)"; fi | tee -a "$OUT/m4-353-nachholen.txt"
  else
    echo "FEHLER  m4-353-offline: Einladung in privat@ schon markiert, Lauf übersprungen" | tee "$OUT/m4-353-offline.txt"
  fi
  FEHLER=$((FEHLER + $(cat "$OUT/m4-353-offline.txt" "$OUT/m4-353-nachholen.txt" 2>/dev/null | grep -c '^FEHLER')))
fi

# Beenden mit wartenden Flag-Aufträgen (privat@): Strg+K, 30 ms später Umschalt+Strg+L, sofort
# schließen; die Anwendung bleibt gehalten, bis beide am Server sind (mail-view.c, hold_for_flags).
if [ "${BEENDEN:-1}" = 1 ]; then
  ordner="$(python3 "$APP_DIR/tests/testdaten.py" --flags KONTO_IMAP_1@example.invalid | sed -n 's/^ORDNER //p')"
  g0="$(server_gelesen)"; m0="$(server_markiert)"
  if [ "$g0" = 0 ] && [ "$m0" = 0 ]; then
    lauf m4-353-beenden KONTO_IMAP_1@example.invalid "$ordner" "$SCHEMA" | tee "$OUT/m4-353-beenden.txt"
    g="$(server_gelesen)"; m="$(server_markiert)"
    if [ "$g" = 1 ] && [ "$m" = 1 ]; then echo "OK      m4-353-beenden: Server nach dem Schließen gelesen=1 markiert=1"
    else echo "FEHLER  m4-353-beenden: Server gelesen=$g markiert=$m, erwartet 1/1"; fi | tee -a "$OUT/m4-353-beenden.txt"
    if pgrep -f "^$BIN" >/dev/null; then echo "FEHLER  m4-353-beenden: Rest-Prozess $BIN"
    else echo "OK      m4-353-beenden: kein Rest-Prozess"; fi | tee -a "$OUT/m4-353-beenden.txt"
    python3 "$APP_DIR/tests/testdaten.py" --server KONTO_IMAP_1@example.invalid ungelesen >/dev/null
    python3 "$APP_DIR/tests/testdaten.py" --server KONTO_IMAP_1@example.invalid unmarkiert >/dev/null
    g="$(server_gelesen)"; m="$(server_markiert)"
    if [ "$g" = 0 ] && [ "$m" = 0 ]; then echo "OK      m4-353-beenden: Ausgangszustand am Server (0/0)"
    else echo "FEHLER  m4-353-beenden: Ausgangszustand nicht erreicht ($g/$m)"; fi | tee -a "$OUT/m4-353-beenden.txt"
  else
    echo "FEHLER  m4-353-beenden: Einladung in privat@ nicht ungelesen/unmarkiert ($g0/$m0), Lauf übersprungen" | tee "$OUT/m4-353-beenden.txt"
  fi
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/m4-353-beenden.txt")))
fi

# IDLE: imapx startet IDLE bevorzugt am Posteingang; der gewählte Ordner bekommt IDLE nur, wenn der
# Posteingang schon auf einer anderen Verbindung idlet (camel-imapx-conn-manager.c:1268-1278). Daher
# der Beleg im Posteingang.
if [ "${IDLE:-1}" = 1 ]; then
  uid="$(python3 "$APP_DIR/tests/testdaten.py" --flags KONTO_IMAP_1@example.invalid 2>/dev/null | sed -n 's#^ORDNER \([^/]*\)/.*#\1#p')"
  lauf m4-353-idle KONTO_IMAP_1@example.invalid "$uid/INBOX" | tee "$OUT/m4-353-idle.txt"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/m4-353-idle.txt")))
fi

# Revolution-Last: mark-seen=false (Schema mit .gschema.override), damit nur die Kürzel Flags
# ändern; ungelesene UIDs am Server vorher/nachher gleich, sonst FEHLER und Rückstellung per IMAP.
if [ "${LAST:-1}" = 1 ]; then
  python3 "$APP_DIR/tests/testdaten.py" --ungelesen KONTO_IMAP_1@example.invalid Revolution-Last >"$OUT/last-ungelesen-vorher.txt" 2>&1
  ordner="$(python3 "$APP_DIR/tests/testdaten.py" --last 3000 --dry-run 2>/dev/null | sed -n 's/^LAST \(.*\) [0-9]*$/\1/p')"
  lauf m4-353-last KONTO_IMAP_1@example.invalid "$ordner" "$SCHEMA" | tee "$OUT/m4-353-last.txt"
  python3 "$APP_DIR/tests/testdaten.py" --ungelesen KONTO_IMAP_1@example.invalid Revolution-Last >"$OUT/last-ungelesen-nachher.txt" 2>&1
  if grep -q '^UNGELESEN ' "$OUT/last-ungelesen-vorher.txt" && cmp -s "$OUT/last-ungelesen-vorher.txt" "$OUT/last-ungelesen-nachher.txt"; then
    echo "OK      m4-353-last: Server ungelesen wie vorher ($(cut -d' ' -f2 "$OUT/last-ungelesen-vorher.txt"))" | tee -a "$OUT/m4-353-last.txt"
  else
    echo "FEHLER  m4-353-last: Server ungelesen vorher $(cut -d' ' -f2 "$OUT/last-ungelesen-vorher.txt"), nachher $(cut -d' ' -f2 "$OUT/last-ungelesen-nachher.txt"); Rückstellung per IMAP:" | tee -a "$OUT/m4-353-last.txt"
    python3 "$APP_DIR/tests/testdaten.py" --ungelesen KONTO_IMAP_1@example.invalid Revolution-Last --soll "$OUT/last-ungelesen-vorher.txt" | sed 's/^/        /'
  fi
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/m4-353-last.txt")))
fi

echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
