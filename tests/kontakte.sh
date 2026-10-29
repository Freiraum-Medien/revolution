#!/usr/bin/env bash
# Adressbücher und Kontaktliste (#368) gegen die echten Adressbücher aus GOA.
#
# Aufruf: tests/kontakte.sh [AUSGABEORDNER]   (Standard BUILD/m6-368)
#   BUILD    Build-Ordner, Standard build-devel/ im Repo (#439, Profil development) (vorher meson compile)
#   LAUF_TIMEOUT  Sekunden je Treiber-Lauf, Standard 900; TD_TIMEOUT je tests/testdaten.py, Standard 300
#   GRAPH_WARTEN  Sekunden, die der Graph-Lauf auf eine Server-Änderung ohne Refresh wartet, Standard 60
#
# Vergleich unabhängig von der App: tests/testdaten.py --adressbuecher (eigener Prozess, Python-GI EBook.BookClient,
# dieselbe Schicht wie GNOME Contacts; GNOME Contacts selbst öffnete ein Fenster). Zwei App-Starts im eigenen Xvfb
# (echter Session-Bus, Einstellungen im Speicher, AT-SPI wie tests/suche.sh), Treiber tests/kontakte-treiber.py:
#   google   hell, Google-CardDAV des Gmail test account (518e9322…): Seitenleiste = Registry (UID, Konto, Name, Kopfzeilen,
#            nur lesend), je Buch Kontaktzahl = Python-GI, Alt+M; Testkontakt von außen anlegen, suchen (Treffer,
#            Nicht-Treffer, Escape), ändern, löschen – jede Änderung live ohne Neuladen.
#   outlook  dunkel, microsoft365 „Kontakte“ outlook.com (3be9d2a4…): dasselbe live über EDS; dazu Gegenprobe am Server
#            über Graph (ohne EDS): erscheint/verschwindet der Kontakt ohne Refresh? sonst nach e_book_client_refresh.
#   google-dunkel  #369 dunkel, nur die Detailansicht im Google-Buch (Bild m6-369-detail-dunkel.png).
# #369 (DETAIL, kontakte-treiber.py): google und google-dunkel mit Testkontakt „Erika Revolution-Test Detail …“ (alle
# Felder), Vergleich mit Python-GI, „Mail schreiben“ (Composer nur geöffnet, nie gesendet), google zusätzlich Feld live
# ändern; outlook legt denselben Kontakt einmal an und belegt, welche Felder microsoft365 übernimmt.
# #370 (kontakte-treiber.py MODUS schreiben/absender): schreiben-google (hell, Start in „Persönlich“, Kontakt mit allen
# Feldern über das Formular im Google-Buch, ändern, Rückgängig, löschen; Nur-lesend-Buch; „Persönlich“ anlegen/löschen),
# schreiben-outlook (dunkel, dasselbe in microsoft365 „Kontakte“, höchstens 3 Schreibvorgänge), absender (Testmail im
# Posteingang des zweiten IMAP-Testkontos, Formular vorausgefüllt, Escape; speichern, zweites Mal: vorhandener Kontakt,
# löschen), beenden (Löschen vorgemerkt, Fenster sofort schließen: gelöscht vor dem Ende, Server 404). Gegenprobe Python-GI und am Server ohne EDS
# (tests/testdaten.py --kontakt-server). Danach UID-Prüfsumme je Buch wie vorher.
#   TEILE  Läufe, Standard „google outlook google-dunkel schreiben-google schreiben-outlook absender“ (zum Eingrenzen, z. B. TEILE=google-dunkel)
# Schreiben nur in diese beiden Bücher und (#370) „Persönlich“ (Briefing Zusatz M6), nur Testkontakte „Revolution-Test …“ über die
# gespeicherte UID bzw. Graph-ID; höchstens 20 Schreibvorgänge je Lauf und Buch (geprüft). Am Ende: kein Testkontakt
# übrig (--kontakt-live pruefen, Graph GET 404) und Kontaktzahlen wie vorher. Zeilen „OK …“/„FEHLER …“, Exit 0 ohne
# Fehler. #411: Abbruch eines Teilschritts zählt als FEHLER (tests/abbruch.sh).
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "$APP_DIR/tests/testkonten.sh"
BUILD="${BUILD:-$APP_DIR/build-devel}"
BIN="$BUILD/src/revolution"
OUT="${1:-$BUILD/m6-368}"
LAUF_TIMEOUT="${LAUF_TIMEOUT:-900}"
GOOGLE_BUCH=518e93226d14866ae0d8074ec0128a43c0c0c257
OUTLOOK_BUCH=3be9d2a4067fd00b72a3d3d5dbd529075a08ea88
FEHLER=0

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
for tool in xvfb-run gdbus python3; do command -v "$tool" >/dev/null || { echo "FEHLER  $tool fehlt"; exit 1; }; done
python3 -c 'import Xlib, PIL' 2>/dev/null || { echo "FEHLER  python3-xlib/python3-pil fehlt"; exit 1; }
python3 -c 'import gi; gi.require_version("Atspi", "2.0"); gi.require_version("EBook", "1.2")' 2>/dev/null \
  || { echo "FEHLER  gir1.2-atspi-2.0/gir1.2-ebook-1.2 fehlt"; exit 1; }
APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
bus_frei () { for _ in $(seq 30); do [ "$(bus_hat_app)" = "(false,)" ] && return 0; sleep 1; done; [ "$(bus_hat_app)" = "(false,)" ]; }
bus_frei || { echo "FEHLER  Session-Bus: $APP_ID läuft schon"; exit 1; }
mkdir -p "$OUT"
. "$APP_DIR/tests/abbruch.sh"

SCHEMA="$OUT/schema"
mkdir -p "$SCHEMA"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA/"
printf '[de.freiraum_medien.Revolution]\nwindow-width=1400\nwindow-height=900\n' >"$SCHEMA/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA" || { echo "FEHLER  Schema: glib-compile-schemas $SCHEMA"; exit 1; }

if td --adressbuecher >"$OUT/adressbuecher.txt" 2>&1; then
  echo "OK      Vergleich: $(grep -c '^ADRESSBUCH ' "$OUT/adressbuecher.txt") Adressbücher (tests/testdaten.py --adressbuecher)"
else
  echo "FEHLER  Vergleich: tests/testdaten.py --adressbuecher (siehe $OUT/adressbuecher.txt)"; exit 1
fi

lauf () {
  local name="$1" buch="$2" modus="$3" schema="$4" graph="$5" detail="$6" bildname="$7" rundir rc=0
  bus_frei || { echo "FEHLER  $name: Session-Bus: $APP_ID läuft schon"; return 1; }
  rundir="$(mktemp -d)"
  timeout -k 10 "$LAUF_TIMEOUT" env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK -u M365_DEBUG \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$SCHEMA" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    ADW_DEBUG_COLOR_SCHEME="$schema" G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals \
    REVOLUTION_BOOK="$buch" BIN="$BIN" OUT="$OUT" NAME="m6-368-$name" BUCH="$buch" MODUS="$modus" GRAPH="$graph" \
    VERGLEICH="$OUT/adressbuecher.txt" KONTAKT="$OUT/$name-kontakt.txt" GRAPH_DATEI="$OUT/$name-graph.txt" \
    DETAIL="$detail" DETAIL_DATEI="$OUT/$name-detail.txt" DETAIL_BILD="$bildname" \
    TESTDATEN="$APP_DIR/tests/testdaten.py" "${LAUF_ENV[@]}" \
    xvfb-run -a -s "-screen 0 1400x900x24" python3 -I "$APP_DIR/tests/kontakte-treiber.py" || rc=$?
  rm -rf "$rundir"
  for _ in $(seq 10); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  return "$rc"
}

