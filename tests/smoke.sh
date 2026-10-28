#!/usr/bin/env bash
# Headless-Smoke-Test M3 (#349) gegen die echten Konten aus GNOME Online Accounts.
#
# Aufruf: tests/smoke.sh [AUSGABEORDNER]   bzw.   meson test -C build --suite smoke -v
#   AUSGABEORDNER  Logs und PNGs, Standard SMOKE_OUT bzw. BUILD/smoke
#   BUILD          Build-Ordner, Standard build/ im Repo
#   BUILD_LOCK     Sperrdatei für meson compile (Standard BUILD/.build.lock); ohne meson test
#                  baut das Skript vorher unter dieser Sperre (flock)
#   SMOKE_TIMEOUT  Sekunden je App-Start, bis alle Konten angemeldet und Ordner geladen sind
#                  (Standard 120)
#
# Ablauf: vier App-Starts über scripts/screenshot.sh (eigener Xvfb, echter Session-Bus, keine
# eigene dbus-run-session, Einstellungen im Speicher): Mail hell und Mail dunkel (1200x4800, alle
# Konten und Ordner im Bild), Kalender (Strg+2) und Kontakte (Strg+3). Je Start wartet das Skript auf die Logzeilen der App
# (G_MESSAGES_DEBUG=revolution), nimmt dann den Screenshot auf und schließt das Fenster per
# WM_DELETE_WINDOW. Prüfungen je Start:
#   (a) Konten: angezeigte Konten (Log „Mail-Konto … angezeigt“) = Regel aus mail-account.c,
#       unabhängig in Python-GI gegen die ESourceRegistry ausgewertet
#   (b) Anmeldung: je angezeigtem Konto „angemeldet: UID … angenommen“
#   (c) Ordner: Log „Ordner: UID Name“ = tests/testdaten.py --ordner (nur lesend), je Konto
#   (d) Log: nur DEBUG-Zeilen der App (Positivliste, G_DEBUG=fatal-criticals bricht bei
#       CRITICAL ab)
#   (e) Beenden: Exit 0 nach WM_DELETE_WINDOW, kein Rest-Prozess (App, Xvfb, Helfer), App-ID
#       nicht mehr auf dem Bus
# Am Ende (f): PIDs von evolution-source-registry und goa-daemon sowie sha256, Änderungszeit und
# Inode in ~/.config/evolution/sources unverändert (keine zweite Registry, nichts neu geschrieben, #343).
# Schreibt nichts auf Server: die App lädt in M3 nur Ordnerlisten, testdaten.py --ordner liest.
# Eine Zeile je Prüfung „OK …“ bzw. „FEHLER …“; Exit 0 nur, wenn alle Prüfungen OK sind.
# #411: Jeder Abbruch eines Teilschritts (Traceback, Exit ≠ 0 von testdaten.py, Zeitlimit) ist ein
# FEHLER (tests/abbruch.sh); jeder App-Start mit Zeitlimit SMOKE_TIMEOUT + 120 s.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

# Ohne pipefail: grep -q beendet Pipes früh, der Schreiber bekäme SIGPIPE und die Pipe
# zählte als Fehlschlag.
set -u

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/src/revolution"
OUT="${1:-${SMOKE_OUT:-$BUILD/smoke}}"
TIMEOUT="${SMOKE_TIMEOUT:-120}"
SOURCES="$HOME/.config/evolution/sources"
FEHLER=0

ok () { echo "OK      $*"; }
fehler () { echo "FEHLER  $*"; FEHLER=$((FEHLER + 1)); }

# Rauschen von Mesa unter Xvfb (kein DRI3, GTK fragt beim Start EGL ab, obwohl screenshot.sh
# GSK_RENDERER=cairo setzt). Nur genau diese zwei Zeilen werden übergangen, jede andere Warnung
# zählt (Prüfung d).
EGL_RAUSCHEN='libEGL warning: DRI3 error: Could not get DRI3 device
libEGL warning: Ensure your X server supports DRI3 to get accelerated rendering'

