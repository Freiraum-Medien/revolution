#!/usr/bin/env bash
# Composer: Neu, Antworten, Allen antworten (#359), Weiterleiten, Anhänge, Entwürfe, Signatur, Identität (#360)
# gegen die Testkonten KONTO_IMAP_2@example.invalid (IMAP) und KONTO_M365@example.invalid (Graph).
#
# Aufruf: tests/composer.sh [AUSGABEORDNER]   (Standard BUILD/m5-359)
#   BUILD    Build-Ordner, Standard build/ im Repo (vorher meson compile)
#   LAEUFE   Auswahl der Läufe, Standard „hell dunkel a11y konversation eigen eigen-konversation
#            weiterleiten-hell weiterleiten-dunkel entwurf entwurf-imap entwurf-graph adressen-hell adressen-dunkel
#            antworten-imap allen-imap weiterleiten-imap antworten-graph allen-graph weiterleiten-graph fehler“
#   LAUF_TIMEOUT  Sekunden je Treiber-Lauf, Standard 600; TD_TIMEOUT je tests/testdaten.py, Standard 300
#
# Testmail: tests/testdaten.py --antwort (APPEND in den Posteingang von geops@, idempotent; mehrere
# Empfänger, „RE:“, References), Kopfzeilen des Originals unabhängig vom Server (--antwort-kopf, IMAP
# EXAMINE). Zwei App-Starts im eigenen Xvfb (echter Session-Bus, Einstellungen im Speicher, X11, flache
# Liste) mit REVOLUTION_FOLDER = Posteingang von geops@, REVOLUTION_MESSAGE = Betreff der Testmail,
# REVOLUTION_TEST_HOOKS=1 und REVOLUTION_TEST_MIME: Senden (Strg+Enter) schreibt die Nachricht in diese
# Datei statt zu senden (#362), nichts geht an einen Server. Theme hell (REVOLUTION_THEME=hell) und
# dunkel (dunkel). Der Treiber tests/composer-treiber.py bedient die App per XTest und prüft: Strg+R und
# Strg+Umschalt+R (MIME: An/Kopie ohne eigene Adressen, „Re:“, In-Reply-To, References, Zitat als
# blockquote, Zuschreibung, multipart/alternative), Strg+N (Absender, Eingabe, Strg+B), Escape schließt.
# Dritter Start (m5-359-a11y) ohne GTK_A11Y=none: a11y-Namen aller Felder und Knöpfe über AT-SPI (Bus
# org.a11y.Bus der Sitzung), Escape mit Fokus im Editor. Vierter Start (m5-359-konversation) in der
# Konversationsansicht (Testdaten --konversation, #357): Strg+R antwortet auf die neueste Nachricht (Nr. 5).
# R2, Antwort auf eigene Mail (Evolution em-composer-utils.c:5127-5131, 5149-5151): m5-359-eigen (flach, eigene
# Mail im Posteingang mit „AW:“) und m5-359-eigen-konversation (eigene Antwort aus Gesendet als neueste):
# Strg+R wird Allen antworten, ohne eigene Adressen.
# Screenshots des Composers je Theme.
# #360 (Testdaten: Anhang-Mail tests/testdaten.py --anhang, ihre Teile vom Server --anhang-teile; Konten ohne
# Schreibfreigabe per REVOLUTION_SKIP_ACCOUNTS aus): m5-360-weiterleiten-hell/-dunkel: Strg+Alt+F auf die
# Anhang-Mail, Strg+M fügt zwei Dateien an (REVOLUTION_TEST_ATTACH statt Dateidialog), eine wird entfernt, Signatur
# GEOPS; MIME: multipart/mixed, message/rfc822 mit Aufbau und Anhang-Bytes wie am Server, Datei-Bytes gleich,
# X-Evolution-Identity. m5-360-entwurf: Autosave (REVOLUTION_TEST_AUTOSAVE=10) in geops@, danach Identität
# outlook.com und zweimal Strg+S; je genau ein Entwurf am Server (--entwurf). m5-360-entwurf-imap/-graph: im
# Entwürfe-Ordner (aus dem Log von m5-360-entwurf) Strg+O, Strg+S ersetzt, Senden (Test) löscht den Entwurf.
# Betreff der Entwürfe je Aufruf eindeutig (ENTWURF_LAUF); danach --entwurf KONTO BETREFF weg MESSAGE-IDs: nur die
# Entwürfe, die die App laut Log gespeichert hat (UID EXPUNGE bzw. Graph permanentDelete genau dieser).
# #432 Senden über den Postausgang (REVOLUTION_TEST_SEND=1, die MIME-Datei ist dann nur eine Kopie; eigener
# Postausgang XDG_DATA_HOME=AUSGABEORDNER/daten): m5-360-entwurf-imap/-graph senden wirklich (geops@ → privat@,
# outlook.com → geops@). m5-432-antworten/allen/weiterleiten-imap|-graph: je Lauf ein eigenes ungelesenes Original
# im Posteingang (tests/testdaten.py --original, Absender und Empfänger nur Testkonten), Strg+R, Strg+Umschalt+R
# bzw. Strg+Alt+F, Strg+Enter; bei „antworten“ dazu Strg+N mit Strg+S (Entwurf) und Senden. m5-432-fehler: Postausgang
# nicht nutzbar (Datei statt .Outbox): Meldung, Fenster bleibt offen, Senden wieder möglich. Danach je gesendeter
# Nachricht (OUT/gesendet.txt) testdaten.py --zustellung: genau 1× beim Empfänger, 1× in Gesendet; Flags der Originale
# vom Server (--original … flags); Aufräumen nur der eigenen Message-IDs (--aufraeumen, --original … aufraeumen).
# #361: m5-361-adressen-hell/-dunkel: Adressvervollständigung mit dem Testkontakt im Google-Adressbuch von
# KONTO_GOOGLE@example.invalid (tests/testdaten.py --kontakt anlegen vorher, --kontakt weg nachher).
# Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“; Exit 0 ohne Fehler (tests/abbruch.sh).
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/src/revolution"
OUT="$(realpath -m "${1:-$BUILD/m5-359}")"
LAUF_TIMEOUT="${LAUF_TIMEOUT:-600}"
FEHLER=0

