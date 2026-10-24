#!/usr/bin/env python3
# konversationen-treiber.py – Treiber für tests/konversationen.sh (#357), läuft im Xvfb aus xvfb-run.
#
# Startet die App (BIN) mit Log im Konversationsmodus (Schema mit conversations=true), wählt die
# Konversation aus tests/testdaten.py --konversation (REVOLUTION_MESSAGE), bedient sie per XTest
# (python3-xlib) und prüft jede Flag-Änderung unabhängig direkt am Server über
# tests/testdaten.py --konversation-flags (nur lesend). Umgebung: siehe tests/konversationen.sh.
# Ausgabe: „OK …“, „FEHLER …“, „WERT …“; Screenshots OUT/NAME-<Schritt>.png.
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

from PIL import Image
from Xlib import X, XK, display, protocol
from Xlib.ext import xtest

BIN, OUT, NAME = os.environ['BIN'], os.environ['OUT'], os.environ['NAME']
KONTO, ORDNER, APP_DIR = os.environ['KONTO'], os.environ['ORDNER'], os.environ['APP_DIR']
NUR_BILD = os.environ.get('NUR_BILD') == '1'
LOGKEY = ORDNER.replace('/', ' ', 1)
BREITE, HOEHE = 1400, 900
# Kopf je Nachricht im Lesebereich der Konversation (neueste oben, nur Nr. 5 aufgeklappt, kurzer Text),
# Knöpfe Gelesen und Markieren rechts daneben (Kalibrierung: NAME-0-kalibrierung.png).
KOPF_X, GELESEN_X, STERN_X = 900, 1332, 1366
KOPF_Y = {5: 145, 4: 321, 3: 383, 2: 445, 1: 507}

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


# ------------------------------------------------------------------ Log der App

LOG = os.path.join(OUT, NAME + '.log')
ZEILE_RE = re.compile(r'^\(revolution:\d+\): revolution-DEBUG: (\d\d):(\d\d):(\d\d\.\d+): (.*)$')


def log_zeilen():
    out = []
    with open(LOG, encoding='utf-8', errors='replace') as f:
        for zeile in f:
            m = ZEILE_RE.match(zeile.rstrip('\n'))
            if m:
                h, mi, s, text = m.groups()
                out.append((int(h) * 3600 + int(mi) * 60 + float(s.replace(',', '.')), text))
    return out


def marke():
    return len(log_zeilen())


def warte(muster, ab, zeit=30):
    """Erste neue Debug-Zeile ab Marke, die muster (regex) enthält: (Zeit, Text) oder None."""
    rx = re.compile(muster)
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        for t, text in log_zeilen()[ab:]:
            if rx.search(text):
                return t, text
        time.sleep(0.2)
    return None


# ------------------------------------------------------------------ Server (unabhängig)

def server_flags():
    """{Nr: (gelesen, markiert, Ordner)} der Konversation direkt vom Server."""
    r = subprocess.run([sys.executable, os.path.join(APP_DIR, 'tests', 'testdaten.py'), '--konversation-flags', KONTO],
                       capture_output=True, text=True, timeout=180)
    out = {}
    for zeile in r.stdout.splitlines():
        m = re.match(r'^KFLAGS (\d) (\d) (\d) (.*)$', zeile)
        if m:
            out[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), m.group(4))
    if len(out) != 5:
        raise RuntimeError('Konversation am Server unvollständig: %s %s' % (r.stdout, r.stderr))
    return out


def pruefe_server(schritt, nr, gelesen=None, markiert=None):
    ist = server_flags()[nr]
    soll = (ist[0] if gelesen is None else gelesen, ist[1] if markiert is None else markiert)
    text = '%s → Server Nr. %d gelesen=%d markiert=%d' % (schritt, nr, ist[0], ist[1])
    (ok if ist[:2] == soll else falsch)(text + ('' if ist[:2] == soll else ' (erwartet %d/%d)' % soll))


# ------------------------------------------------------------------ XTest

def taste(kombi, pause=0.3):
    codes = [d.keysym_to_keycode(XK.string_to_keysym(k)) for k in kombi.split('+')]
    for c in codes:
        xtest.fake_input(d, X.KeyPress, c)
    for c in reversed(codes):
        xtest.fake_input(d, X.KeyRelease, c)
    d.sync()
    time.sleep(pause)


def klick(p):
    xtest.fake_input(d, X.MotionNotify, x=p[0], y=p[1])
    xtest.fake_input(d, X.ButtonPress, 1)
    xtest.fake_input(d, X.ButtonRelease, 1)
    d.sync()
    time.sleep(0.5)


