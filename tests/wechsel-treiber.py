#!/usr/bin/env python3
# wechsel-treiber.py – Treiber für tests/wechsel.sh (#412), läuft im Xvfb aus xvfb-run.
#
# Startet die App (BIN) im Posteingang (ORDNER) mit den drei Mails aus tests/testdaten.py --schwer
# (Start blau, Schwer gelb, Ziel grün, untereinander). Je Pause aus PAUSEN, WDH-mal: Klick auf Start,
# angezeigt; Pfeil ab (Schwer lädt), nach der Pause Pfeil ab (Ziel). Ab dem ersten Pfeil bis 2 s nach
# dem zweiten Bilder des Lesebereichs, so schnell es geht (XGetImage). Prüft: unter dem Kopf von
# Schwer und von Ziel kein Bild mit dem Inhalt einer anderen Mail; am Ende grün; im Log „sichtbar“ der
# Ladung von Ziel erst nach deren „started“, „Gelesen“ (Signal „shown“) ebenso. Danach Ziel wieder
# ungelesen (Strg+Umschalt+K).
# Umgebung: BIN, OUT, NAME, ORDNER, UIDS („Start Schwer Ziel“ als IMAP-UIDs), PAUSEN, WDH.
# Ausgabe: „OK …“, „FEHLER …“, „WERT …“; Bilder je Lauf in OUT/NAME-bilder-<Pause>-<n>.txt,
# Bilder mit altem Inhalt als PNG.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

import datetime
import os
import re
import subprocess
import sys
import time
import traceback

from PIL import Image
from Xlib import X, XK, display, protocol
from Xlib.ext import xtest

BIN, OUT, NAME, ORDNER = os.environ['BIN'], os.environ['OUT'], os.environ['NAME'], os.environ['ORDNER']
START, SCHWER, ZIEL = os.environ['UIDS'].split()
PAUSEN = [float(p) for p in os.environ.get('PAUSEN', '0.2 0.24 0.3').split()]
WDH = int(os.environ.get('WDH', '3'))
LOGKEY = ORDNER.replace('/', ' ', 1)
# #357: in Konversationen (Standard) nennt das Log die Nachricht über den Suchordner der Konversation
# (UID mit 8 Zeichen Präfix, z. B. „Nl4YRJvS196“), sonst Konto und Ordner mit der IMAP-UID.
NACHRICHT = r'\S+ \S+ (?:\S{8})?%s'
# Fenster 1200×760 (Screenshot build/m4-412/probe.png): Spalte der Liste, Farbe der gewählten Zeile
# (wie tests/flags-treiber.py), Lesebereich mit Kopf; Betreffzeile im Kopf; Inhalt (Karte mit WebView).
LISTE_X = 300
GEWAEHLT = (211, 70, 21)
BEREICH = (580, 55, 1190, 750)
BETREFF = (80, 28, 400, 50)
INHALT = (30, 120, 590, 690)
FARBEN = {'blau': (52, 101, 164), 'gelb': (245, 194, 17), 'gruen': (46, 194, 126)}

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


LOG = os.path.join(OUT, NAME + '.log')
ZEILE_RE = re.compile(r'^\(revolution:\d+\): revolution-DEBUG: (\d\d):(\d\d):(\d\d\.\d+): (.*)$')


def log_zeilen():
    out = []
    with open(LOG, encoding='utf-8', errors='replace') as f:
        for zeile in f:
            m = ZEILE_RE.match(zeile.rstrip('\n'))
            if m:
                h, mi, s, text = m.groups()
                out.append((int(h) * 3600 + int(mi) * 60 + float(s), text))
    return out


def jetzt():
    n = datetime.datetime.now()
    return n.hour * 3600 + n.minute * 60 + n.second + n.microsecond / 1e6


def warte(muster, ab, zeit=30):
    rx = re.compile(muster)
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        for t, text in log_zeilen()[ab:]:
            if rx.search(text):
                return t, text
        time.sleep(0.05)
    return None


def taste(kombi):
    codes = [d.keysym_to_keycode(XK.string_to_keysym(k)) for k in kombi.split('+')]
    for c in codes:
        xtest.fake_input(d, X.KeyPress, c)
    for c in reversed(codes):
        xtest.fake_input(d, X.KeyRelease, c)
    d.sync()