[ -x "$BIN" ] || { echo "FEHLER  Build: $BIN fehlt (meson compile)"; exit 1; }
for tool in xvfb-run gdbus python3; do command -v "$tool" >/dev/null || { echo "FEHLER  $tool fehlt"; exit 1; }; done
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

SCHEMA="$OUT/schema"
mkdir -p "$SCHEMA"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA/"
printf '[de.freiraum_medien.Revolution]\nconversations=false\nmark-seen=false\n' >"$SCHEMA/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA" || { echo "FEHLER  Schema: glib-compile-schemas $SCHEMA"; exit 1; }
# Lauf m5-359-konversation: Antwort in der Konversationsansicht (Testdaten --konversation, #357).
SCHEMA_K="$OUT/schema-konversation"
mkdir -p "$SCHEMA_K"
cp "$APP_DIR/data/"*.gschema.xml "$SCHEMA_K/"
printf '[de.freiraum_medien.Revolution]\nconversations=true\nmark-seen=false\n' >"$SCHEMA_K/90_test.gschema.override"
glib-compile-schemas --strict "$SCHEMA_K" || { echo "FEHLER  Schema: glib-compile-schemas $SCHEMA_K"; exit 1; }

td --antwort >"$OUT/antwort.txt" 2>&1 || { cat "$OUT/antwort.txt"; abbruch_bilanz; exit 1; }
td --antwort-kopf >"$OUT/original-kopf.txt" 2>&1 || { cat "$OUT/original-kopf.txt"; abbruch_bilanz; exit 1; }
ORDNER="$(sed -n 's/^ANTWORT \([^ ]*\) .*/\1/p' "$OUT/antwort.txt")"
BETREFF="$(sed -n 's/^KOPF Subject: //p' "$OUT/original-kopf.txt")"

