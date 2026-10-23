#!/usr/bin/env bash
# Headless-Screenshot des Prototyps.
# Aufruf: scripts/screenshot.sh NAME SCHEMA [BREITExHÖHE]
#   NAME    Dateiname ohne .png, z. B. mail-hell
#   SCHEMA  prefer-light oder prefer-dark (ADW_DEBUG_COLOR_SCHEME)
#   GRÖSSE  Fenstergröße in Pixeln, Standard 1200x760 (Fenster-Standard, window.blp)
#   CLICK=X,Y (optional, Umgebung): vor der Aufnahme ein Linksklick per XTest, z. B. Menü öffnen;
#     mehrere Klicks nacheinander als „X,Y X,Y“
#   KEYS="Control_L+3 …" (optional, Umgebung): vor CLICK Tastendrücke per XTest (X-Keysym-Namen)
#   ROUND=15 (optional, Umgebung): Fensterecken nachträglich mit ImageMagick abrunden (Radius in px,
#     transparente Ecken). Seit #400 zeichnet GTK die Ecken selbst (siehe Compositor), ROUND
#     bleibt als Absicherung; auf dem Desktop gilt --window-radius: 15px (libadwaita-Stylesheet
#     default-light-yaru-default.css Zeile 159).
#   SCALE=1.5 (optional, Umgebung): Ausgabe-PNG 1,5-fach bei gleicher logischer Größe und gleichem
#     Layout. GTK rendert mit GDK_SCALE=2 (X11-Backend, Xvfb und Fenster doppelt so groß),
#     ImageMagick verkleinert danach auf SCALE/2 (1,5 → 75 %). CLICK, KEYS und ROUND bleiben
#     logisch (wie ohne SCALE), das Skript rechnet sie um. Werte über 2 wären hochskaliert.
#   AFTER_KEYS="Down …" (optional, Umgebung): wie KEYS, aber nach CLICK, z. B. Tastatur in einer
#     angeklickten Liste (#348)
#   WAIT=40 (optional, Umgebung): Sekunden zusätzlich warten vor KEYS/CLICK und Aufnahme,
#     z. B. bis die Ordner aller Konten geladen sind (#348); Standard 0
#   BUILD=Pfad (optional, Umgebung): Build-Ordner, Standard build/ im Repo. GSettings-Schema
#     aus BUILD/data, Einstellungen nur im Speicher (GSETTINGS_BACKEND=memory, #342).
#   LOG=Datei (optional, Umgebung, #349): Ausgabe der App (stdout und stderr) in diese Datei
#   READY=Datei (optional, Umgebung, #349): statt fester WAIT-Sekunden warten, bis die Datei
#     existiert, höchstens WAIT Sekunden (tests/smoke.sh legt sie an, sobald alles geladen ist)
#   QUIT=Datei (optional, Umgebung, #349): nach der Aufnahme das Fenster per WM_DELETE_WINDOW
#     schließen wie der Schließen-Knopf, höchstens 20 s aufs Ende warten; in die Datei
#     „PID Exit-Code“ bzw. „PID Zeitlimit“ (dann kill). Ohne QUIT: kill wie bisher.
# Compositor (#400): Xvfb hat keinen. Ohne ihn zeichnet GTK Fenster und Popover eckig ohne
#   Schatten (window.solid-csd), Menüs sähen anders aus als auf dem Desktop. Ein Python-Helfer
#   meldet sich als Compositing-Manager (Besitzer der Auswahl _NET_WM_CM_S0, ICCCM/EWMH);
#   GTK nutzt dann RGBA-Fenster. Die Aufnahme setzt alle Fenster mit Alphakanal in
#   Stapelreihenfolge zusammen (python3-pil) und schneidet auf das App-Fenster zu.
# Läuft ausschließlich in einem eigenen Xvfb (xvfb-run -a); es erscheint nie ein Fenster auf
# dem Desktop. D-Bus: der echte Session-Bus, keine eigene Sitzung (PM #343). Eine eigene
# dbus-run-session startete eine zweite evolution-source-registry und einen zweiten goa-daemon
# gegen das echte ~/.config; die zweite Registry schrieb Dateien in ~/.config/evolution/sources
# neu. So schreibt nur die vorhandene Registry. Läuft Revolution schon auf dem Bus, bricht das
# Skript ab: der Start würde sonst die laufende Instanz aktivieren (Fenster auf dem Desktop).
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$APP_DIR/build}"
BIN="$BUILD/src/revolution"
OUT_DIR="${OUT_DIR:-$APP_DIR/../Design/Prototyp}"

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
  sed -n '3,29p' "$0" >&2
  exit 1
