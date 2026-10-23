#!/usr/bin/env python3
# flags-treiber.py – Treiber für tests/flags.sh (#353), läuft im Xvfb aus xvfb-run.
#
# Startet die App (BIN) mit Log, bedient sie per XTest (python3-xlib) und prüft jede Änderung
# direkt am Server über tests/testdaten.py --flags (nur lesend). Umgebung: siehe tests/flags.sh.
# Ausgabe: „OK …“, „FEHLER …“, „WERT …“; Screenshots OUT/NAME-<Schritt>.png.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

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
LOGKEY = ORDNER.replace('/', ' ', 1)
EINLADUNG = '[Revolution-Test] Einladung: Abstimmung Testdaten'
IMAP = 'outlook' not in KONTO
# Fenster 1200×760: erste Zeile der Liste, Stern-Knopf der Nachricht (Screenshot m4-353 probe.png).
ZEILE = (420, 108)
STERN = (962, 25)
ZEILE_HOEHE = 108

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
    """(gelesen, markiert) der Einladung direkt vom Server."""
    r = subprocess.run([sys.executable, os.path.join(APP_DIR, 'tests', 'testdaten.py'), '--flags', KONTO],
                       capture_output=True, text=True, timeout=180)
    for zeile in r.stdout.splitlines():
        m = re.match(r'^FLAGS (\d) (\d) (.*?)( \(flag\.flagStatus=.*\))?$', zeile)
        if m and m.group(3) == EINLADUNG:
            return int(m.group(1)), int(m.group(2))
    raise RuntimeError('Einladung nicht gefunden: %s %s' % (r.stdout, r.stderr))


def server_aendern(aktion, *ordner):
    r = subprocess.run([sys.executable, os.path.join(APP_DIR, 'tests', 'testdaten.py'), '--server', KONTO, aktion, *ordner],
                       capture_output=True, text=True, timeout=180)
    wert('Server %s: %s' % (aktion, (r.stdout + r.stderr).strip().replace('\n', ' | ')))


def pruefe_server(schritt, gelesen=None, markiert=None):
    ist = server_flags()
    soll = (ist[0] if gelesen is None else gelesen, ist[1] if markiert is None else markiert)
    text = '%s → Server gelesen=%d markiert=%d' % (schritt, ist[0], ist[1])
    (ok if ist == soll else falsch)(text + ('' if ist == soll else ' (erwartet %d/%d)' % soll))
    return ist


# ------------------------------------------------------------------ XTest

def taste(kombi):
    codes = [d.keysym_to_keycode(XK.string_to_keysym(k)) for k in kombi.split('+')]
    for c in codes:
        xtest.fake_input(d, X.KeyPress, c)
    for c in reversed(codes):
        xtest.fake_input(d, X.KeyRelease, c)
    d.sync()
    time.sleep(0.3)


def klick(p, knopf=1, mit=None):
    xtest.fake_input(d, X.MotionNotify, x=p[0], y=p[1])
    if mit:
        xtest.fake_input(d, X.KeyPress, d.keysym_to_keycode(XK.string_to_keysym(mit)))
    xtest.fake_input(d, X.ButtonPress, knopf)
    xtest.fake_input(d, X.ButtonRelease, knopf)
    if mit:
        xtest.fake_input(d, X.KeyRelease, d.keysym_to_keycode(XK.string_to_keysym(mit)))
    d.sync()
    time.sleep(0.5)


def ziehen(von, bis, schritte=12, dauer=0.12):
    """Maus-Ziehen wie ein schnelles Wischen (Taste 1 gedrückt, gleichmäßige Bewegung)."""
    xtest.fake_input(d, X.MotionNotify, x=von[0], y=von[1])
    xtest.fake_input(d, X.ButtonPress, 1)
    d.sync()
    for i in range(1, schritte + 1):
        time.sleep(dauer / schritte)
        xtest.fake_input(d, X.MotionNotify, x=int(von[0] + (bis[0] - von[0]) * i / schritte),
                         y=int(von[1] + (bis[1] - von[1]) * i / schritte))
        d.sync()
    xtest.fake_input(d, X.ButtonRelease, 1)
    d.sync()
    time.sleep(0.5)


def bild(schritt):
    # Zeiger aus dem Bild (Xvfb zeichnet ihn mit) und ohne Hover-Zustand.
    xtest.fake_input(d, X.MotionNotify, x=0, y=0)
    d.sync()
    time.sleep(0.3)
    root = d.screen().root
    g = root.get_geometry()
    raw = root.get_image(0, 0, g.width, g.height, X.ZPixmap, 0xffffffff).data
    pfad = os.path.join(OUT, '%s-%s.png' % (NAME, schritt))
    Image.frombytes('RGB', (g.width, g.height), raw, 'raw', 'BGRX').save(pfad)
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