# #360: Testdaten und Aufräumen (nur Test-Entwürfe der zwei Konten).
case " ${LAEUFE:-weiterleiten} " in
  *" weiterleiten"*|*" entwurf"*|*" antworten-"*|*" allen-"*|*" fehler"*)
    td --anhang >"$OUT/anhang.txt" 2>&1 || { cat "$OUT/anhang.txt"; abbruch_bilanz; exit 1; }
    td --anhang-teile KONTO_IMAP_2@example.invalid "$OUT/teile" >"$OUT/teile.txt" 2>&1 || { cat "$OUT/teile.txt"; abbruch_bilanz; exit 1; }
    head -c 4096 /dev/urandom >"$OUT/anhang-360.bin"
    cp "$APP_DIR/src/icons/scalable/actions/drafts-symbolic.svg" "$OUT/anhang-360.svg"
    # Konten ohne Schreibfreigabe (Briefing M5) und alle nicht benötigten ausschließen.
    OHNE="$(python3 -c '
import gi; gi.require_version("EDataServer", "1.2"); from gi.repository import EDataServer as E
r = E.SourceRegistry.new_sync(None)
for s in r.list_sources(E.SOURCE_EXTENSION_MAIL_ACCOUNT):
    i = r.ref_source(s.get_extension(E.SOURCE_EXTENSION_MAIL_ACCOUNT).get_identity_uid() or "")
    a = i.get_extension(E.SOURCE_EXTENSION_MAIL_IDENTITY).get_address() if i and i.has_extension(E.SOURCE_EXTENSION_MAIL_IDENTITY) else ""
    if a and a not in ("KONTO_IMAP_2@example.invalid", "KONTO_M365@example.invalid"): print(s.get_uid())' | paste -sd,)"
    echo "WERT    REVOLUTION_SKIP_ACCOUNTS=$OHNE" ;;
esac
# #361: Testkontakt (nur Google-Adressbuch des Testkontos, Name „Revolution-Test …“).
ADRESSBUCH=""
case " ${LAEUFE:-adressen} " in
  *" adressen"*)
    td --kontakt anlegen >"$OUT/kontakt.txt" 2>&1 || { cat "$OUT/kontakt.txt"; abbruch_bilanz; exit 1; }
    ADRESSBUCH="$(sed -n 's/^ADRESSBUCH \([^ ]*\) .*/\1/p' "$OUT/kontakt.txt")" ;;
esac
ANHANG_BETREFF="Anhänge PDF, Bild, ZIP"
ENTWURF_ORDNER_IMAP=""
# Betreff der Entwürfe je Aufruf eindeutig (Review R2: kein Aufräumen über fremde Betreffe, parallele Agenten).
ENTWURF_LAUF="$(date +%Y%m%d%H%M%S)-$$"
ENTWURF_ORDNER_GRAPH=""
# #432: Konto-UIDs (Posteingang: imapx „INBOX“, microsoft365 „Posteingang“) und Originale dieses Aufrufs.
read -r UID_GEOPS UID_OUTLOOK < <(PYTHONPATH="$APP_DIR/tests" timeout 60 python3 -c 'import testdaten as t
from gi.repository import EDataServer as E
k = t.konten_finden(E.SourceRegistry.new_sync(None))
print(k["KONTO_IMAP_2@example.invalid"].account.get_uid(), k["KONTO_M365@example.invalid"].account.get_uid())')
case " ${LAEUFE:-antworten-} " in
  *" antworten-"*|*" allen-"*|*" weiterleiten-imap"*|*" weiterleiten-graph"*)
    td --original KONTO_IMAP_2@example.invalid "$ENTWURF_LAUF" anlegen >"$OUT/originale-geops.txt" 2>&1 || { cat "$OUT/originale-geops.txt"; abbruch_bilanz; exit 1; }
    td --original KONTO_M365@example.invalid "$ENTWURF_LAUF" anlegen >"$OUT/originale-arndmesser.txt" 2>&1 ||
      { cat "$OUT/originale-arndmesser.txt"; abbruch_bilanz; exit 1; }
    grep '^ORIGINAL' "$OUT"/originale-*.txt | cut -c1-160 ;;
