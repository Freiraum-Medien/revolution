#!/usr/bin/env bash
# Headless-Screenshot des Prototyps, hell und dunkel.
# Läuft ausschließlich in einem eigenen Xvfb (xvfb-run -a) und einer eigenen
# D-Bus-Sitzung (dbus-run-session). Es erscheint nie ein Fenster auf dem Desktop.
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$APP_DIR/build/src/revolution"
OUT_DIR="${OUT_DIR:-$APP_DIR/../Design/Prototyp}"
WIDTH=1200
HEIGHT=760

[ -x "$BIN" ] || { echo "Fehlt: $BIN (erst meson compile -C build)" >&2; exit 1; }
for tool in xvfb-run dbus-run-session import xwininfo; do
  command -v "$tool" >/dev/null || { echo "Fehlt: $tool" >&2; exit 1; }
done
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
    REAL_DISPLAY="$REAL_DISPLAY" BIN="$BIN" OUT="$out" \
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
      if [ -n "$win" ]; then import -window "$win" "$OUT"; else import -window root "$OUT"; fi
      kill "$pid"; wait "$pid" 2>/dev/null || true
    '
  rm -rf "$rundir"
  echo "Geschrieben: $out"
}

shoot prefer-light "$OUT_DIR/geruest-hell.png"
shoot prefer-dark  "$OUT_DIR/geruest-dunkel.png"
