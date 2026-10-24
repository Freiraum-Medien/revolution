#!/usr/bin/env python3
# sortierung-treiber.py – Teil „nicht oben“ von tests/sortierung.sh (#419), läuft im Xvfb aus xvfb-run.
#
# Startet die App (BIN) im Posteingang (Konversationsansicht, Schema aus GSETTINGS_SCHEMA_DIR). Nach
# „<Konto> INBOX vollständig“ Klick in die vierte Zeile und fünfmal Pfeil ab: die neunte Zeile ist gewählt,
# die Liste steht nicht oben (Faden, die älteste, steht weiter unten). Dann legt tests/testdaten.py --sortierung KONTO neu zwei Mails am Server ab; sobald
# die Live-Änderung sie in die Liste bringt („Änderung (Konversationen) N neu“), Bild. Erwartet wie
# Evolution und Apple Mail: kein Sprung, die gewählte Zeile steht an derselben Stelle (Akzentfarbe in der
# Spalte der Liste, ±2 px); oben (#418) springt die Liste nur, wenn sie oben stand.
# Umgebung: BIN, OUT, NAME, KONTO, KONTO_UID, APP_DIR, NEU (Ausgabedatei von --sortierung neu), TIMEOUT.
# Ausgabe: „OK …“, „FEHLER …“, „WERT …“; Bilder OUT/NAME-vorher.png und OUT/NAME.png, Log OUT/NAME.log.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import re
import subprocess
import sys
import time
import traceback

from PIL import Image
from Xlib import X, XK, display, protocol
from Xlib.ext import xtest

BIN, OUT, NAME, KONTO = os.environ['BIN'], os.environ['OUT'], os.environ['NAME'], os.environ['KONTO']
KONTO_UID, APP_DIR, NEU = os.environ['KONTO_UID'], os.environ['APP_DIR'], os.environ['NEU']
TIMEOUT = int(os.environ.get('TIMEOUT', '300'))
LOG = os.path.join(OUT, NAME + '.log')
# Fenster 1200×760 wie tests/sortierung.sh: Spalte der Liste (Avatare), Farbe der gewählten Zeile (wie
# tests/flags-treiber.py), Liste unter der Kopfleiste ab y = 59.
LISTE_X, LISTE_Y, ZEILE_Y = 300, 59, 400
GEWAEHLT = (211, 70, 21)

fehler = 0
d = display.Display()


def ok(text):
    print('OK      %s: %s' % (NAME, text), flush=True)


def falsch(text):
    global fehler
    fehler += 1
    print('FEHLER  %s: %s' % (NAME, text), flush=True)


def wert(text):
    print('WERT    %s: %s' % (NAME, text), flush=True)


def log_text():
    with open(LOG, encoding='utf-8', errors='replace') as f:
        return f.read()


def warte(muster, anzahl=1, zeit=TIMEOUT):
    """Bis muster mindestens anzahl-mal im Log steht; True/False."""
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        if len(re.findall(muster, log_text())) >= anzahl:
            return True
        time.sleep(0.2)
    return False


def bild():
    g = d.screen().root.get_geometry()
    return Image.frombytes('RGB', (g.width, g.height), d.screen().root.get_image(
        0, 0, g.width, g.height, X.ZPixmap, 0xffffffff).data, 'raw', 'BGRX')


def gewaehlt_y(b):
    """Oberkante der gewählten Zeile in der Spalte der Liste oder None."""
    for y in range(LISTE_Y, b.size[1]):
        if all(abs(a - c) <= 30 for a, c in zip(b.getpixel((LISTE_X, y)), GEWAEHLT)):
            return y
    return None


def taste(name):
    code = d.keysym_to_keycode(XK.string_to_keysym(name))
    xtest.fake_input(d, X.KeyPress, code)
    xtest.fake_input(d, X.KeyRelease, code)
    d.sync()