esac
: >"$OUT/gesendet.txt"

art () { case "$1" in *-antworten-*) echo Antworten ;; *-allen-*) echo Allen ;; *) echo Weiterleiten ;; esac; }

lauf () {
  local name="$1" theme="$2" schema="$3" a11y="$4" rundir rc=0 einstellungen="$SCHEMA" betreff="$BETREFF" ordner="$ORDNER"
  local extra=()
  case "$name" in
    m5-359-konversation) einstellungen="$SCHEMA_K"; betreff="Konversation Marktstand" ;;
    m5-359-eigen-konversation) einstellungen="$SCHEMA_K"; betreff="Eigene Antwort Kisten" ;;
    m5-359-eigen) betreff="Eigene Mail im Eingang" ;;
    m5-360-weiterleiten-*) betreff="$ANHANG_BETREFF" ;;
    m5-360-entwurf-imap) ordner="$ENTWURF_ORDNER_IMAP"; betreff="Entwurf 360 $ENTWURF_LAUF IMAP" ;;
    m5-360-entwurf-graph) ordner="$ENTWURF_ORDNER_GRAPH"; betreff="Entwurf 360 $ENTWURF_LAUF Graph" ;;
    m5-432-*-imap) ordner="$UID_GEOPS/INBOX"; betreff="432 $ENTWURF_LAUF $(art "$name")" ;;
    m5-432-*-graph) ordner="$UID_OUTLOOK/Posteingang"; betreff="432 $ENTWURF_LAUF $(art "$name")" ;;
  esac
  case "$name" in
    m5-360-*) extra=(REVOLUTION_SKIP_ACCOUNTS="$OHNE" REVOLUTION_TEST_AUTOSAVE=10 TEILE="$OUT/teile.txt" ENTWURF_LAUF="$ENTWURF_LAUF"
                     REVOLUTION_TEST_ATTACH="$OUT/anhang-360.bin:$OUT/anhang-360.svg") ;;
  esac
  case "$name" in
    m5-360-entwurf-imap|m5-360-entwurf-graph|m5-432-*)
      extra+=(REVOLUTION_SKIP_ACCOUNTS="$OHNE" REVOLUTION_TEST_SEND=1 XDG_DATA_HOME="$OUT/daten" ENTWURF_LAUF="$ENTWURF_LAUF"
             ORIGINALE="$OUT/originale-$([ "${name%-graph}" != "$name" ] && echo arndmesser || echo geops).txt")
      [ "$name" = m5-432-fehler ] && extra+=(XDG_DATA_HOME="$OUT/daten-fehler")
      [ "${name#m5-432-}" != "$name" ] && [ "${name%-graph}" != "$name" ] && extra+=(M365_DEBUG=1) ;;
  esac
  # #360: Graph-Anfragen beim Speichern in outlook.com (Echo-Schutz #416); der Treiber filtert Kopfzeilen und Token.
  [ "$name" = m5-360-entwurf ] && extra+=(M365_DEBUG=1)
  [ -n "$ordner" ] || { echo "FEHLER  $name: Entwürfe-Ordner unbekannt (Lauf m5-360-entwurf)"; return 1; }
  # Innerhalb der run.lock: die vorige Instanz gibt den Namen verzögert frei (bis 30 s).
  for _ in $(seq 30); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || { echo "FEHLER  $name: $APP_ID läuft schon"; return 1; }
  rundir="$(mktemp -d)"
  timeout -k 10 "$LAUF_TIMEOUT" env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK \
    XDG_RUNTIME_DIR="$rundir" GSETTINGS_SCHEMA_DIR="$einstellungen" GSETTINGS_BACKEND=memory \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo \
    ADW_DEBUG_COLOR_SCHEME="$schema" REVOLUTION_THEME="$theme" ${a11y:+GTK_A11Y=none NO_AT_BRIDGE=1} \
    G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals REVOLUTION_FOLDER="$ordner" REVOLUTION_MESSAGE="$betreff" \
    "${extra[@]}" \
    REVOLUTION_TEST_HOOKS=1 REVOLUTION_TEST_MIME="$OUT/$name-senden.eml" \
    ADRESSBUCH="$ADRESSBUCH" BIN="$BIN" OUT="$OUT" NAME="$name" ORDNER="$ORDNER" KOPF="$OUT/original-kopf.txt" APP_DIR="$APP_DIR" \
    xvfb-run -a -s "-screen 0 1200x760x24" python3 -I "$APP_DIR/tests/composer-treiber.py" || rc=$?
  rm -rf "$rundir"
  for _ in $(seq 10); do [ "$(bus_hat_app)" = "(false,)" ] && break; sleep 1; done
  [ "$(bus_hat_app)" = "(false,)" ] || echo "FEHLER  $name: App nach dem Lauf noch auf dem Bus"
  return "$rc"
}