# Aufräumen und Beleg: gespeicherter Testkontakt nicht mehr vorhanden (sonst jetzt löschen, zählt als FEHLER).
aufgeraeumt () {
  local name="$1" buch="$2" datei
  : >"$OUT/$name-aufraeumen.txt"
  for datei in "$OUT/$name-kontakt.txt" "$OUT/$name-detail.txt" "$OUT/$name-kontakt.txt.lokal"; do
    [ -s "$datei" ] || continue
    buch="$(cut -d' ' -f2 "$datei")"
    td --kontakt-live pruefen "$buch" "$datei" >>"$OUT/$name-aufraeumen.txt" 2>&1
    if [ "$(tail -1 "$OUT/$name-aufraeumen.txt" | cut -d' ' -f1-2)" = "VORHANDEN 0" ]; then
      echo "OK      $name: Testkontakt $(cut -d' ' -f3 "$datei") nicht mehr im Buch"
    else
      echo "FEHLER  $name: Testkontakt noch vorhanden, wird gelöscht: $(tail -1 "$OUT/$name-aufraeumen.txt")"; FEHLER=$((FEHLER + 1))
      td --kontakt-live loeschen "$buch" "$datei" >>"$OUT/$name-aufraeumen.txt" 2>&1
    fi
  done
  if [ -s "$OUT/$name-graph.txt" ] && ! grep -q ': WEG ' "$OUT/$name.txt"; then
    echo "FEHLER  $name: Graph-Kontakt nicht als gelöscht belegt, wird gelöscht"; FEHLER=$((FEHLER + 1))
    td --kontakt-graph loeschen "$OUT/$name-graph.txt" >>"$OUT/$name-aufraeumen.txt" 2>&1
  fi
  local n
  for b in "$GOOGLE_BUCH" "$OUTLOOK_BUCH" system-address-book; do
    n="$(grep -c "SCHREIBVORGANG $b" "$OUT/$name.txt")"
    if [ "$n" -le 20 ]; then echo "OK      $name: $n Schreibvorgänge in ${b:0:8} (≤ 20)"
    else echo "FEHLER  $name: $n Schreibvorgänge in ${b:0:8} (> 20)"; FEHLER=$((FEHLER + 1)); fi
  done
  n="$(grep -c 'SCHREIBVORGANG graph' "$OUT/$name.txt")"
  [ "$n" -le 20 ] || { echo "FEHLER  $name: $n Graph-Schreibvorgänge (> 20)"; FEHLER=$((FEHLER + 1)); }
}

teil () {
  local name="$1" buch="$2"; shift
  rm -f "$OUT/$name-kontakt.txt" "$OUT/$name-graph.txt" "$OUT/$name-detail.txt" "$OUT/$name-kontakt.txt.lokal"
  lauf "$name" "$@" | tee "$OUT/$name.txt"
  teilschritt "$name" "${PIPESTATUS[0]}" "$OUT/$name.txt"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$name.txt")))
  aufgeraeumt "$name" "$buch"
}

# #370: Nur-lesend-Buch mit den wenigsten Kontakten (Bearbeiten/Löschen nicht angeboten); Ordner der Testmail für
# „Absender übernehmen“ (Posteingang des zweiten IMAP-Testkontos, Betreff wie tests/anhaenge.sh).
NURLESEND_BUCH="$(awk '$1 == "ADRESSBUCH" && / nurlesend 1 initial 1 kontakte [1-9]/ { for (i = 1; i < NF; i++) if ($i == "kontakte") print $(i + 1), $2 }' \
  "$OUT/adressbuecher.txt" | sort -n | head -1 | cut -d' ' -f2)"