def bild(schritt):
    xtest.fake_input(d, X.MotionNotify, x=0, y=0)
    d.sync()
    time.sleep(0.5)
    root = d.screen().root
    raw = root.get_image(0, 0, BREITE, HOEHE, X.ZPixmap, 0xffffffff).data
    pfad = os.path.join(OUT, '%s-%s.png' % (NAME, schritt))
    Image.frombytes('RGB', (BREITE, HOEHE), raw, 'raw', 'BGRX').save(pfad)
    wert('Screenshot %s' % pfad)


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


# ------------------------------------------------------------------ Schritte

def datum(nr):
    """Datum der Mail Nr. wie tests/testdaten.py konversation_datum (Unix-Zeit)."""
    return int((datetime.datetime(2026, 10, 23, 8, 0, tzinfo=datetime.timezone(datetime.timedelta(hours=2)))
                + datetime.timedelta(minutes=17 * (nr - 1))).timestamp())


def gruppierung(ab, server):
    """5 Nachrichten in einer Konversation, je Nachricht Datum und Ordner wie am Server."""
    t = warte(r'^Konversation: \S+ mit (\d+) Nachrichten angezeigt', ab, 120)
    if t is None:
        falsch('Konversation nicht angezeigt (Log „Konversation: … angezeigt“ fehlt)')
        return False
    (ok if ' mit 5 Nachrichten ' in t[1] else falsch)('Gruppierung: %s' % t[1])
    eintraege = {}
    for _, z in log_zeilen()[ab:]:
        m = re.match(r'^Konversation: Nachricht (\S+) (\d+) aus (\S+?)/(.*)$', z)
        if m:
            eintraege[int(m.group(2))] = (m.group(1), m.group(4))
    for nr in range(1, 6):
        app = eintraege.get(datum(nr))
        soll = server[nr][2]
        (ok if app is not None and app[1] == soll else falsch)(
            'Nr. %d (%s): App %s, Server-Ordner %s' % (nr, datetime.datetime.fromtimestamp(datum(nr)).strftime('%H:%M'),
                                                       '%s aus %s' % app if app else 'fehlt', soll))
    gemischt = len({o for _, o in eintraege.values()})
    (ok if gemischt == 2 else falsch)('Eingang und Gesendet gemischt: %d Ordner in der Konversation' % gemischt)
    return True


def flag_klick(schritt, p, was, nr, **soll):
    ab = marke()
    klick(p)
    treffer = warte(r'^Flags: %s \S+ %s gespeichert nach' % (re.escape(LOGKEY), re.escape(was)), ab)
    if treffer is None:
        falsch('%s: Log „Flags: … %s gespeichert“ fehlt' % (schritt, was))
        return
    ok('%s: %s' % (schritt, treffer[1]))
    pruefe_server(schritt, nr, **soll)


def gelesen_timer(schritt, ab):
    start = warte(r'^Gelesen: %s \S+ in 1500 ms' % re.escape(LOGKEY), ab, 20)
    gesetzt = warte(r'^Flags: %s \S+ gelesen \(automatisch\)$' % re.escape(LOGKEY), ab, 20)
    ende = warte(r'^Flags: %s \S+ gelesen \(automatisch\) gespeichert' % re.escape(LOGKEY), ab, 40)
    if start is None or gesetzt is None or ende is None:
        falsch('%s: Gelesen nach Verzögerung fehlt im Log' % schritt)
        return
    abstand = (gesetzt[0] - start[0]) * 1000
    (ok if 1450 <= abstand <= 2500 else falsch)('%s: gelesen nach %.0f ms (Evolution 1500 ms)' % (schritt, abstand))
    pruefe_server(schritt, 5, gelesen=1)


