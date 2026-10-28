# Abbruch eines Teilschritts zählt als FEHLER (#411); gemeinsam für die Testskripte in tests/.
#
# Einbinden nach OUT (Ausgabeordner), APP_DIR und BUILD:  . "$APP_DIR/tests/abbruch.sh"
#   td ARGUMENTE…          tests/testdaten.py mit Zeitlimit TD_TIMEOUT (Standard 300 s). Exit ≠ 0
#                          (Traceback, ABBRUCH, Zeitlimit) ist ein Abbruch: Zeile „FEHLER  Abbruch: …“
#                          auf stderr und in OUT/abbrueche.txt, auch aus $(…) und Pipes heraus.
#   teilschritt NAME RC DATEI
#                          nach einem Treiber- oder App-Lauf mit Ausgabe DATEI: Zeitlimit (Exit 124
#                          bzw. 137 von timeout) oder Exit ≠ 0 ohne eigene FEHLER-Zeile in DATEI →
#                          FEHLER-Zeile auf stdout und in DATEI (die Skripte zählen FEHLER in DATEI).
#   abbruch_bilanz         am Ende: Wiederholungen nach transientem IMAP-Abbruch als WERT melden,
#                          Abbrüche aus OUT/abbrueche.txt ausgeben und zu FEHLER addieren.
# Wiederholung: tests/testdaten.py wiederholt einen transienten Netzabbruch (z. B. Gmail
# „imaplib.IMAP4.abort: socket error: EOF“) einmal und vermerkt das in TESTDATEN_WIEDERHOLT.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

# #439: Datenordner des Profils (revolution bzw. revolution-devel) aus dem Build, für tests/testdaten.py.
export USER_DIR="${USER_DIR:-$(sed -n 's/^#define USER_DIR_NAME "\(.*\)"$/\1/p' "${BUILD:-}/config.h" 2>/dev/null)}"
ABBRUECHE="$OUT/abbrueche.txt"
export TESTDATEN_WIEDERHOLT="$OUT/wiederholt.txt"
: >"$ABBRUECHE"
: >"$TESTDATEN_WIEDERHOLT"
TD_TIMEOUT="${TD_TIMEOUT:-300}"

abbruch () { echo "FEHLER  Abbruch: $*" | tee -a "$ABBRUECHE" >&2; }

td () {
  local rc=0
  timeout -k 10 "$TD_TIMEOUT" python3 "$APP_DIR/tests/testdaten.py" "$@" || rc=$?
  case "$rc" in
    0) ;;
    124|137) abbruch "tests/testdaten.py $*: Zeitlimit $TD_TIMEOUT s (Exit $rc)" ;;
    *) abbruch "tests/testdaten.py $*: Exit $rc" ;;
  esac
  return "$rc"
}

teilschritt () {
  local name="$1" rc="$2" datei="$3"
  case "$rc" in
    0) ;;
    124|137) echo "FEHLER  $name: Abbruch, Zeitlimit (Exit $rc)" | tee -a "$datei" ;;
    *) grep -q '^FEHLER' "$datei" 2>/dev/null \
         || echo "FEHLER  $name: Abbruch, Exit $rc ohne FEHLER-Zeile (Traceback siehe stderr)" | tee -a "$datei" ;;
  esac
}

abbruch_bilanz () {
  local n
  echo "WERT    Wiederholungen nach transientem Netzabbruch: $(grep -c . "$TESTDATEN_WIEDERHOLT")"
  sed 's/^/WERT      /' "$TESTDATEN_WIEDERHOLT"
  n="$(grep -c . "$ABBRUECHE")"
  cat "$ABBRUECHE"
  FEHLER=$((FEHLER + n))
}
