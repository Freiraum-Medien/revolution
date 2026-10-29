#!/usr/bin/env python3
# testkonten.py – Testkonten aus ~/.config/revolution/testkonten.ini (#437), Vorlage tests/testkonten.ini.example.
#
# Python: sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); from testkonten import KONTO_IMAP_1, …
# Shell:  . "$APP_DIR/tests/testkonten.sh" (ruft dieses Skript mit --shell auf)
# Fehlt die Datei oder ein Eintrag: Abbruch mit Pfad, keine Vorgabewerte.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

import configparser
import os
import shlex
import sys

PFAD = os.path.join(os.path.expanduser('~'), '.config', 'revolution', 'testkonten.ini')
KONTEN = ('konto_imap_1', 'konto_imap_2', 'konto_m365', 'konto_google', 'nur_lesend_1', 'nur_lesend_2',
          'nur_lesend_3')
HILFSADRESSEN = ('jana', 'lena', 'tom', 'kontakt361', 'kontakt361b', 'msgid_domain')

_ini = configparser.ConfigParser(interpolation=None)
try:
    if not _ini.read(PFAD, encoding='utf-8'):
        sys.exit('FEHLER  Testkonten: %s fehlt (Vorlage tests/testkonten.ini.example)' % PFAD)
    _werte = {k.upper(): _ini[k]['adresse'] for k in KONTEN}
    _werte.update({'HILFS_' + h.upper(): _ini['hilfsadressen'][h] for h in HILFSADRESSEN})
    SCHREIBEND = tuple(_ini[k]['adresse'] for k in KONTEN if _ini[k]['freigabe'] == 'schreibend')
    NUR_LESEND = tuple(_ini[k]['adresse'] for k in KONTEN if _ini[k]['freigabe'] == 'lesend')
except (configparser.Error, KeyError) as e:
    sys.exit('FEHLER  Testkonten: %s unvollständig (%s)' % (PFAD, e))

KONTO_IMAP_1 = _werte['KONTO_IMAP_1']
KONTO_IMAP_2 = _werte['KONTO_IMAP_2']
KONTO_M365 = _werte['KONTO_M365']
KONTO_GOOGLE = _werte['KONTO_GOOGLE']
NUR_LESEND_1 = _werte['NUR_LESEND_1']
NUR_LESEND_2 = _werte['NUR_LESEND_2']
NUR_LESEND_3 = _werte['NUR_LESEND_3']
HILFS_JANA = _werte['HILFS_JANA']
HILFS_LENA = _werte['HILFS_LENA']
HILFS_TOM = _werte['HILFS_TOM']
HILFS_KONTAKT361 = _werte['HILFS_KONTAKT361']
HILFS_KONTAKT361B = _werte['HILFS_KONTAKT361B']
MSGID_DOMAIN = _werte['HILFS_MSGID_DOMAIN']

# #373: Kalender-UIDs (Abschnitt [kalender]); nur die Kalender-Tests brauchen ihn, sie brechen ohne ihn ab.
KALENDER = dict(_ini['kalender']) if _ini.has_section('kalender') else {}

if __name__ == '__main__':
    if sys.argv[1:] != ['--shell']:
        sys.exit('Aufruf: tests/testkonten.py --shell')
    for name, wert in _werte.items():
        print('%s=%s' % (name, shlex.quote(wert)))
    print('TESTKONTEN=%s' % shlex.quote(' '.join(SCHREIBEND)))