def konversation(ab):
    vorher = server_flags()
    wert('Server vorher: %s' % sorted(vorher.items()))
    if not gruppierung(ab, vorher):
        return vorher
    time.sleep(2)
    bild('0-kalibrierung')

    # Gelesen-Timer je Nachricht: Nr. 5 ist offen; ungelesen → nach 1,5 s gelesen. Sonst erst per Knopf
    # ungelesen, dann zu- und aufklappen (neu angezeigt).
    if vorher[5][0] == 0:
        gelesen_timer('Gelesen-Timer Nr. 5', ab)
    else:
        flag_klick('Knopf Gelesen Nr. 5', (GELESEN_X, KOPF_Y[5]), 'ungelesen', 5, gelesen=0)
        ab2 = marke()
        klick((KOPF_X, KOPF_Y[5]))
        warte(r'^Konversation: \S+ zugeklappt', ab2, 5)
        klick((KOPF_X, KOPF_Y[5]))
        (ok if warte(r'^Konversation: \S+ aufgeklappt', ab2, 5) else falsch)('Nr. 5 zu- und aufgeklappt')
        gelesen_timer('Gelesen-Timer Nr. 5 (neu angezeigt)', ab2)
    bild('1-gelesen')

    flag_klick('Stern Nr. 3', (STERN_X, KOPF_Y[3]), 'markiert', 3, markiert=1)
    bild('2-markiert')
    flag_klick('Stern Nr. 3 aus', (STERN_X, KOPF_Y[3]), 'nicht markiert', 3, markiert=0)
    flag_klick('Knopf Gelesen Nr. 1', (GELESEN_X, KOPF_Y[1]), 'ungelesen', 1, gelesen=0)
    flag_klick('Knopf Gelesen Nr. 1 zurück', (GELESEN_X, KOPF_Y[1]), 'gelesen', 1, gelesen=1)

    # Aufklappen: HTML mit entferntem Bild und Anhang (Nr. 3), eigene WebView mit Höhe des Inhalts.
    ab = marke()
    klick((KOPF_X, KOPF_Y[3]))
    auf = warte(r'^Konversation: \S+ aufgeklappt', ab, 5)
    geladen = warte(r'^Nachricht: \S+ \S+ \S+ geladen nach .*HTML.* 1 Anhänge\)', ab, 60)
    gesperrt = warte(r'^Entfernte Inhalte: gesperrt', ab, 30)
    hoehe = warte(r'^Nachricht: Höhe \d+', ab, 30)
    (ok if auf else falsch)('Nr. 3 aufgeklappt: %s' % (auf[1] if auf else 'fehlt'))
    (ok if geladen else falsch)('Nr. 3 geladen: %s' % (geladen[1] if geladen else 'fehlt'))
    (ok if gesperrt else falsch)('Nr. 3 entfernte Inhalte gesperrt: %s' % (gesperrt[1] if gesperrt else 'fehlt'))
    (ok if hoehe else falsch)('Nr. 3 Höhe nach Inhalt: %s' % (hoehe[1] if hoehe else 'fehlt'))
    time.sleep(1)
    bild('3-aufgeklappt')
    ab = marke()
    klick((KOPF_X, KOPF_Y[3]))
    zu = warte(r'^Konversation: \S+ zugeklappt', ab, 5)
    (ok if zu else falsch)('Nr. 3 zugeklappt (Lesebereich und WebView entfernt): %s' % (zu[1] if zu else 'fehlt'))
    bild('4-zugeklappt')

    # Zeile der Konversation: Markieren gilt für alle Nachrichten wie ein zugeklappter Thread in Evolution.
    ab = marke()
    taste('Control_L+Shift_L+l')
    ende = time.monotonic() + 60
    while time.monotonic() < ende and sum(' markiert gespeichert nach' in z for _, z in log_zeilen()[ab:]) < 5:
        time.sleep(0.2)
    n = sum(' markiert gespeichert nach' in z for _, z in log_zeilen()[ab:])
    (ok if n == 5 else falsch)('Umschalt+Strg+L an der Konversation: %d Nachrichten markiert' % n)
    s = server_flags()
    (ok if all(s[i][1] == 1 for i in s) else falsch)('Server: alle 5 markiert (%s)' % [s[i][1] for i in sorted(s)])
    ab = marke()
    taste('Control_L+Shift_L+l')
    ende = time.monotonic() + 60
    while time.monotonic() < ende and sum('nicht markiert gespeichert nach' in z for _, z in log_zeilen()[ab:]) < 5:
        time.sleep(0.2)
    s = server_flags()
    (ok if all(s[i][1] == 0 for i in s) else falsch)('Server: alle 5 nicht markiert (%s)' % [s[i][1] for i in sorted(s)])

    # Umschalter Konversation/flach (Strg+T).
    ab = marke()
    taste('Control_L+t')
    aus = warte(r'^Konversationen: aus', ab, 5)
    flach = warte(r'^Nachrichten: %s vollständig (\d+)' % re.escape(LOGKEY), ab, 120)
    (ok if aus and flach else falsch)('Strg+T flach: %s; %s' % (aus[1] if aus else '-', flach[1] if flach else '-'))
    time.sleep(2)
    bild('5-flach')
    ab = marke()
    taste('Control_L+t')
    an = warte(r'^Konversationen: an', ab, 5)
    wieder = warte(r'^Konversation: \S+ mit 5 Nachrichten angezeigt', ab, 120)
    konv = warte(r'^Nachrichten: %s vollständig (\d+)' % re.escape(LOGKEY), ab, 120)
    (ok if an and wieder else falsch)('Strg+T Konversation: %s; %s' % (an[1] if an else '-', wieder[1] if wieder else '-'))
    if flach and konv:
        nf = int(re.search(r'vollständig (\d+)', flach[1]).group(1))
        nk = int(re.search(r'vollständig (\d+)', konv[1]).group(1))
        (ok if nk < nf else falsch)('Zeilen flach %d, als Konversationen %d' % (nf, nk))
    time.sleep(2)
    bild('6-wieder-konversation')

    # Ausgangszustand Nr. 5 (gelesen) herstellen.
    if vorher[5][0] == 0:
        flag_klick('Knopf Gelesen Nr. 5 (Ausgangszustand)', (GELESEN_X, KOPF_Y[5]), 'ungelesen', 5, gelesen=0)
    return vorher


