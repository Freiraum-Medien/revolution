#!/usr/bin/env bash
# Headless-Screenshot des Prototyps.
# Aufruf: scripts/screenshot.sh NAME SCHEMA [BREITExHÖHE]
#   NAME    Dateiname ohne .png, z. B. mail-hell
#   SCHEMA  prefer-light oder prefer-dark (ADW_DEBUG_COLOR_SCHEME)
#   GRÖSSE  Fenstergröße in Pixeln, Standard 1200x760 (Fenster-Standard, window.blp)
#   CLICK=X,Y (optional, Umgebung): vor der Aufnahme ein Linksklick per XTest, z. B. Menü öffnen
# Läuft ausschließlich in einem eigenen Xvfb (xvfb-run -a) und einer eigenen
# D-Bus-Sitzung (dbus-run-session). Es erscheint nie ein Fenster auf dem Desktop.
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$APP_DIR/build/src/revolution"
OUT_DIR="${OUT_DIR:-$APP_DIR/../Design/Prototyp}"

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
  sed -n '3,7p' "$0" >&2
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
[ -z "$CLICK" ] || [[ "$CLICK" =~ ^[0-9]+,[0-9]+$ ]] || { echo "CLICK als X,Y angeben, z. B. 640,27" >&2; exit 1; }

[ -x "$BIN" ] || { echo "Fehlt: $BIN (erst meson compile -C build)" >&2; exit 1; }
for tool in xvfb-run dbus-run-session import xwininfo python3; do
  command -v "$tool" >/dev/null || { echo "Fehlt: $tool" >&2; exit 1; }
done
python3 -c 'import Xlib' 2>/dev/null || { echo "Fehlt: python3-xlib" >&2; exit 1; }
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
    REAL_DISPLAY="$REAL_DISPLAY" BIN="$BIN" OUT="$out" WIDTH="$WIDTH" HEIGHT="$HEIGHT" CLICK="$CLICK" \
    GDK_BACKEND=x11 GDK_DEBUG=no-portals ADW_DISABLE_PORTAL=1 GIO_USE_VFS=local GSK_RENDERER=cairo GTK_A11Y=none NO_AT_BRIDGE=1 \
    ADW_DEBUG_COLOR_SCHEME="$scheme" \
    xvfb-run -a -s "-screen 0 ${WIDTH}x${HEIGHT}x24" \
    dbus-run-session -- bash -c '
      set -euo pipefail
      if [ -z "${DISPLAY:-}" ] || [ "$DISPLAY" = "$REAL_DISPLAY" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then
        echo "Abbruch: Anzeige der echten Sitzung wäre betroffen (DISPLAY=${DISPLAY:-} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-})" >&2
        exit 2
      fi
      echo "Xvfb-Anzeige: $DISPLAY (echte Sitzung: ${REAL_DISPLAY:-keine})"
      "$BIN" & pid=$!
      sleep 3
      win=$(xwininfo -root -tree | awk "/\"Revolution\"/ {print \$1; exit}")
      if [ -n "$win" ]; then
        # Ohne Fenstermanager setzt XConfigureWindow die Größe direkt (python3-xlib).
        WIN="$win" python3 -c "import os; from Xlib import display; d = display.Display(); d.create_resource_object(\"window\", int(os.environ[\"WIN\"], 16)).configure(width=int(os.environ[\"WIDTH\"]), height=int(os.environ[\"HEIGHT\"])); d.sync()"
        sleep 2
        if [ -n "$CLICK" ]; then
          python3 -c "import os; from Xlib import display, X; from Xlib.ext import xtest; d = display.Display(); x, y = map(int, os.environ[\"CLICK\"].split(\",\")); xtest.fake_input(d, X.MotionNotify, x=x, y=y); xtest.fake_input(d, X.ButtonPress, 1); xtest.fake_input(d, X.ButtonRelease, 1); d.sync()"
          sleep 1
          # Popover ist eine eigene X-Oberfläche: ganze Xvfb-Anzeige (= Fenstergröße) aufnehmen.
          import -window root "$OUT"
        else
          import -window "$win" "$OUT"
        fi
      else
        import -window root "$OUT"
      fi
      kill "$pid"; wait "$pid" 2>/dev/null || true
    '
  rm -rf "$rundir"
  echo "Geschrieben: $out"
}

shoot "$SCHEME" "$OUT_DIR/$NAME.png"