[[ "$TIMEOUT" =~ ^[0-9]+$ ]] || { echo "SMOKE_TIMEOUT als Sekunden angeben" >&2; exit 2; }
mkdir -p "$OUT"
. "$APP_DIR/tests/abbruch.sh"
LAEUFE="mail-hell mail-dunkel kalender-hell kontakte-hell"
# Nur eigene Dateien früherer Läufe entfernen (OUT kann ein Ordner mit fremden Dateien sein).
for n in $LAEUFE; do rm -f "$OUT/$n".{log,out,png,ready,quit} "$OUT/$n"-*; done
rm -f "$OUT"/{build,konten-registry,ordner-testdaten,sources-vorher,sources-nachher}.txt "$OUT/konten-registry.err"

# ---------------------------------------------------------------- Vorbedingungen

if [ ! -f "$BUILD/build.ninja" ]; then
  fehler "Build: $BUILD ist kein Build-Ordner (meson setup)"; exit 1
fi
if [ -z "${MESON_TEST_ITERATION:-}" ]; then
  # Unter meson test hat meson schon gebaut; ein zweiter Bau dort wartete ggf. auf die Sperre.
  flock "${BUILD_LOCK:-$BUILD/.build.lock}" meson compile -C "$BUILD" >"$OUT/build.txt" 2>&1 \
    || { fehler "Build: meson compile (siehe $OUT/build.txt)"; exit 1; }
fi
if [ -x "$BIN" ]; then ok "Build: $BIN"; else fehler "Build: $BIN fehlt"; exit 1; fi

APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
bus_hat_app () {
  gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
    -m org.freedesktop.DBus.NameHasOwner "$APP_ID" 2>/dev/null
}
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then fehler "Session-Bus: DBUS_SESSION_BUS_ADDRESS leer"; exit 1; fi
if [ "$(bus_hat_app)" = "(false,)" ]; then
  ok "Session-Bus: NameHasOwner $APP_ID = false"
else
  fehler "Session-Bus: $APP_ID läuft schon oder Bus nicht erreichbar ($(bus_hat_app))"; exit 1
fi

# ---------------------------------------------------------------- Zustand vorher, Vergleichsdaten