def flag_schritt(schritt, aktion, was, gelesen=None, markiert=None):
    ab = marke()
    aktion()
    treffer = warte(r'^Flags: %s \S+ %s gespeichert nach' % (re.escape(LOGKEY), re.escape(was)), ab)
    if treffer is None:
        falsch('%s: Log „Flags: … %s gespeichert“ fehlt' % (schritt, was))
        return
    ok('%s: %s' % (schritt, treffer[1]))
    warte(r'Änderung', ab, 5)
    pruefe_server(schritt, gelesen, markiert)


def gelesen_verzoegert(schritt):
    """Auswahl einer ungelesenen Zeile: Gelesen erst nach mark-seen-timeout (1500 ms)."""
    ab = marke()
    klick(ZEILE)
    start = warte(r'^Gelesen: %s \S+ in 1500 ms' % re.escape(LOGKEY), ab, 10)
    ende = warte(r'^Flags: %s \S+ gelesen \(automatisch\) gespeichert' % re.escape(LOGKEY), ab, 30)
    gesetzt = warte(r'^Flags: %s \S+ gelesen \(automatisch\)$' % re.escape(LOGKEY), ab, 1)
    if start is None or ende is None or gesetzt is None:
        falsch('%s: Gelesen nach Verzögerung fehlt im Log' % schritt)
        return
    abstand = (gesetzt[0] - start[0]) * 1000
    (ok if 1450 <= abstand <= 2500 else falsch)('%s: Auswahl → gelesen nach %.0f ms (Evolution 1500 ms)' % (schritt, abstand))
    pruefe_server(schritt, gelesen=1)


def server_live(schritt, aktion, muster, f5, *ordner):
    """Serverseitige Änderung; f5: Liste mit F5 aktualisieren, sonst nur auf IDLE warten."""
    ab = marke()
    server_aendern(aktion, *ordner)
    weg = 'IDLE'
    treffer = None if f5 else warte(muster, ab, 120)
    # Gmail meldet eine Änderung über eine andere Verbindung teils erst nach Sekunden (beobachtet:
    # erstes F5 ohne Änderung); daher bis zu dreimal F5 im Abstand von 5 s, Anzahl im Ergebnis.
    for nr in range(1, 4 if f5 else 1):
        weg = 'F5 (%d.)' % nr
        ab2 = marke()
        taste('F5')
        if warte(r'^Aktualisieren: %s fertig' % re.escape(LOGKEY), ab2, 60) is None:
            falsch('%s: „Aktualisieren: … fertig“ fehlt' % schritt)
        treffer = warte(muster, ab, 5)
        if treffer is not None:
            break
    if treffer is None:
        falsch('%s: Liste zeigt die Serveränderung nicht (Log %s)' % (schritt, muster))
    else:
        ok('%s über %s: %s' % (schritt, weg, treffer[1]))