fi
NAME="$1"
SCHEME="$2"
SIZE="${3:-1200x760}"
[[ "$NAME" =~ ^[A-Za-z0-9_-]+$ ]] || { echo "NAME nur aus A-Z, a-z, 0-9, _ und -" >&2; exit 1; }
case "$SCHEME" in prefer-light|prefer-dark) ;; *) echo "Unbekanntes Schema: $SCHEME" >&2; exit 1 ;; esac
[[ "$SIZE" =~ ^[0-9]+x[0-9]+$ ]] || { echo "Größe als BREITExHÖHE angeben, z. B. 900x700" >&2; exit 1; }
WIDTH="${SIZE%x*}"
HEIGHT="${SIZE#*x}"
CLICK="${CLICK:-}"
[ -z "$CLICK" ] || [[ "$CLICK" =~ ^[0-9]+,[0-9]+(\ [0-9]+,[0-9]+)*$ ]] || { echo "CLICK als X,Y angeben, z. B. 640,27" >&2; exit 1; }
KEYS="${KEYS:-}"
[ -z "$KEYS" ] || [[ "$KEYS" =~ ^[A-Za-z0-9_+\ ]+$ ]] || { echo "KEYS als Keysym-Namen angeben, z. B. Control_L+3" >&2; exit 1; }
AFTER_KEYS="${AFTER_KEYS:-}"
[ -z "$AFTER_KEYS" ] || [[ "$AFTER_KEYS" =~ ^[A-Za-z0-9_+\ ]+$ ]] || { echo "AFTER_KEYS als Keysym-Namen angeben, z. B. Down" >&2; exit 1; }
ROUND="${ROUND:-}"
WAIT="${WAIT:-0}"
[[ "$WAIT" =~ ^[0-9]+$ ]] || { echo "WAIT als Sekunden angeben, z. B. 40" >&2; exit 1; }
[ -z "$ROUND" ] || [[ "$ROUND" =~ ^[0-9]+$ ]] || { echo "ROUND als Radius in Pixeln angeben, z. B. 15" >&2; exit 1; }
LOG="${LOG:-}"
READY="${READY:-}"
QUIT="${QUIT:-}"
SCALE="${SCALE:-}"
RENDER=1
if [ -n "$SCALE" ]; then
  [[ "$SCALE" =~ ^[0-9]+(\.[0-9]+)?$ ]] && LC_ALL=C awk -v s="$SCALE" 'BEGIN { exit !(s > 0 && s <= 2) }' \
    || { echo "SCALE als Faktor über 0 bis 2 angeben, z. B. 1.5" >&2; exit 1; }
  RENDER=2
fi

[ -x "$BIN" ] || { echo "Fehlt: $BIN (erst meson compile -C build)" >&2; exit 1; }
[ -f "$BUILD/data/gschemas.compiled" ] || { echo "Fehlt: $BUILD/data/gschemas.compiled (erst meson compile)" >&2; exit 1; }
for tool in xvfb-run gdbus import xwininfo python3 ${ROUND:+magick} ${SCALE:+magick}; do
  command -v "$tool" >/dev/null || { echo "Fehlt: $tool" >&2; exit 1; }
done
python3 -c 'import Xlib' 2>/dev/null || { echo "Fehlt: python3-xlib" >&2; exit 1; }
python3 -c 'import PIL' 2>/dev/null || { echo "Fehlt: python3-pil" >&2; exit 1; }
mkdir -p "$OUT_DIR"

APP_ID="$(sed -n 's/.*<id>\(.*\)<\/id>.*/\1/p' "$BUILD/data/"*.metainfo.xml | head -1)"
[ -n "$APP_ID" ] || { echo "App-ID nicht gefunden in $BUILD/data/*.metainfo.xml" >&2; exit 1; }
[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ] || { echo "Kein Session-Bus (DBUS_SESSION_BUS_ADDRESS leer)" >&2; exit 1; }
if gdbus call --session -d org.freedesktop.DBus -o /org/freedesktop/DBus \
     -m org.freedesktop.DBus.NameHasOwner "$APP_ID" | grep -q true; then
  echo "Abbruch: $APP_ID läuft schon auf dem Session-Bus" >&2
  exit 1
fi

REAL_DISPLAY="${DISPLAY:-}"
# Laufzeitverzeichnis auch bei Abbruch (set -e, Strg+C) entfernen.
RUNDIR=""
trap 'rm -rf "${RUNDIR:-}"' EXIT