pids () {
  echo "evolution-source-registry $(pgrep -d, -f '^/usr/libexec/evolution-source-registry') goa-daemon $(pgrep -d, -f '^/usr/libexec/goa-daemon')"
}
PIDS_VORHER="$(pids)"
# Inhalt (sha256) und Datei-Kennung (Änderungszeit, Inode): auch ein Neuschreiben mit gleichem
# Inhalt fiele auf.
quellen () { sha256sum "$SOURCES"/*; stat -c '%n %Y %i' "$SOURCES"/*; }
quellen >"$OUT/sources-vorher.txt" 2>&1

# Regel aus src/mail/mail-account.c (revolution_mail_account_hidden_reason), hier unabhängig
# über Python-GI: eingebaute Speicher nie; Online-Konto (GOA/UOA) mit Mail aus nie; sonst
# „enabled“ der Collection bzw. ohne Collection der Quelle.
python3 -I - >"$OUT/konten-registry.txt" 2>"$OUT/konten-registry.err" <<'EOF'
import os
import gi
gi.require_version('EDataServer', '1.2')
from gi.repository import EDataServer as E

# #415: Test-Hook der App (mail-account.c): Konten aus REVOLUTION_SKIP_ACCOUNTS nie.
skip = os.environ.get('REVOLUTION_SKIP_ACCOUNTS', '').split(',') if os.environ.get('REVOLUTION_TEST_HOOKS') == '1' else []
r = E.SourceRegistry.new_sync(None)
for s in r.list_sources(E.SOURCE_EXTENSION_MAIL_ACCOUNT):
    if s.get_uid() in ('local', 'vfolder', 'rss') or s.get_uid() in skip:
        continue
    online = r.find_extension(s, E.SOURCE_EXTENSION_GOA) or r.find_extension(s, E.SOURCE_EXTENSION_UOA)
    if online and not s.get_enabled():
        continue
    coll = r.find_extension(s, E.SOURCE_EXTENSION_COLLECTION)
    if (coll or s).get_enabled():
        print(s.get_uid(), s.get_display_name())
EOF
[ $? -eq 0 ] || fehler "Registry: Python-GI-Regel (siehe $OUT/konten-registry.err)"
KONTEN="$(cut -d' ' -f1 "$OUT/konten-registry.txt" | sort)"

if td --ordner >"$OUT/ordner-testdaten.txt" 2>&1; then
  ok "Vergleichsliste: tests/testdaten.py --ordner, $(grep -c '^ANZAHL ' "$OUT/ordner-testdaten.txt") Konten"
else
  echo "        Vergleichsliste fehlt (siehe $OUT/ordner-testdaten.txt)"
fi

# ---------------------------------------------------------------- Log der App auswerten

# Zeilen der App ohne Präfix „(revolution:PID): revolution-DEBUG: HH:MM:SS.mmm: “.
debug () { sed -nE 's/^\(revolution:[0-9]+\): revolution-DEBUG: [0-9:.]+: //p' "$1"; }

# Fertig je Konto: on_store_noted (src/main.c) schreibt in jedem Zweig zuletzt eine dieser
# Zeilen; sie kommen also auch bei Fehlern (z. B. ohne Netz), dann ohne Wartezeit.
fertig () {
  local log="$1" uid
  local shown
  shown="$(debug "$log" | sed -nE 's/^Mail-Konto ([^ ]+) .*: angezeigt$/\1/p' | sort -u)"
  debug "$log" | grep -q '^Seitenleiste: ' || return 1
  for uid in $shown; do
    debug "$log" | grep -qE "^Ordner: $uid ([0-9]+ geladen|nicht geladen: |nicht mehr angezeigt)" \
      || grep -qF "($uid): " "$log" || return 1
  done
}

warte () {
  local log="$1" ready="$2"
  for _ in $(seq "$TIMEOUT"); do
    if [ -f "$log" ] && fertig "$log"; then echo fertig >"$ready"; return; fi
    sleep 1
  done
  echo Zeitlimit >"$ready"
}

# Letzter Stand je Konto aus „Mail-Konto UID „Name“: Grund“ (die Seitenleiste gleicht bei
# jedem Registry-Signal neu ab).
angezeigt () {
  debug "$1" | sed -nE 's/^Mail-Konto ([^ ]+) .*: ([^:]+)$/\1 \2/p' \
    | awk '{ uid = $1; sub(/^[^ ]+ /, ""); stand[uid] = $0 } END { for (u in stand) if (stand[u] == "angezeigt") print u }' \
    | sort
}

pruefe_lauf () {
  local name="$1" log="$OUT/$1.log" quit="$OUT/$1.quit" uid fehlt rest pid rc n_app n_ref disp p

  # Anzeige: screenshot.sh bricht ab, wenn DISPLAY die echte Sitzung wäre oder WAYLAND_DISPLAY
  # gesetzt ist; sonst nennt es den eigenen Xvfb.
  if grep -q '^Xvfb-Anzeige: ' "$OUT/$name.out"; then
    ok "$name Anzeige: $(grep '^Xvfb-Anzeige: ' "$OUT/$name.out" | sed 's/^Xvfb-Anzeige: //'), kein WAYLAND_DISPLAY"
  else
    fehler "$name Anzeige: kein eigener Xvfb (siehe $OUT/$name.out)"
  fi
  if [ -s "$OUT/$name.png" ]; then ok "$name Screenshot: $OUT/$name.png"; else fehler "$name Screenshot fehlt"; fi
  if [ "$(cat "$OUT/$name.ready" 2>/dev/null)" = fertig ]; then
    ok "$name Laden: alle Konten angemeldet bzw. abgeschlossen, Ordner geladen"
  else
    fehler "$name Laden: nach $TIMEOUT s nicht fertig"
  fi

  # (a) Konten
  if [ -n "$KONTEN" ] && [ "$(angezeigt "$log")" = "$KONTEN" ]; then
    ok "$name (a) Konten: $(echo "$KONTEN" | wc -l) angezeigt = Registry-Regel ($(debug "$log" | grep '^Seitenleiste: ' | tail -1))"
  else
    fehler "$name (a) Konten: App [$(angezeigt "$log" | tr '\n' ' ')] ≠ Registry [$(echo "$KONTEN" | tr '\n' ' ')]"
  fi

  # (b) Anmeldung
  fehlt=""
  for uid in $KONTEN; do
    debug "$log" | grep -qE "^angemeldet: $uid [^ ]+ angenommen$" || fehlt="$fehlt $uid"
  done
  if [ -n "$KONTEN" ] && [ -z "$fehlt" ]; then
    ok "$name (b) Anmeldung: alle $(echo "$KONTEN" | wc -l) angenommen ($(debug "$log" | sed -nE 's/^angemeldet: [^ ]+ ([^ ]+) angenommen$/\1/p' | sort | uniq -c | awk '{printf "%s%s×%s", s, $1, $2; s=", "}'))"
  else
    fehler "$name (b) Anmeldung: nicht angenommen:${fehlt:- keine Konten}"
  fi

  # (c) Ordner: Namen je Konto wie testdaten.py --ordner, Anzahl „N geladen“ = ANZAHL; beide
  # Zahlen müssen da sein (sonst gälten zwei leere Listen nach Fehlern als gleich).
  # #363: Virtuellen Papierkorb/Spam von imapx (.#evolution/…) blendet die App neben einem echten Ordner gleichen
  # Typs aus (mail-folder.c, Log „Spezialordner: UID virtueller Ordner … ausgeblendet“); die Vergleichsliste
  # ebenso, ausdrücklich als WERT.
  fehlt=""
  for uid in $KONTEN; do
    versteckt="$(debug "$log" | sed -nE "s/^Spezialordner: $uid virtueller Ordner (\.#evolution\/(Trash|Junk)) ausgeblendet$/\1/p" | sort -u)"
    [ -n "$versteckt" ] && echo "WERT    $name (c) $uid ausgeblendet: $(echo $versteckt)"
    n_app="$(debug "$log" | sed -nE "s/^Ordner: $uid ([0-9]+) geladen$/\1/p" | tail -1)"
    n_ref="$(awk -v u="$uid" '$1 == "ANZAHL" && $2 == u { print $3 }' "$OUT/ordner-testdaten.txt")"
    [ -n "$n_ref" ] && n_ref=$((n_ref - $(printf '%s' "$versteckt" | grep -c .)))
    if [ -n "$n_app" ] && [ "$n_app" = "$n_ref" ] \
       && diff <(sed -n "s/^ORDNER $uid //p" "$OUT/ordner-testdaten.txt" | grep -vxF -e "${versteckt:-//}" | sort) \
               <(debug "$log" | sed -n "s/^Ordner: $uid //p" \
                   | grep -vE '^([0-9]+ geladen|nicht geladen: .*|nicht mehr angezeigt)$' | sort -u) \
               >"$OUT/$name-ordner-$uid.diff"; then
      rm -f "$OUT/$name-ordner-$uid.diff"
    else
      fehlt="$fehlt $uid(${n_app:-?}/${n_ref:-?})"
    fi
  done
  if [ -n "$KONTEN" ] && [ -z "$fehlt" ]; then
    ok "$name (c) Ordner: $(debug "$log" | grep -cE '^Ordner: [^ ]+ [0-9]+ geladen$') Konten, $(debug "$log" | sed -nE 's/^Ordner: [^ ]+ ([0-9]+) geladen$/\1/p' | awk '{ n += $1 } END { print n + 0 }') Ordner = testdaten.py --ordner"
  else
    fehler "$name (c) Ordner: abweichend (UID(Anzahl App/testdaten.py)):${fehlt:- keine Konten}, Namen in $OUT/$name-ordner-*.diff"
  fi

  # (d) Log: Positivliste. Erlaubt sind nur DEBUG-Zeilen der App und die zwei Zeilen
  # Mesa-Rauschen; jede andere Zeile (WARNING, CRITICAL, Gtk-Message, …) ist ein Fehler.
  grep -vE '^\(revolution:[0-9]+\): revolution-DEBUG: ' "$log" | grep -vxF "$EGL_RAUSCHEN" >"$OUT/$name-rest.txt"
  if [ -s "$OUT/$name-rest.txt" ]; then
    fehler "$name (d) Log: $(wc -l <"$OUT/$name-rest.txt") unerwartete Zeilen, z. B. $(grep -m1 . "$OUT/$name-rest.txt" | cut -c1-200) (siehe $OUT/$name-rest.txt)"
  else
    ok "$name (d) Log: nur DEBUG-Zeilen, 0 WARNING/CRITICAL ($(grep -cxF "$EGL_RAUSCHEN" "$log") Zeilen Mesa-Rauschen übergangen)"
  fi

  # (e) Beenden
  read -r pid rc 2>/dev/null <"$quit" || { pid=""; rc="keine QUIT-Datei"; }
  rest="$(pgrep -a -f "^$BIN( |$)")"
  # Auch Xvfb und die Compositing-Attrappe dieses Laufs (Anzeige aus screenshot.sh) müssen weg.
  disp="$(sed -nE 's/^Xvfb-Anzeige: (:[0-9]+) .*/\1/p' "$OUT/$name.out")"
  if [ -n "$disp" ]; then
    rest="$rest$(pgrep -a -f "^Xvfb $disp( |$)")"
    for p in $(pgrep -f _NET_WM_CM_S0); do
      tr '\0' '\n' <"/proc/$p/environ" 2>/dev/null | grep -qx "DISPLAY=$disp" && rest="$rest Helfer:$p"
    done
  fi
  if [ "$rc" = 0 ] && [ -n "$pid" ] && [ ! -e "/proc/$pid" ] && [ -z "$rest" ] && [ "$(bus_hat_app)" = "(false,)" ]; then
    ok "$name (e) Beenden: WM_DELETE_WINDOW → Exit 0 (PID $pid), kein Rest-Prozess (App, Xvfb $disp, Helfer), NameHasOwner = false"
  else
    fehler "$name (e) Beenden: Exit ${rc:-?} (PID ${pid:-?}), Rest: ${rest:-keiner}, Bus: $(bus_hat_app)"
  fi
}