def testkonto():
    vorher = server_flags()
    wert('Ausgangszustand Server: gelesen=%d markiert=%d' % vorher)

    # Auswahl: ist die Einladung ungelesen, setzt die App „gelesen“ nach 1,5 s.
    if vorher[0] == 0:
        gelesen_verzoegert('Auswahl')
    else:
        klick(ZEILE)
        time.sleep(2)
        pruefe_server('Auswahl (schon gelesen)', gelesen=1)
    bild('1-vorher')

    flag_schritt('Strg+Umschalt+K', lambda: taste('Control_L+Shift_L+k'), 'ungelesen', gelesen=0)
    bild('2-nachher-ungelesen')
    flag_schritt('Strg+K', lambda: taste('Control_L+k'), 'gelesen', gelesen=1)
    flag_schritt('Strg+S', lambda: taste('Control_L+s'), 'markiert', markiert=1)
    bild('3-markiert')
    flag_schritt('Strg+Umschalt+S', lambda: taste('Control_L+Shift_L+s'), 'nicht markiert', markiert=0)

    def kontextmenue():
        klick(ZEILE, knopf=3)
        time.sleep(0.5)
        bild('4-kontextmenue')
        # Der erste sichtbare Eintrag hat den Fokus (Screenshot): „Als ungelesen markieren“.
        taste('Return')
    flag_schritt('Kontextmenü „Als ungelesen markieren“', kontextmenue, 'ungelesen', gelesen=0)

    klick(ZEILE, mit='Control_L')
    time.sleep(0.5)
    gelesen_verzoegert('Auswahl neu')

    links, rechts = (300, ZEILE[1]), (530, ZEILE[1])
    flag_schritt('Wischen rechts', lambda: ziehen(links, rechts), 'ungelesen (Wischen)', gelesen=0)
    bild('5-gewischt-ungelesen')
    flag_schritt('Wischen rechts', lambda: ziehen(links, rechts), 'gelesen (Wischen)', gelesen=1)
    flag_schritt('Wischen links', lambda: ziehen(rechts, links), 'markiert (Wischen)', markiert=1)
    flag_schritt('Wischen links', lambda: ziehen(rechts, links), 'nicht markiert (Wischen)', markiert=0)

    flag_schritt('Stern-Knopf', lambda: klick(STERN), 'markiert', markiert=1)
    bild('6-stern-knopf')
    flag_schritt('Stern-Knopf', lambda: klick(STERN), 'nicht markiert', markiert=0)

    geaendert = r'^Nachrichten: %s Änderung 0 neu, 0 entfernt, 1 geändert' % re.escape(LOGKEY)
    # imapx hält IDLE nur am Posteingang (camel-imapx-conn-manager.c:1266-1285), Änderungen in
    # Revolution-Test kommen daher mit F5; IDLE im Lauf -idle (Posteingang von privat@).
    server_live('Server markiert', 'markiert', geaendert, True)
    time.sleep(0.5)
    bild('7-server-markiert')
    server_live('Server unmarkiert', 'unmarkiert', geaendert, True)
    if 'privat' in KONTO:
        server_live('Server neue Mail (APPEND)', 'neu',
                    r'^Nachrichten: %s Änderung 1 neu, 0 entfernt' % re.escape(LOGKEY), True)
        time.sleep(0.5)
        bild('8-server-neu')
        # Grenze des Unterbaus: imapx übergeht EXPUNGE außerhalb von IDLE (EDS camel-imapx-server.c:
        # 789-820), refresh_info am gewählten Ordner schickt nur NOOP (5734-5735); eine anderswo
        # gelöschte Mail bleibt hier bis zum nächsten Start stehen. Löschen per IDLE: Lauf -idle.
        server_aendern('weg')

    # Ausgangszustand wiederherstellen (über die App).
    jetzt = server_flags()
    if jetzt[0] != vorher[0]:
        flag_schritt('Ausgangszustand', lambda: taste('Control_L+Shift_L+k' if vorher[0] == 0 else 'Control_L+k'),
                     'ungelesen' if vorher[0] == 0 else 'gelesen', gelesen=vorher[0])
    if jetzt[1] != vorher[1]:
        flag_schritt('Ausgangszustand', lambda: taste('Control_L+s' if vorher[1] else 'Control_L+Shift_L+s'),
                     'markiert' if vorher[1] else 'nicht markiert', markiert=vorher[1])
    return vorher