def klick(x, y):
    xtest.fake_input(d, X.MotionNotify, x=x, y=y)
    xtest.fake_input(d, X.ButtonPress, 1)
    xtest.fake_input(d, X.ButtonRelease, 1)
    xtest.fake_input(d, X.MotionNotify, x=0, y=0)
    d.sync()


def fenster():
    for _ in range(100):
        for w in d.screen().root.query_tree().children:
            try:
                if w.get_wm_name() == 'Revolution' and w.get_attributes().map_state == X.IsViewable:
                    return w
            except Exception:
                pass
        time.sleep(0.2)
    return None


def schliessen(app, w):
    w.send_event(protocol.event.ClientMessage(
        window=w, client_type=d.intern_atom('WM_PROTOCOLS'),
        data=(32, [d.intern_atom('WM_DELETE_WINDOW'), X.CurrentTime, 0, 0, 0])), event_mask=X.NoEventMask)
    d.sync()
    try:
        return app.wait(30)
    except subprocess.TimeoutExpired:
        return None


def pruefen(app, w):
    if not warte(r'Nachrichten: %s INBOX vollständig' % KONTO_UID):
        falsch('Liste nicht geladen in %d s' % TIMEOUT)
        return
    time.sleep(1)
    klick(LISTE_X + 120, ZEILE_Y)
    time.sleep(1)
    for _ in range(5):
        taste('Down')
        time.sleep(0.3)
    time.sleep(2)
    vorher = bild()
    vorher.save(os.path.join(OUT, NAME + '-vorher.png'))
    y0 = gewaehlt_y(vorher)
    if y0 is None or y0 < 300:
        falsch('nach Pfeil ab keine gewählte Zeile unten im Bild (y = %s, %s-vorher.png)' % (y0, NAME))
        return
    wert('gewählte Zeile vorher bei y = %d' % y0)

    neue = r'INBOX Änderung \(Konversationen\) [1-9][0-9]* neu'
    anzahl = len(re.findall(neue, log_text()))
    with open(NEU, 'w') as f:
        rc = subprocess.run([sys.executable, os.path.join(APP_DIR, 'tests', 'testdaten.py'), '--sortierung', KONTO,
                             'neu'], stdout=f, stderr=subprocess.STDOUT, timeout=300).returncode
    if rc != 0:
        falsch('Abbruch: tests/testdaten.py --sortierung %s neu: Exit %d (%s)' % (KONTO, rc, NEU))
        return
    if not warte(neue, anzahl + 1):
        falsch('neue Konversation nicht in %d s in der Liste' % TIMEOUT)
        return
    time.sleep(3)
    nachher = bild()
    nachher.save(os.path.join(OUT, NAME + '.png'))
    y1 = gewaehlt_y(nachher)
    wert('gewählte Zeile nachher bei y = %s' % y1)
    (ok if y1 is not None and abs(y1 - y0) <= 2 else falsch)(
        'Liste nicht oben: kein Sprung, gewählte Zeile an derselben Stelle (vorher y = %d, nachher y = %s)' % (y0, y1))


def main():
    log = open(LOG, 'w')
    app = subprocess.Popen([BIN], stdout=log, stderr=subprocess.STDOUT)
    try:
        w = fenster()
        if w is None:
            falsch('kein Fenster')
            return 1
        w.configure(width=1200, height=760)
        d.sync()
        pruefen(app, w)
        rc = schliessen(app, w)
        (ok if rc == 0 else falsch)('Beenden: Exit %s' % rc)
    finally:
        if app.poll() is None:
            app.kill()
            app.wait()
        log.close()
    andere = [z.rstrip() for z in open(LOG, encoding='utf-8', errors='replace')
              if z.startswith('(revolution:') and 'revolution-DEBUG' not in z]
    (ok if not andere else falsch)('keine Warnungen/Criticals im Log%s' % ('' if not andere else ': %s' % andere[:3]))
    return 1 if fehler else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception:
        traceback.print_exc()
        falsch('Abbruch: %s' % traceback.format_exc().strip().splitlines()[-1])
        sys.exit(1)