for lauf in "m5-359-hell hell prefer-light aus" "m5-359-dunkel dunkel prefer-dark aus" "m5-359-a11y hell prefer-light" \
           "m5-359-konversation hell prefer-light aus" "m5-359-eigen hell prefer-light aus" \
           "m5-359-eigen-konversation hell prefer-light aus" "m5-360-weiterleiten-hell hell prefer-light" \
           "m5-360-weiterleiten-dunkel dunkel prefer-dark" "m5-360-entwurf hell prefer-light" \
           "m5-360-entwurf-imap hell prefer-light aus" "m5-360-entwurf-graph hell prefer-light aus" \
           "m5-432-antworten-imap hell prefer-light aus" "m5-432-allen-imap hell prefer-light aus" \
           "m5-432-weiterleiten-imap hell prefer-light aus" "m5-432-antworten-graph hell prefer-light aus" \
           "m5-432-allen-graph hell prefer-light aus" "m5-432-weiterleiten-graph hell prefer-light aus" \
           "m5-432-fehler hell prefer-light aus" \
           "m5-361-adressen-hell hell prefer-light" "m5-361-adressen-dunkel dunkel prefer-dark"; do
  set -- $lauf
  case " ${LAEUFE:-hell dunkel a11y konversation eigen eigen-konversation weiterleiten-hell weiterleiten-dunkel entwurf entwurf-imap entwurf-graph adressen-hell adressen-dunkel antworten-imap allen-imap weiterleiten-imap antworten-graph allen-graph weiterleiten-graph fehler} " in
    *" ${1#m5-[34][0-9][0-9]-} "*) ;; *) continue ;; esac
  # #432: Postausgang nicht nutzbar: Datei an der Stelle des Maildir-Ordners .Outbox (mail-outbox.c).
  if [ "$1" = m5-432-fehler ]; then
    rm -rf "$OUT/daten-fehler"; mkdir -p "$OUT/daten-fehler/revolution/mail/local"; : >"$OUT/daten-fehler/revolution/mail/local/.Outbox"
  fi
  lauf "$1" "$2" "$3" "${4:-}" | tee "$OUT/$1.txt"
  teilschritt "$1" "${PIPESTATUS[0]}" "$OUT/$1.txt"
  FEHLER=$((FEHLER + $(grep -c '^FEHLER' "$OUT/$1.txt")))
  # #360: Entwürfe-Ordner (Store-UID/voller Name) aus „Entwurf: gespeichert folder://UID/NAME UID“.
  if [ "$1" = m5-360-entwurf ]; then
    ENTWURF_ORDNER_IMAP="$(sed -n 's|.*Entwurf: gespeichert folder://\(cae[^/]*\)/\([^ ]*\) .*|\1/\2|p' "$OUT/$1.log" | tail -1 |
      python3 -c 'import sys, urllib.parse; print(urllib.parse.unquote(sys.stdin.read().strip()))')"
    ENTWURF_ORDNER_GRAPH="$(sed -n 's|.*Entwurf: gespeichert folder://\(aed[^/]*\)/\([^ ]*\) .*|\1/\2|p' "$OUT/$1.log" | tail -1 |
      python3 -c 'import sys, urllib.parse; print(urllib.parse.unquote(sys.stdin.read().strip()))')"
    echo "WERT    Entwürfe-Ordner: $ENTWURF_ORDNER_IMAP $ENTWURF_ORDNER_GRAPH"
  fi
