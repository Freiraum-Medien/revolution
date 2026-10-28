# testkonten.sh – Testkonten aus ~/.config/revolution/testkonten.ini für Shell-Skripte (#437).
#
# Einbinden: . "$APP_DIR/tests/testkonten.sh"
# Setzt KONTO_IMAP_1, KONTO_IMAP_2, KONTO_M365, KONTO_GOOGLE, NUR_LESEND_1…3, HILFS_* und TESTKONTEN
# (alle schreibend freigegebenen, durch Leerzeichen getrennt); die INI liest tests/testkonten.py.
# Fehlt die Datei: Fehlermeldung mit Pfad, Abbruch des Skripts.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

_testkonten="$(python3 -I "$(dirname "${BASH_SOURCE[0]}")/testkonten.py" --shell)" || exit 1
eval "$_testkonten"
unset _testkonten