# ---------------------------------------------------------------- Läufe

lauf () {
  local name="$1" scheme="$2" keys="$3" size="$4" watcher
  warte "$OUT/$name.log" "$OUT/$name.ready" & watcher=$!
  env G_MESSAGES_DEBUG=revolution G_DEBUG=fatal-criticals BUILD="$BUILD" OUT_DIR="$OUT" \
    LOG="$OUT/$name.log" READY="$OUT/$name.ready" QUIT="$OUT/$name.quit" WAIT=$((TIMEOUT + 5)) \
    KEYS="$keys" timeout -k 10 $((TIMEOUT + 120)) "$APP_DIR/scripts/screenshot.sh" "$name" "$scheme" "$size" >"$OUT/$name.out" 2>&1 \
    || fehler "$name: scripts/screenshot.sh Exit $? (siehe $OUT/$name.out)"
  kill "$watcher" 2>/dev/null; wait "$watcher" 2>/dev/null
  pruefe_lauf "$name"
}

# Namen wie LAEUFE oben. Mail 4800 px hoch, damit alle Konten mit allen Ordnern im Bild sind
# (wie README „Ordner“); Kalender und Kontakte in der Standardgröße des Fensters.
lauf mail-hell prefer-light "" 1200x4800
lauf mail-dunkel prefer-dark "" 1200x4800
lauf kalender-hell prefer-light "Control_L+2" 1200x760
lauf kontakte-hell prefer-light "Control_L+3" 1200x760

# ---------------------------------------------------------------- (f) Zustand nachher

PIDS_NACHHER="$(pids)"
if [ "$PIDS_NACHHER" = "$PIDS_VORHER" ]; then ok "(f) PIDs unverändert: $PIDS_NACHHER"; else fehler "(f) PIDs: vorher $PIDS_VORHER, nachher $PIDS_NACHHER"; fi
quellen >"$OUT/sources-nachher.txt" 2>&1
if diff -q "$OUT/sources-vorher.txt" "$OUT/sources-nachher.txt" >/dev/null; then
  ok "(f) ~/.config/evolution/sources unverändert: $(ls "$SOURCES" | wc -l) Dateien, sha256, Änderungszeit und Inode gleich"
else
  fehler "(f) ~/.config/evolution/sources geändert: $(diff "$OUT/sources-vorher.txt" "$OUT/sources-nachher.txt" | grep -c '^[<>]') Zeilen abweichend"
fi

abbruch_bilanz
echo "Ergebnis: $FEHLER Fehler, Ausgaben in $OUT"
[ "$FEHLER" -eq 0 ]