done

# Aufräumen: nur die Entwürfe, die die App in diesem Aufruf gespeichert hat (Message-IDs aus „Entwurf: gespeichert
# <Ordner> <UID> <Message-ID>“ aller Logs), danach Prüfung über den Betreff dieses Aufrufs.
MIDS="$(cat "$OUT"/*.log 2>/dev/null | sed -n 's/.*Entwurf: gespeichert [^ ]* [^ ]* \(<[^>]*>\).*/\1/p' | sort -u)"
if [ -n "$MIDS" ]; then
  echo "WERT    eigene Entwürfe: $(wc -l <<<"$MIDS") Message-IDs"
  : >"$OUT/aufraeumen-pruefung.txt"
  for konto in KONTO_IMAP_2@example.invalid KONTO_M365@example.invalid; do
    art=IMAP; [ "$konto" = KONTO_M365@example.invalid ] && art=Graph
    td --entwurf "$konto" "[Revolution-Test] Entwurf 360 $ENTWURF_LAUF $art" weg $MIDS >>"$OUT/aufraeumen.txt" 2>&1
    td --entwurf "$konto" "[Revolution-Test] Entwurf 360 $ENTWURF_LAUF $art" | tee -a "$OUT/aufraeumen-pruefung.txt"
  done
  grep -q '^ENTWUERFE [1-9]\|^ENTWUERFE [0-9]* [1-9]' "$OUT/aufraeumen-pruefung.txt" &&
    { echo "FEHLER  Aufräumen: Test-Entwürfe dieses Laufs am Server übrig"; FEHLER=$((FEHLER + 1)); }
fi

# #432: Zustellung je gesendeter Nachricht (bis 5 min warten), dann genau 1× beim Empfänger und 1× in Gesendet.
anzahl () {  # MESSAGE-ID KONTO ROLLE
  local n=0
  for _ in $(seq 30); do
    n="$(td --zustellung "$2" "$1" | awk -v r="$3" '$1 == "ZUSTELLUNG" && $3 == r { print $4 }')"
    [ "${n:-0}" -ge 1 ] && break
    sleep 10
  done
  echo "${n:-0}"
}
pruefe_zahl () {  # NAME TEXT IST
  if [ "$3" = 1 ]; then echo "OK      $1: $2 1×"; else echo "FEHLER  $1: $2 ${3}× (erwartet 1)"; FEHLER=$((FEHLER + 1)); fi
}
while read -r name mid von empf; do
  for konto in $empf; do
    case " KONTO_IMAP_1@example.invalid KONTO_IMAP_2@example.invalid KONTO_M365@example.invalid KONTO_GOOGLE@example.invalid " in
      *" $konto "*) pruefe_zahl "$name" "$mid beim Empfänger $konto (Posteingang)" "$(anzahl "$mid" "$konto" eingang)" ;;
      *) echo "FEHLER  $name: Empfänger $konto ist kein Testkonto"; FEHLER=$((FEHLER + 1)) ;;
    esac
  done
  pruefe_zahl "$name" "$mid in Gesendet von $von" "$(anzahl "$mid" "$von" gesendet)"
done < <(tee "$OUT/zustellung-liste.txt" <"$OUT/gesendet.txt")