def lastordner():
    """Revolution-Last (3.000 Mails): Auswahl und Scrollposition bleiben bei Flag-Änderung."""
    # Mausrad statt Bild ab: scrollt, ohne die Auswahl zu ändern (sonst liefe der Gelesen-Timer).
    for _ in range(30):
        klick(ZEILE, knopf=5)
    zeile = (420, 108 + 2 * ZEILE_HOEHE)
    ab = marke()
    klick(zeile)
    time.sleep(2.5)
    # Eine Taste ohne Wirkung, damit GTK wie nach den Kürzeln den Tastatur-Fokusring zeigt
    # (focus-visible); sonst unterschiede sich nur der Rahmen der gewählten Zeile.
    taste('Escape')
    bild('1-vorher')
    treffer = warte(r'^Flags: %s (\S+) gelesen \(automatisch\) gespeichert' % re.escape(LOGKEY), ab, 1)
    if treffer:
        wert('gewählte Zeile war ungelesen: %s' % treffer[1])
    ab = marke()
    taste('Control_L+Shift_L+k')
    t = warte(r'^Flags: %s (\S+) ungelesen gespeichert' % re.escape(LOGKEY), ab)
    (ok if t else falsch)('Strg+Umschalt+K in der Mitte der Liste: %s' % (t[1] if t else 'fehlt'))
    time.sleep(0.5)
    bild('2-nachher')
    taste('Shift_L+F10')
    time.sleep(0.5)
    bild('3-kontextmenue-tastatur')
    taste('Escape')
    ab = marke()
    taste('Control_L+k')
    t = warte(r'^Flags: %s (\S+) gelesen gespeichert' % re.escape(LOGKEY), ab)
    (ok if t else falsch)('Strg+K (zurück): %s' % (t[1] if t else 'fehlt'))
    time.sleep(0.5)
    bild('4-zurueck')
    # Scrollposition: alle übrigen Zeilen der Liste pixelgleich (ohne die gewählte Zeile, deren Schrift
    # zwischen fett/normal wechselt, und ohne die Spalte der Overlay-Bildlaufleiste). Auswahl: die
    # Akzentfläche der gewählten Zeile ist in beiden Bildern an derselben Stelle.
    a = Image.open(os.path.join(OUT, NAME + '-1-vorher.png')).convert('RGB')
    oben, unten = zeile[1] - ZEILE_HOEHE // 2, zeile[1] + ZEILE_HOEHE // 2
    punkt = (280, zeile[1])
    for name, text in (('-2-nachher.png', 'ungelesen'), ('-4-zurueck.png', 'wieder gelesen')):
        b = Image.open(os.path.join(OUT, NAME + name)).convert('RGB')
        gleich = all(a.crop((270, y0, 558, y1)).tobytes() == b.crop((270, y0, 558, y1)).tobytes()
                     for y0, y1 in ((50, oben), (unten, 760)))
        (ok if gleich else falsch)('Scrollposition: übrige Zeilen vorher = %s pixelgleich' % text)
        auswahl = a.getpixel(punkt) == b.getpixel(punkt) != a.getpixel((268, zeile[1]))
        (ok if auswahl else falsch)('Auswahl: gewählte Zeile vorher und %s markiert (Farbe %s bei %s)'
                                    % (text, b.getpixel(punkt), punkt))
    if treffer:
        ab = marke()
        taste('Control_L+Shift_L+k')
        t = warte(r'^Flags: %s (\S+) ungelesen gespeichert' % re.escape(LOGKEY), ab)
        (ok if t else falsch)('Ausgangszustand (wieder ungelesen): %s' % (t[1] if t else 'fehlt'))


def idle():
    """Posteingang von privat@: neue Mail und Löschen am Server erscheinen ohne F5 (IMAP IDLE)."""
    server_live('IDLE neue Mail (APPEND)', 'neu', r'^Nachrichten: %s Änderung 1 neu, 0 entfernt' % re.escape(LOGKEY),
                False, 'INBOX')
    time.sleep(0.5)
    bild('1-neu')
    server_live('IDLE Mail gelöscht', 'weg', r'^Nachrichten: %s Änderung 0 neu, 1 entfernt' % re.escape(LOGKEY),
                False, 'INBOX')
    bild('2-geloescht')
    if marke() and not [t for t in log_zeilen() if t[1].startswith('Aktualisieren:')]:
        ok('ohne F5 (kein „Aktualisieren:“ im Log)')


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
        if warte(r'^Nachrichten: %s vollständig' % re.escape(LOGKEY), 0, 120) is None:
            falsch('Liste nicht geladen (%s)' % ORDNER)
            return 1
        ok('Liste geladen: %s' % ORDNER)
        time.sleep(1)
        if NAME.endswith('-last'):
            lastordner()
        elif NAME.endswith('-idle'):
            idle()
        else:
            vorher = testkonto()
        w.send_event(protocol.event.ClientMessage(
            window=w, client_type=d.intern_atom('WM_PROTOCOLS'),
            data=(32, [d.intern_atom('WM_DELETE_WINDOW'), X.CurrentTime, 0, 0, 0])), event_mask=X.NoEventMask)
        d.sync()
        try:
            rc = app.wait(30)
            (ok if rc == 0 else falsch)('Beenden: Exit %d' % rc)
        except subprocess.TimeoutExpired:
            falsch('Beenden: nach 30 s nicht beendet')
        if not NAME.endswith(('-last', '-idle')):
            nachher = server_flags()
            (ok if nachher == vorher else falsch)('Server nach dem Lauf gelesen=%d markiert=%d, vorher %d/%d'
                                                  % (nachher + vorher))
    finally:
        if app.poll() is None:
            app.kill()
            app.wait()
        log.close()
    rest = [z for z in open(LOG, encoding='utf-8', errors='replace')
            if not z.startswith('(revolution:') and not z.startswith('libEGL warning:')]
    rest += [z for z in open(LOG, encoding='utf-8', errors='replace')
             if z.startswith('(revolution:') and 'revolution-DEBUG' not in z]
    (falsch if rest else ok)('Log: %s' % ('%d unerwartete Zeilen: %s' % (len(rest), rest[:3]) if rest else 'nur DEBUG-Zeilen'))
    return 1 if fehler else 0


if __name__ == '__main__':
    sys.exit(main())