def zeile_start():
    """Mitte der gewählten Zeile (Start, gewählt über REVOLUTION_MESSAGE) oder None."""
    root = d.screen().root
    spalte = Image.frombytes('RGB', (1, 700), root.get_image(LISTE_X, 55, 1, 700, X.ZPixmap, 0xffffffff).data,
                             'raw', 'BGRX')
    ys = [y + 55 for y in range(700) if all(abs(a - b) <= 30 for a, b in zip(spalte.getpixel((0, y)), GEWAEHLT))]
    return (LISTE_X, (ys[0] + ys[-1]) // 2) if ys else None


def klick(p):
    xtest.fake_input(d, X.MotionNotify, x=p[0], y=p[1])
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


MAX_VERSCHIEDEN = 200


def aufnahme(sekunden, nach=None, aktion=None):
    """[(Zeit, Bild)] des Lesebereichs, so schnell XGetImage es zulässt. Gleiche Bilder hintereinander
    teilen sich ein Bild-Objekt; höchstens MAX_VERSCHIEDEN verschiedene (Speicher: je Bild 1,7 MB,
    ein Lauf ohne diese Grenze belegte 14,5 GB). aktion() einmal nach „nach“ Sekunden, gibt ihre Zeit."""
    root = d.screen().root
    x0, y0, x1, y1 = BEREICH
    bilder = []
    vorher = bild = None
    verschieden = 0
    beginn = time.monotonic()
    ende = beginn + sekunden
    t_aktion = None
    while time.monotonic() < ende:
        if aktion is not None and t_aktion is None and time.monotonic() >= beginn + nach:
            aktion()
            t_aktion = jetzt()
        t = jetzt()
        raw = root.get_image(x0, y0, x1 - x0, y1 - y0, X.ZPixmap, 0xffffffff).data
        if raw != vorher:
            verschieden += 1
            if verschieden > MAX_VERSCHIEDEN:
                raise RuntimeError('mehr als %d verschiedene Bilder in %.1f s' % (MAX_VERSCHIEDEN, sekunden))
            vorher = raw
            bild = Image.frombytes('RGB', (x1 - x0, y1 - y0), raw, 'raw', 'BGRX')
        bilder.append((t, bild))
    return bilder, t_aktion


def anteile(bild):
    """Anteil der Pixel je Testfarbe im Inhalt (jedes 4. Pixel, Toleranz 30 je Kanal)."""
    klein = bild.crop(INHALT).resize(((INHALT[2] - INHALT[0]) // 4, (INHALT[3] - INHALT[1]) // 4), Image.NEAREST)
    px = list(zip(*[iter(klein.tobytes())] * 3))
    n = len(px)
    return {name: sum(1 for p in px if all(abs(p[i] - f[i]) <= 30 for i in range(3))) / n
            for name, f in FARBEN.items()}


def geladen(uid, ab, zeit=30):
    return warte(r'^Nachricht: %s geladen nach' % (NACHRICHT % uid), ab, zeit)


def lauf(pause, nr):
    tag = '%.2f s #%d' % (pause, nr)
    # Start zeigen (Klick: Fokus in der Liste). Erst danach den Wechsel messen.
    ab = len(log_zeilen())
    klick(ZEILE)
    if geladen(START, ab) is None or warte(r'^Nachricht: sichtbar ', ab, 15) is None:
        falsch('%s: Start (UID %s) nicht angezeigt' % (tag, START))
        return
    time.sleep(1)
    ab = len(log_zeilen())
    x0, y0, x1, y1 = BEREICH
    kopf_start = Image.frombytes('RGB', (x1 - x0, y1 - y0), d.screen().root.get_image(
        x0, y0, x1 - x0, y1 - y0, X.ZPixmap, 0xffffffff).data, 'raw', 'BGRX').crop(BETREFF).tobytes()
    taste('Down')
    t_schwer = jetzt()
    bilder, t_ziel = aufnahme(pause + 2.0, pause, lambda: taste('Down'))
    wert('%s: Pfeil ab zu Schwer %.3f, zu Ziel %.3f (Abstand %.0f ms)' % (tag, t_schwer, t_ziel,
                                                                     (t_ziel - t_schwer) * 1000))
    bilder_pruefen(tag, pause, nr, bilder, kopf_start)
    log_pruefen(tag, ab)

    # Ausgangszustand: Ziel wieder ungelesen (nach dem automatischen Gelesen).
    if warte(r'^Flags: %s gelesen \(automatisch\) gespeichert' % (NACHRICHT % ZIEL), ab, 15) is None:
        falsch('%s: Gelesen (automatisch) nicht gespeichert' % tag)
        return
    ab = len(log_zeilen())
    taste('Control_L+Shift_L+k')
    (ok if warte(r'^Flags: %s ungelesen gespeichert' % (NACHRICHT % ZIEL), ab, 15) else falsch)(
        '%s: Ziel wieder ungelesen' % tag)


def bilder_pruefen(tag, pause, nr, bilder, kopf_start):
    """Ab Pfeil ab zu Schwer: Kopf je Bild (Betreffzeile wie vor dem Wechsel = Start, wie im letzten Bild
    = Ziel, sonst Schwer). Kein Bild mit dem Inhalt einer anderen Mail als der im Kopf (Start blau,
    Schwer gelb, Ziel grün); am Ende Ziel (grün)."""
    kopf_ziel = bilder[-1][1].crop(BETREFF).tobytes()
    erwartet = {'Start': 'blau', 'Schwer': 'gelb', 'Ziel': 'gruen'}
    alt = {'Schwer': 0, 'Ziel': 0}
    alt_bilder = set()
    ab = {}
    protokoll = []
    schon = {}
    for t, b in bilder:
        # Gleiche Bilder hintereinander sind dasselbe Objekt (aufnahme): je Bild nur einmal rechnen.
        if id(b) not in schon:
            k = b.crop(BETREFF).tobytes()
            schon[id(b)] = (anteile(b), 'Start' if k == kopf_start else 'Ziel' if k == kopf_ziel else 'Schwer')
        a, kopf = schon[id(b)]
        protokoll.append('%.3f kopf=%s blau=%.3f gelb=%.3f gruen=%.3f' % (t, kopf, a['blau'], a['gelb'], a['gruen']))
        ab.setdefault(kopf, t)
        if a['gruen'] > 0.5:
            ab.setdefault('gruen', t)
        if kopf in alt and any(v > 0.01 for f, v in a.items() if f != erwartet[kopf]):
            alt[kopf] += 1
            if id(b) not in alt_bilder and len(alt_bilder) < 3:
                b.save(os.path.join(OUT, '%s-alt-%.2f-%d-%s-%.3f.png' % (NAME, pause, nr, kopf, t)))
            alt_bilder.add(id(b))
    with open(os.path.join(OUT, '%s-bilder-%.2f-%d.txt' % (NAME, pause, nr)), 'w') as f:
        f.write('\n'.join(protokoll) + '\n')
    bilder[-1][1].save(os.path.join(OUT, '%s-ende-%.2f-%d.png' % (NAME, pause, nr)))
    abstand = (bilder[-1][0] - bilder[0][0]) / max(len(bilder) - 1, 1) * 1000
    wert('%s: %d Aufnahmen (alle %.1f ms, %d verschiedene Bilder), Kopf Schwer ab %s, Kopf Ziel ab %s, grün ab %s'
         % (tag, len(bilder), abstand, len(schon), *('%.3f' % ab[k] if k in ab else '-' for k in ('Schwer', 'Ziel', 'gruen'))))
    for kopf in ('Ziel', 'Schwer'):
        (ok if alt[kopf] == 0 else falsch)('%s: %d Aufnahmen mit Inhalt einer anderen Mail unter dem Kopf von %s'
                                           % (tag, alt[kopf], kopf))
    (ok if anteile(bilder[-1][1])['gruen'] > 0.5 else falsch)('%s: Inhalt von Ziel am Ende sichtbar' % tag)


def log_pruefen(tag, ab):
    """Ladung von Ziel: „sichtbar“ und „Gelesen“ (Signal shown) erst nach ihrem „started“."""
    s = geladen(SCHWER, ab, 1)
    z = geladen(ZIEL, ab, 10)
    if s is None or z is None:
        falsch('%s: Log „geladen“ für Schwer (%s) oder Ziel (%s) fehlt' % (tag, SCHWER, ZIEL))
        return
    zeilen = log_zeilen()[ab:]
    i_ziel = next(i for i, (_, text) in enumerate(zeilen) if text == z[1])
    zeigt = next((text.split()[2] for _, text in zeilen[i_ziel:] if text.startswith('Nachricht: zeigt ')), None)
    if zeigt is None:
        falsch('%s: Log „zeigt“ der Ladung von Ziel fehlt' % tag)
        return

    def zeile(muster):
        return next(((i, t, text) for i, (t, text) in enumerate(zeilen) if re.search(muster, text)), None)

    started = zeile(r'^Nachricht: lädt %s \(started\)' % re.escape(zeigt))
    sichtbar = zeile(r'^Nachricht: sichtbar %s \((committed|finished)\)' % re.escape(zeigt))
    uebergangen = zeile(r'^Nachricht: %s vor STARTED übergangen' % re.escape(zeigt))
    gelesen = zeile(r'^Gelesen: %s in ' % (NACHRICHT % ZIEL))
    wert('%s: %s' % (tag, ' | '.join('%.3f %s' % (t, text) for _, t, text in
                                       sorted(filter(None, (started, uebergangen, sichtbar, gelesen))))))
    if started is None or sichtbar is None:
        falsch('%s: Log „started“ oder „sichtbar“ für %s fehlt' % (tag, zeigt))
        return
    (ok if sichtbar[0] > started[0] and sichtbar[1] >= started[1] else falsch)(
        '%s: „%s“ nach „started“ (%+.0f ms)' % (tag, sichtbar[2], (sichtbar[1] - started[1]) * 1000))
    if gelesen is None:
        falsch('%s: Log „Gelesen“ für Ziel (%s) fehlt' % (tag, ZIEL))
        return
    (ok if gelesen[0] > started[0] and gelesen[1] >= sichtbar[1] else falsch)(
        '%s: Gelesen-Timer (shown) nach „started“ und „sichtbar“ (%+.0f ms nach sichtbar)'
        % (tag, (gelesen[1] - sichtbar[1]) * 1000))


def schliessen(app, w):
    w.send_event(protocol.event.ClientMessage(
        window=w, client_type=d.intern_atom('WM_PROTOCOLS'),
        data=(32, [d.intern_atom('WM_DELETE_WINDOW'), X.CurrentTime, 0, 0, 0])), event_mask=X.NoEventMask)
    d.sync()
    try:
        return app.wait(30)
    except subprocess.TimeoutExpired:
        return None


def starten(log):
    """App starten, Start (über REVOLUTION_MESSAGE gewählt) in der Liste suchen: (app, Fenster) oder
    (app, None). Neue Mails aus dem Server-Durchgang stehen nicht immer an ihrem Platz nach Datum
    (Konversationen, #357; Bild NAME-ohne-zeile.png); aus dem Cache beim nächsten Start schon."""
    global ZEILE
    ab = len(log_zeilen())
    app = subprocess.Popen([BIN], stdout=log, stderr=subprocess.STDOUT)
    w = fenster()
    if w is None:
        falsch('kein Fenster')
        return app, None
    w.configure(width=1200, height=760)
    d.sync()
    if warte(r'^Nachrichten: %s vollständig' % re.escape(LOGKEY), ab, 120) is None:
        falsch('Liste nicht geladen (%s)' % ORDNER)
        return app, None
    if geladen(START, ab, 30) is None:
        falsch('Start (UID %s) nicht über REVOLUTION_MESSAGE gewählt' % START)
        return app, None
    time.sleep(1)
    ZEILE = zeile_start()
    if ZEILE is None:
        g = d.screen().root.get_geometry()
        Image.frombytes('RGB', (g.width, g.height), d.screen().root.get_image(
            0, 0, g.width, g.height, X.ZPixmap, 0xffffffff).data, 'raw', 'BGRX').save(
            os.path.join(OUT, NAME + '-ohne-zeile.png'))
        wert('gewählte Zeile nicht im Bild (%s-ohne-zeile.png), Neustart' % NAME)
        wert('Beenden für Neustart: Exit %s' % schliessen(app, w))
        return app, None
    wert('Zeile von Start bei %s' % (ZEILE,))
    return app, w


def main():
    log = open(LOG, 'w')
    app = None
    try:
        app, w = starten(log)
        if w is None and app.poll() is not None:
            app, w = starten(log)
        if w is None:
            falsch('Zeile von Start nicht gefunden')
            return 1
        # Schwer einmal ganz laden: danach im Cache wie in der Messung #354 R4 („geladen nach 6 ms“);
        # sonst holt der Worker sie vom Server und der Wechsel bricht das Holen statt der Anzeige ab.
        ab = len(log_zeilen())
        klick(ZEILE)
        time.sleep(1)
        taste('Down')
        if geladen(SCHWER, ab, 120) is None:
            falsch('Schwer (UID %s) nicht geladen' % SCHWER)
            return 1
        time.sleep(5)
        for pause in PAUSEN:
            for nr in range(1, WDH + 1):
                lauf(pause, nr)
        rc = schliessen(app, w)
        (ok if rc == 0 else falsch)('Beenden: Exit %s' % rc)
    finally:
        if app is not None and app.poll() is None:
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