def nur_bild(ab):
    """Screenshot mit aufgeklappter HTML-Nachricht (Nr. 3), z. B. dunkel."""
    if warte(r'^Konversation: \S+ mit 5 Nachrichten angezeigt', ab, 120) is None:
        falsch('Konversation nicht angezeigt')
        return
    time.sleep(2)
    ab = marke()
    klick((KOPF_X, KOPF_Y[3]))
    hoehe = warte(r'^Nachricht: Höhe \d+', ab, 60)
    (ok if hoehe else falsch)('Nr. 3 Höhe nach Inhalt: %s' % (hoehe[1] if hoehe else 'fehlt'))
    time.sleep(1.5)
    bild('aufgeklappt')
    # Mausrad über dem Inhalt (WebView von Nr. 3): die Konversation scrollt als Ganzes (Bild ändert sich).
    vorher = Image.open(os.path.join(OUT, '%s-aufgeklappt.png' % NAME))
    xtest.fake_input(d, X.MotionNotify, x=1000, y=700)
    for _ in range(5):
        xtest.fake_input(d, X.ButtonPress, 5)
        xtest.fake_input(d, X.ButtonRelease, 5)
    d.sync()
    time.sleep(1)
    root = d.screen().root
    raw = root.get_image(0, 0, BREITE, HOEHE, X.ZPixmap, 0xffffffff).data
    nachher = Image.frombytes('RGB', (BREITE, HOEHE), raw, 'raw', 'BGRX')
    nachher.save(os.path.join(OUT, '%s-gescrollt.png' % NAME))
    bereich = (700, 120, BREITE, HOEHE)
    anders = sum(a != b for a, b in zip(vorher.crop(bereich).getdata(), nachher.crop(bereich).getdata()))
    (ok if anders > 1000 else falsch)('Mausrad über der WebView scrollt die Konversation (%d Pixel geändert)' % anders)


def main():
    log = open(LOG, 'w')
    app = subprocess.Popen([BIN], stdout=log, stderr=subprocess.STDOUT)
    vorher = None
    try:
        w = fenster()
        if w is None:
            falsch('kein Fenster')
            return 1
        w.configure(width=BREITE, height=HOEHE)
        d.sync()
        if NUR_BILD:
            nur_bild(0)
        else:
            vorher = konversation(0)
        w.send_event(protocol.event.ClientMessage(
            window=w, client_type=d.intern_atom('WM_PROTOCOLS'),
            data=(32, [d.intern_atom('WM_DELETE_WINDOW'), X.CurrentTime, 0, 0, 0])), event_mask=X.NoEventMask)
        d.sync()
        try:
            rc = app.wait(30)
            (ok if rc == 0 else falsch)('Beenden: Exit %d' % rc)
        except subprocess.TimeoutExpired:
            falsch('Beenden: nach 30 s nicht beendet')
        if vorher is not None:
            nachher = server_flags()
            (ok if nachher == vorher else falsch)('Server nach dem Lauf wie vorher: %s' % sorted(nachher.items()))
    finally:
        if app.poll() is None:
            app.kill()
            app.wait()
        log.close()
    rest = [z for z in open(LOG, encoding='utf-8', errors='replace')
            if not z.startswith('(revolution:') and not z.startswith('libEGL warning:') and z.strip()]
    # Webprozess (#355): „(process:N): revolution-DEBUG: Webprozess: …“ ist ebenfalls DEBUG der App.
    rest = [z for z in rest if not re.match(r'^\(process:\d+\): revolution-DEBUG: ', z)]
    rest += [z for z in open(LOG, encoding='utf-8', errors='replace')
             if z.startswith('(revolution:') and 'revolution-DEBUG' not in z]
    (falsch if rest else ok)('Log: %s' % ('%d unerwartete Zeilen: %s' % (len(rest), rest[:3]) if rest else 'nur DEBUG-Zeilen'))
    return 1 if fehler else 0


if __name__ == '__main__':
    sys.exit(main())