# #432: Flags der Originale vom Server. IMAP: \Answered (+ \Seen) nach Antworten/Allen antworten; FORWARDED kennt
# imapx nicht als IMAP-Flag (EDS camel-imapx-utils.c:83-94), nur lokal. Graph kennt ANSWERED/FORWARDED nicht
# (evolution-ews camel-m365-folder.c:663-664), am Server sichtbar nur isRead (SEEN).
flagzeile () { sed -n "s/^FLAGS $2 //p" "$OUT/originale-flags-$1.txt"; }
for konto in KONTO_IMAP_2@example.invalid KONTO_M365@example.invalid; do
  k="${konto%%@*}"
  [ -s "$OUT/originale-$k.txt" ] || continue
  td --original "$konto" "$ENTWURF_LAUF" flags >"$OUT/originale-flags-$k.txt" 2>&1
  sed 's/^/WERT    Original '"$konto"': /' "$OUT/originale-flags-$k.txt"
  art=imap; [ "$k" = arndmesser ] && art=graph
  for a in Antworten Allen Weiterleiten; do
    grep -q "^m5-432-$(tr '[:upper:]' '[:lower:]' <<<"$a")-$art " "$OUT/gesendet.txt" || continue
    f="$(flagzeile "$k" "$a")"
    case "$art/$a" in
      imap/Weiterleiten) soll='ohne \Answered und \Seen'; grep -Eq 'Answered|Seen' <<<"$f" && soll="$soll!" ;;
      imap/*) soll='\Answered und \Seen'; { grep -q 'Answered' <<<"$f" && grep -q 'Seen' <<<"$f"; } || soll="$soll!" ;;
      graph/Weiterleiten) soll='isRead=False'; grep -q 'isRead=False' <<<"$f" || soll="$soll!" ;;
      graph/*) soll='isRead=True'; grep -q 'isRead=True' <<<"$f" || soll="$soll!" ;;
    esac
    if [ "${soll%!}" = "$soll" ]; then echo "OK      Original $konto $a am Server: $f"
    else echo "FEHLER  Original $konto $a am Server: „$f“ (erwartet ${soll%!})"; FEHLER=$((FEHLER + 1)); fi
  done
done

# #432: Aufräumen nur eigener Nachrichten (Message-IDs aus gesendet.txt bzw. der Originale), Daten-Ordner.
MIDS_GESENDET="$(awk '{ print $2 }' "$OUT/gesendet.txt" | sort -u)"
if [ -n "$MIDS_GESENDET" ]; then
  for konto in KONTO_IMAP_2@example.invalid KONTO_IMAP_1@example.invalid KONTO_M365@example.invalid; do
    td --aufraeumen "$konto" $MIDS_GESENDET | tee -a "$OUT/aufraeumen-gesendet.txt"
  done
fi
for konto in KONTO_IMAP_2@example.invalid KONTO_M365@example.invalid; do
  [ -s "$OUT/originale-${konto%%@*}.txt" ] && td --original "$konto" "$ENTWURF_LAUF" aufraeumen >>"$OUT/aufraeumen-originale.txt" 2>&1
done
for b in "432 IMAP" "432 Graph" "432 Fehler"; do
  for konto in KONTO_IMAP_2@example.invalid KONTO_M365@example.invalid; do
    [ -n "$MIDS" ] && td --entwurf "$konto" "[Revolution-Test] Entwurf 360 $ENTWURF_LAUF $b" | grep '^ENTWUERFE [1-9]' &&
      { echo "FEHLER  Aufräumen: Entwürfe „$b“ in $konto übrig"; FEHLER=$((FEHLER + 1)); }
  done
done
rm -rf "$OUT/daten" "$OUT/daten-fehler"

case " ${LAEUFE:-adressen} " in
  *" adressen"*)
    td --kontakt weg >"$OUT/kontakt-weg.txt" 2>&1
    td --kontakt | tee "$OUT/kontakt-pruefung.txt"
    grep -q '^KONTAKT ' "$OUT/kontakt-pruefung.txt" &&
      { echo "FEHLER  Aufräumen: Testkontakt übrig"; FEHLER=$((FEHLER + 1)); } ;;
esac

abbruch_bilanz
echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
