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
# Compositor (#400): Xvfb hat keinen. Ohne ihn zeichnet GTK Fenster und Popover eckig ohne
#   Schatten (window.solid-csd), Menüs sähen anders aus als auf dem Desktop. Ein Python-Helfer
#   meldet sich als Compositing-Manager (Besitzer der Auswahl _NET_WM_CM_S0, ICCCM/EWMH);
#   GTK nutzt dann RGBA-Fenster. Die Aufnahme setzt alle Fenster mit Alphakanal in
#   Stapelreihenfolge zusammen (python3-pil) und schneidet auf das App-Fenster zu.
# Läuft ausschließlich in einem eigenen Xvfb (xvfb-run -a) und einer eigenen
# D-Bus-Sitzung (dbus-run-session). Es erscheint nie ein Fenster auf dem Desktop.
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$APP_DIR/build/src/revolution"
OUT_DIR="${OUT_DIR:-$APP_DIR/../Design/Prototyp}"

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
  sed -n '3,13p' "$0" >&2
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
ROUND="${ROUND:-}"
[ -z "$ROUND" ] || [[ "$ROUND" =~ ^[0-9]+$ ]] || { echo "ROUND als Radius in Pixeln angeben, z. B. 15" >&2; exit 1; }

[ -x "$BIN" ] || { echo "Fehlt: $BIN (erst meson compile -C build)" >&2; exit 1; }
for tool in xvfb-run dbus-run-session import xwininfo python3 ${ROUND:+magick}; do
  command -v "$tool" >/dev/null || { echo "Fehlt: $tool" >&2; exit 1; }
done
python3 -c 'import Xlib' 2>/dev/null || { echo "Fehlt: python3-xlib" >&2; exit 1; }
python3 -c 'import PIL' 2>/dev/null || { echo "Fehlt: python3-pil" >&2; exit 1; }
mkdir -p "$OUT_DIR"

REAL_DISPLAY="${DISPLAY:-}"

shoot () {
  local scheme="$1" out="$2" rundir
  rundir="$(mktemp -d)"
  # Wayland/Keyring der echten Sitzung abschneiden, eigenes Laufzeitverzeichnis,
  # keine Portale (sonst startet die Test-D-Bus-Sitzung xdg-desktop-portal),
  # X11-Backend erzwingen.
  env -u WAYLAND_DISPLAY -u DISPLAY -u GNOME_KEYRING_CONTROL -u SSH_AUTH_SOCK \
    XDG_RUNTIME_DIR="$rundir" \
    REAL_DISPLAY="$REAL_DISPLAY" BIN="$BIN" OUT="$out" WIDTH="$WIDTH" HEIGHT="$HEIGHT" CLICK="$CLICK" KEYS="$KEYS" \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo GTK_A11Y=none NO_AT_BRIDGE=1 \
    ADW_DEBUG_COLOR_SCHEME="$scheme" \
    xvfb-run -a -s "-screen 0 ${WIDTH}x${HEIGHT}x24 +extension Composite" \
    dbus-run-session -- bash -c '
      set -euo pipefail
      if [ -z "${DISPLAY:-}" ] || [ "$DISPLAY" = "$REAL_DISPLAY" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then
        echo "Abbruch: Anzeige der echten Sitzung wäre betroffen (DISPLAY=${DISPLAY:-} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-})" >&2
        exit 2
      fi
      echo "Xvfb-Anzeige: $DISPLAY (echte Sitzung: ${REAL_DISPLAY:-keine})"
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
      "$BIN" & pid=$!
      sleep 3
      win=$(xwininfo -root -tree | awk "/\"Revolution\"/ {print \$1; exit}")
      if [ -n "$win" ]; then
        # Ohne Fenstermanager setzt XConfigureWindow die Größe direkt (python3-xlib).
        WIN="$win" python3 -c "import os; from Xlib import display; d = display.Display(); d.create_resource_object(\"window\", int(os.environ[\"WIN\"], 16)).configure(width=int(os.environ[\"WIDTH\"]), height=int(os.environ[\"HEIGHT\"])); d.sync()"
        sleep 2
        if [ -n "$KEYS" ]; then
          # Je Eintrag in KEYS: Tasten nacheinander drücken, rückwärts loslassen.
          python3 -c "
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
          sleep 1
        fi
        if [ -n "$CLICK" ]; then
          for point in $CLICK; do
            POINT="$point" python3 -c "import os; from Xlib import display, X; from Xlib.ext import xtest; d = display.Display(); x, y = map(int, os.environ[\"POINT\"].split(\",\")); xtest.fake_input(d, X.MotionNotify, x=x, y=y); xtest.fake_input(d, X.ButtonPress, 1); xtest.fake_input(d, X.ButtonRelease, 1); d.sync()"
            sleep 1
          done
          # Zeiger nach 0,0: ein unter ihm geöffnetes Fenster zeigte sonst einen Tooltip (#337).
          python3 -c "from Xlib import display, X; from Xlib.ext import xtest; d = display.Display(); xtest.fake_input(d, X.MotionNotify, x=0, y=0); d.sync()"
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
      kill "$pid" "$cm"; wait "$pid" 2>/dev/null || true
    '
  rm -rf "$rundir"
  if [ -n "$ROUND" ]; then
    # Maske: weißes abgerundetes Rechteck auf Schwarz, als Alphakanal übernehmen.
    magick "$out" \( +clone -alpha off -fill black -colorize 100 -fill white \
      -draw "roundrectangle 0,0 %[fx:w-1],%[fx:h-1] $ROUND,$ROUND" \) \
      -alpha off -compose CopyOpacity -composite "$out"
  fi
  echo "Geschrieben: $out"
}

shoot "$SCHEME" "$OUT_DIR/$NAME.png"