shoot () {
  local scheme="$1" out="$2"
  RUNDIR="$(mktemp -d)"
  # Wayland/Keyring der echten Sitzung abschneiden, eigenes Laufzeitverzeichnis (der echte
  # Session-Bus bleibt über DBUS_SESSION_BUS_ADDRESS erreichbar), keine Portale, X11-Backend
  # erzwingen.
  env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK \
    XDG_RUNTIME_DIR="$RUNDIR" GSETTINGS_SCHEMA_DIR="$BUILD/data" GSETTINGS_BACKEND=memory \
    REAL_DISPLAY="$REAL_DISPLAY" BIN="$BIN" OUT="$out" WIDTH="$((WIDTH * RENDER))" HEIGHT="$((HEIGHT * RENDER))" \
    CLICK="$CLICK" KEYS="$KEYS" AFTER_KEYS="$AFTER_KEYS" WAIT="$WAIT" RENDER="$RENDER" ${SCALE:+GDK_SCALE=$RENDER} \
    LOG="$LOG" READY="$READY" QUIT="$QUIT" \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo GTK_A11Y=none NO_AT_BRIDGE=1 \
    ADW_DEBUG_COLOR_SCHEME="$scheme" \
    xvfb-run -a -s "-screen 0 $((WIDTH * RENDER))x$((HEIGHT * RENDER))x24 +extension Composite" \
    bash -c '
      set -euo pipefail
      if [ -z "${DISPLAY:-}" ] || [ "$DISPLAY" = "$REAL_DISPLAY" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then
        echo "Abbruch: Anzeige der echten Sitzung wäre betroffen (DISPLAY=${DISPLAY:-} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-})" >&2
        exit 2
      fi
      echo "Xvfb-Anzeige: $DISPLAY (echte Sitzung: ${REAL_DISPLAY:-keine})"
      # Attrappe und App bei jedem Ende beenden, auch nach Fehlern (keine Waisen).
      pid=""; cm=""
      trap "kill \$pid \$cm 2>/dev/null || true" EXIT
      # Compositing-Manager-Attrappe (siehe Kopf): besitzt _NET_WM_CM_S0, solange sie läuft.
      python3 -c "
import time
from Xlib import display, X
d = display.Display()
w = d.screen().root.create_window(-10, -10, 1, 1, 0, X.CopyFromParent)
w.set_selection_owner(d.intern_atom(\"_NET_WM_CM_S0\"), X.CurrentTime)
d.sync()
time.sleep(600)
" & cm=$!
      sleep 1
      if [ -n "$LOG" ]; then "$BIN" >"$LOG" 2>&1 & pid=$!; else "$BIN" & pid=$!; fi
      sleep 3
      win=$(xwininfo -root -tree | awk "/\"Revolution\"/ {print \$1; exit}")
      if [ -n "$win" ]; then
        echo "Fenster $win auf $DISPLAY"
        # Ohne Fenstermanager setzt XConfigureWindow die Größe direkt (python3-xlib).
        WIN="$win" python3 -c "import os; from Xlib import display; d = display.Display(); d.create_resource_object(\"window\", int(os.environ[\"WIN\"], 16)).configure(width=int(os.environ[\"WIDTH\"]), height=int(os.environ[\"HEIGHT\"])); d.sync()"
        sleep 2
        if [ -n "$READY" ]; then
          for _ in $(seq "$WAIT"); do [ -e "$READY" ] && break; sleep 1; done
          [ -e "$READY" ] || echo "READY nach $WAIT s nicht erreicht: $READY"
          # Eine Sekunde, damit GTK die zuletzt geladenen Zeilen auch zeichnet.
          sleep 1
        else
          sleep "$WAIT"
        fi
        # Je Eintrag in KEYS bzw. AFTER_KEYS: Tasten nacheinander drücken, rückwärts loslassen.
        press_keys="
import os, sys
from Xlib import display, X, XK
from Xlib.ext import xtest
d = display.Display()
for combo in os.environ[\"KEYS\"].split():
    codes = [d.keysym_to_keycode(XK.string_to_keysym(k)) for k in combo.split(\"+\")]
    if 0 in codes:
        sys.exit(\"Unbekannte Taste in: \" + combo)
    for c in codes:
        xtest.fake_input(d, X.KeyPress, c)
    for c in reversed(codes):
        xtest.fake_input(d, X.KeyRelease, c)
    d.sync()
"
        if [ -n "$KEYS" ]; then
          python3 -c "$press_keys"
          sleep 1
        fi
        if [ -n "$CLICK" ]; then
          for point in $CLICK; do
            POINT="$point" python3 -c "import os; from Xlib import display, X; from Xlib.ext import xtest; d = display.Display(); r = int(os.environ[\"RENDER\"]); x, y = (int(v) * r for v in os.environ[\"POINT\"].split(\",\")); xtest.fake_input(d, X.MotionNotify, x=x, y=y); xtest.fake_input(d, X.ButtonPress, 1); xtest.fake_input(d, X.ButtonRelease, 1); d.sync()"
            sleep 1
          done
          # Zeiger nach 0,0: ein unter ihm geöffnetes Fenster zeigte sonst einen Tooltip (#337).
          python3 -c "from Xlib import display, X; from Xlib.ext import xtest; d = display.Display(); xtest.fake_input(d, X.MotionNotify, x=0, y=0); d.sync()"
          sleep 1
        fi
        if [ -n "$AFTER_KEYS" ]; then
          KEYS="$AFTER_KEYS" python3 -c "$press_keys"
          sleep 1
        fi
        # Alle sichtbaren Fenster (App, Popover) mit Alphakanal in Stapelreihenfolge
        # zusammensetzen und auf das App-Fenster zuschneiden.
        WIN="$win" python3 -c "
import os
from Xlib import display, X
from PIL import Image
d = display.Display()
root = d.screen().root
size = (d.screen().width_in_pixels, d.screen().height_in_pixels)
out = Image.new(\"RGBA\", size, (0, 0, 0, 0))
for w in root.query_tree().children:
    if w.get_attributes().map_state != X.IsViewable:
        continue
    g = w.get_geometry()
    raw = w.get_image(0, 0, g.width, g.height, X.ZPixmap, 0xffffffff).data
    if g.depth == 32:
        im = Image.frombytes(\"RGBa\", (g.width, g.height), raw, \"raw\", \"BGRa\").convert(\"RGBA\")
    else:
        im = Image.frombytes(\"RGB\", (g.width, g.height), raw, \"raw\", \"BGRX\").convert(\"RGBA\")
    out.alpha_composite(im, (g.x, g.y))
g = d.create_resource_object(\"window\", int(os.environ[\"WIN\"], 16)).get_geometry()
out.crop((g.x, g.y, g.x + g.width, g.y + g.height)).save(os.environ[\"OUT\"])
"
      else
        import -window root "$OUT"
      fi
      if [ -n "$QUIT" ] && [ -n "$win" ]; then
        # WM_PROTOCOLS/WM_DELETE_WINDOW (ICCCM 4.2.8.1): GTK behandelt das wie den Schließen-Knopf.
        WIN="$win" python3 -c "
import os
from Xlib import display, X, protocol
d = display.Display()
w = d.create_resource_object(\"window\", int(os.environ[\"WIN\"], 16))
ev = protocol.event.ClientMessage(window=w, client_type=d.intern_atom(\"WM_PROTOCOLS\"),
                                  data=(32, [d.intern_atom(\"WM_DELETE_WINDOW\"), X.CurrentTime, 0, 0, 0]))
w.send_event(ev, event_mask=X.NoEventMask)
d.sync()
"
        # Beendet ist die App als Zombie (Z in /proc/PID/stat), bis wait den Exit-Code abholt.
        state=""
        for _ in $(seq 20); do
          state=$(awk "{print \$3}" "/proc/$pid/stat" 2>/dev/null || echo Z)
          [ "$state" = Z ] && break
          sleep 1
        done
        if [ "$state" = Z ]; then
          rc=0; wait "$pid" || rc=$?
          echo "$pid $rc" >"$QUIT"
          # Abgeholt: die PID kann neu vergeben werden, kein kill mehr darauf.
          pid=""
        else
          echo "$pid Zeitlimit" >"$QUIT"
        fi
      fi
      kill "$pid" "$cm" 2>/dev/null || true; wait "$pid" 2>/dev/null || true
    '
  rm -rf "$RUNDIR"
  if [ -n "$SCALE" ]; then
    # Doppelt gerendert, auf SCALE verkleinern (1,5 → 75 %); -filter Lanczos für scharfe Kanten.
    magick "$out" -filter Lanczos -resize "$(LC_ALL=C awk -v s="$SCALE" -v r="$RENDER" 'BEGIN { printf "%.4f", s / r * 100 }')%" "$out"
  fi
  if [ -n "$ROUND" ]; then
    local radius="$ROUND"
    [ -z "$SCALE" ] || radius="$(LC_ALL=C awk -v r="$ROUND" -v s="$SCALE" 'BEGIN { printf "%.0f", r * s }')"
    # Maske: weißes abgerundetes Rechteck auf Schwarz, als Alphakanal übernehmen.
    magick "$out" \( +clone -alpha off -fill black -colorize 100 -fill white \
      -draw "roundrectangle 0,0 %[fx:w-1],%[fx:h-1] $radius,$radius" \) \
      -alpha off -compose CopyOpacity -composite "$out"
  fi
  echo "Geschrieben: $out"
}

shoot "$SCHEME" "$OUT_DIR/$NAME.png"