# Mail-Konten: Kontakte-Läufe laden keins (Anmeldefehler fremder Server sind nicht Teil des Tests), absender nur das
# zweite IMAP-Testkonto (wie tests/aktionen.sh konten).
read -r MAIL_UID ANDERE_MAILKONTEN < <(python3 -c '
import sys; sys.path.insert(0, sys.argv[1] + "/tests")
import testdaten as t
from gi.repository import EDataServer
r = EDataServer.SourceRegistry.new_sync(None)
uid = t.konten_finden(r)[sys.argv[2]].account.get_uid()
print(uid, ",".join(sorted(s.get_uid() for s in r.list_sources(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT) if s.get_uid() != uid)))' \
  "$APP_DIR" "$KONTO_IMAP_2")
ALLE_MAILKONTEN="$MAIL_UID,$ANDERE_MAILKONTEN"
LAUF_ENV=()
TEILE="${TEILE:-google outlook google-dunkel schreiben-google schreiben-outlook absender beenden}"
for t in $TEILE; do
  LAUF_ENV=()
  case "$t" in
    schreiben-google)
      LAUF_ENV=(ZIEL="$GOOGLE_BUCH" NURLESEND="$NURLESEND_BUCH" LOKAL=1 BILD_PREFIX=m6-370-hell-
                REVOLUTION_TEST_HOOKS=1 REVOLUTION_SKIP_ACCOUNTS="$ALLE_MAILKONTEN")
      teil schreiben-google system-address-book schreiben prefer-light 0 "" m6-370 ;;
    schreiben-outlook)
      LAUF_ENV=(ZIEL="$OUTLOOK_BUCH" BILD_PREFIX=m6-370-dunkel-
                REVOLUTION_TEST_HOOKS=1 REVOLUTION_SKIP_ACCOUNTS="$ALLE_MAILKONTEN")
      teil schreiben-outlook "$GOOGLE_BUCH" schreiben prefer-dark 0 "" m6-370 ;;
    beenden)
      LAUF_ENV=(REVOLUTION_TEST_HOOKS=1 REVOLUTION_SKIP_ACCOUNTS="$ALLE_MAILKONTEN")
      teil beenden "$GOOGLE_BUCH" beenden prefer-light 0 "" m6-370 ;;
    absender)
      # Nur das Konto der Testmail laden (Gelesen-Markierung schreibt; nur lesende Konten ausgeschlossen).
      LAUF_ENV=(REVOLUTION_FOLDER="$MAIL_UID/INBOX" REVOLUTION_MESSAGE="Anhänge PDF, Bild, ZIP"
                REVOLUTION_TEST_HOOKS=1 REVOLUTION_SKIP_ACCOUNTS="$ANDERE_MAILKONTEN")
      teil absender "$GOOGLE_BUCH" absender prefer-light 0 "" m6-370 ;;
    google) teil google "$GOOGLE_BUCH" alle prefer-light 0 voll m6-369-detail-hell ;;
    outlook) teil outlook "$OUTLOOK_BUCH" live prefer-dark 1 m365 m6-369-detail-m365 ;;
    google-dunkel) teil google-dunkel "$GOOGLE_BUCH" detail prefer-dark 0 anzeige m6-369-detail-dunkel ;;
    *) echo "FEHLER  TEILE: $t unbekannt"; FEHLER=$((FEHLER + 1)) ;;
  esac
done

td --adressbuecher >"$OUT/adressbuecher-nachher.txt" 2>&1
if diff -q <(grep '^ADRESSBUCH' "$OUT/adressbuecher.txt") <(grep '^ADRESSBUCH' "$OUT/adressbuecher-nachher.txt") >/dev/null; then
  echo "OK      Kontaktzahlen aller Bücher nach dem Test wie vorher (adressbuecher-nachher.txt)"
else
  echo "FEHLER  Kontaktzahlen geändert: $(diff <(grep '^ADRESSBUCH' "$OUT/adressbuecher.txt") <(grep '^ADRESSBUCH' "$OUT/adressbuecher-nachher.txt") | tr '\n' ' ')"
  FEHLER=$((FEHLER + 1))
fi

# #370: fremde Kontakte unverändert (UID-Prüfsumme je Buch, tests/testdaten.py --adressbuecher).
if diff -q <(grep '^UIDHASH' "$OUT/adressbuecher.txt") <(grep '^UIDHASH' "$OUT/adressbuecher-nachher.txt") >/dev/null; then
  echo "OK      UID-Prüfsummen aller Bücher nach dem Test wie vorher ($(grep -c '^UIDHASH' "$OUT/adressbuecher.txt") Bücher)"
else
  echo "FEHLER  UID-Prüfsummen geändert: $(diff <(grep '^UIDHASH' "$OUT/adressbuecher.txt") <(grep '^UIDHASH' "$OUT/adressbuecher-nachher.txt") | tr '\n' ' ')"
  FEHLER=$((FEHLER + 1))
fi

abbruch_bilanz

echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
