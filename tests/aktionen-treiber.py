#!/usr/bin/env python3
# aktionen-treiber.py – Treiber für tests/aktionen.sh (#363), läuft im Xvfb aus xvfb-run.
#
# Startet die App (BIN), bedient sie per XTest und AT-SPI (Menüeintrag, Ordner der Seitenleiste,
# Knopf „Rückgängig“) und prüft jede Aktion am Server über tests/testdaten.py --ort (IMAP bzw. Graph,
# unabhängig von Camel). Umgebung: siehe tests/aktionen.sh. microsoft365: Graph-Anfragen (M365_DEBUG=1)
# nur als „[graph] METHODE Pfad“ im Log (ohne Kopfzeilen, kein Token).
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import re
import subprocess
import sys
import threading
import time
import traceback

import gi
from PIL import Image
from Xlib import X, XK, display, protocol
from Xlib.ext import xtest

gi.require_version('Atspi', '2.0')
from gi.repository import Atspi  # noqa: E402

BIN, OUT, NAME, APP_DIR = os.environ['BIN'], os.environ['OUT'], os.environ['NAME'], os.environ['APP_DIR']
KONTO, ORDNER, WAS = os.environ['KONTO'], os.environ['ORDNER'], os.environ['WAS']
LOGKEY = ORDNER.replace('/', ' ', 1)
HEIM = ORDNER.split('/', 1)[1]
M365 = 'outlook' in KONTO
ZEILE = (420, 156)
LOG = os.path.join(OUT, NAME + '.log')
ZEILE_RE = re.compile(r'^\(revolution:\d+\): revolution-DEBUG: \d\d:\d\d:\d\d\.\d+: (.*)$')

fehler = 0
d = display.Display()
FENSTER = None
app_pid = None


def ok(text):
    print('OK      %s: %s' % (NAME, text), flush=True)


def falsch(text):
    global fehler
    fehler += 1
    print('FEHLER  %s: %s' % (NAME, text), flush=True)


def wert(text):
    print('WERT    %s: %s' % (NAME, text), flush=True)


def log_zeilen():
    with open(LOG, encoding='utf-8', errors='replace') as f:
        return [m.group(1) for m in map(ZEILE_RE.match, f) if m]


def warte(muster, ab, zeit=60):
    rx = re.compile(muster)
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        for text in log_zeilen()[ab:]:
            if rx.search(text):
                return text
        time.sleep(0.2)
    return None


def filtern(quelle, ziel):
    """App-Ausgabe ins Log; M365_DEBUG: nur Methode und Pfad (Muster tests/verschieben.sh)."""
    for zeile in iter(quelle.readline, ''):
        g = re.match(r'^> (GET|POST|PATCH|PUT|DELETE) (\S+)', zeile)
        if g:
            ziel.write('[graph] %s %s\n' % (g.group(1), g.group(2).split('?')[0]))
        elif zeile.startswith('> {'):
            for methode, url in re.findall(r'"method"\s*:\s*"([A-Z]+)"\s*,\s*"url"\s*:\s*"([^"?]*)', zeile):
                ziel.write('[graph] %s %s (batch)\n' % (methode, url))
        elif not zeile.startswith(('>', '<')) and 'Bearer' not in zeile and zeile.strip():
            ziel.write(zeile)
        ziel.flush()


def ort():
    """[(Nr, Ordner)] am Server (tests/testdaten.py --ort), sortiert."""
    r = subprocess.run([sys.executable, os.path.join(APP_DIR, 'tests', 'testdaten.py'), '--ort', KONTO, WAS],
                       capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        raise RuntimeError('testdaten.py --ort: %s' % r.stderr.strip()[-300:])
    return sorted((z.split(' ')[1], z[4:].split(' ', 1)[1].rsplit(' ', 1)[0], z.rsplit(' ', 1)[1])
                  for z in r.stdout.splitlines() if z.startswith('ORT '))


def kurz(name):
    return name.rsplit('/', 1)[-1] if M365 else name


def taste(kombi, pause=0.5):
    codes = [d.keysym_to_keycode(XK.string_to_keysym(k)) for k in kombi.split('+')]
    for c in codes:
        xtest.fake_input(d, X.KeyPress, c)
    for c in reversed(codes):
        xtest.fake_input(d, X.KeyRelease, c)
    d.sync()
    time.sleep(pause)


def klick(p, pause=0.8):
    xtest.fake_input(d, X.MotionNotify, x=int(p[0]), y=int(p[1]))
    xtest.fake_input(d, X.ButtonPress, 1)
    xtest.fake_input(d, X.ButtonRelease, 1)
    d.sync()
    time.sleep(pause)


def ziehen(von, bis, schritte=40, dauer=1.6):
    """Langsames Ziehen (Ziehen und Ablegen, nicht Wischen)."""
    xtest.fake_input(d, X.MotionNotify, x=von[0], y=von[1])
    xtest.fake_input(d, X.ButtonPress, 1)
    d.sync()
    for i in range(1, schritte + 1):
        time.sleep(dauer / schritte)
        xtest.fake_input(d, X.MotionNotify, x=int(von[0] + (bis[0] - von[0]) * i / schritte),
                         y=int(von[1] + (bis[1] - von[1]) * i / schritte))
        d.sync()
    for i in range(6):  # über dem Ziel: XDND-Status abwarten
        time.sleep(0.1)
        xtest.fake_input(d, X.MotionNotify, x=bis[0] + i % 2, y=bis[1])
        d.sync()
    xtest.fake_input(d, X.ButtonRelease, 1)
    d.sync()
    time.sleep(0.8)


def bild(schritt):
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


def a11y_finden(rolle, name, o=None):
    """Erster Knoten der App mit Rolle und Name (Ende des Namens) im Barrierefreiheitsbaum (#431)."""
    if o is None:
        desktop = Atspi.get_desktop(0)
        apps = [desktop.get_child_at_index(i) for i in range(desktop.get_child_count())]
        o = next((a for a in apps if a is not None and a.get_process_id() == app_pid), None)
        if o is None:
            return None
    if o.get_role_name() == rolle and (o.get_name() or '').split(', ')[0].endswith(name) and \
            o.get_state_set().contains(Atspi.StateType.SHOWING):
        return o
    for i in range(o.get_child_count()):
        treffer = a11y_finden(rolle, name, o.get_child_at_index(i))
        if treffer:
            return treffer
    return None


def mitte(o):
    """Bildschirmpunkt: GTK 4.22 unter X11 meldet SCREEN-Koordinaten als 0,0, nur WINDOW stimmt (gemessen);
    dazu der Ursprung des X-Fensters der App."""
    e = o.get_extents(Atspi.CoordType.WINDOW)
    g = FENSTER.translate_coords(d.screen().root, 0, 0)
    return -g.x + e.x + e.width // 2, -g.y + e.y + e.height // 2


# ------------------------------------------------------------------ Schritte

def alle(rolle, o=None):
    """Sichtbare Knoten einer Rolle in Baumreihenfolge."""
    if o is None:
        desktop = Atspi.get_desktop(0)
        o = next(a for a in (desktop.get_child_at_index(i) for i in range(desktop.get_child_count()))
                 if a is not None and a.get_process_id() == app_pid)
    out = [o] if o.get_role_name() == rolle and o.get_state_set().contains(Atspi.StateType.SHOWING) else []
    for i in range(o.get_child_count()):
        out += alle(rolle, o.get_child_at_index(i))
    return out


def menue():
    """Einträge des Menüs „Verschieben“ haben bei GTK 4.22 keinen Namen am Barrierefreiheitsbus und liegen in
    einem eigenen Fenster (gemessen); ihre Reihenfolge steht im Log (mail-view.c, append_move_items: „Aktion:
    Ziel <Ordner>“). Gewählt per Pfeiltaste, bis der Eintrag den Fokus hat, dann Eingabe."""
    ab = len(log_zeilen())
    taste('Control_L+Shift_L+m', 1.0)
    ziele = [t.split('Aktion: Ziel ', 1)[1] for t in log_zeilen()[ab:] if t.startswith('Aktion: Ziel ')]
    treffer = [i for i, z in enumerate(ziele) if z.endswith(HEIM.rsplit('/', 1)[-1] + '-Ziel')]
    if len(treffer) != 1:
        raise RuntimeError('Menü: Ziele im Log %s' % ziele)
    for _ in range(len(ziele) + 1):
        eintraege = sorted(alle('menu item'), key=lambda o: o.get_extents(Atspi.CoordType.SCREEN).y)
        fokus = [i for i, e in enumerate(eintraege) if e.get_state_set().contains(Atspi.StateType.FOCUSED)]
        if fokus == treffer:
            break
        taste('Down', 0.3)
    else:
        raise RuntimeError('Menü: Eintrag %d nicht erreicht (%d Einträge)' % (treffer[0], len(eintraege)))
    bild('menue')
    taste('Return')


def ziehen_auf_ordner():
    zeile = a11y_finden('list item', HEIM.rsplit('/', 1)[-1] + '-Ziel')
    erste = a11y_finden('list item', 'Alle Eingänge')
    if zeile is None or erste is None:
        raise RuntimeError('Seitenleiste: Zielordner nicht gefunden')
    # Die Zeilen der Seitenleiste meldet GTK 4.22 um einen festen Betrag zu hoch (gemessen: Zielordner eine Zeile
    # über der wirklichen); Abgleich an „Alle Eingänge“, das im Fenster 1200×900 bei y = 100 steht (Screenshots).
    x, y = mitte(zeile)
    ziehen(ZEILE, (x, y + 100 - mitte(erste)[1]))


def ziehen_auf_eigenen_ordner():
    """Ziel = Quelle: Hinweis statt stiller Wirkungslosigkeit (#363 R2), Server unverändert."""
    zeile = a11y_finden('list item', HEIM.rsplit('/', 1)[-1])
    erste = a11y_finden('list item', 'Alle Eingänge')
    if zeile is None or erste is None:
        raise RuntimeError('Seitenleiste: eigener Ordner nicht gefunden')
    x, y = mitte(zeile)
    ab = len(log_zeilen())
    vorher = ort()
    ziehen(ZEILE, (x, y + 100 - mitte(erste)[1]))
    t = warte(r'^Hinweis: Die Nachricht ist schon in diesem Ordner', ab, 10)
    (ok if t else falsch)('Ziehen auf den eigenen Ordner: %s' % (t or 'Hinweis fehlt'))
    bild('hinweis-ziel-quelle')
    (ok if ort() == vorher else falsch)('Ziehen auf den eigenen Ordner: Server unverändert')


def rueckgaengig_knopf():
    time.sleep(1)  # Hinweis fertig eingeblendet
    knopf = a11y_finden('button', 'Rückgängig')
    if knopf is None:
        raise RuntimeError('Knopf „Rückgängig“ nicht gefunden')
    bild('knopf')
    klick(mitte(knopf))


# (Name, Bedienung, Log-Wort, Rückgängig, Ziel am Server prüfen). Strg+Z nach der Prüfung am Server (der
# Hinweis ist dann weg, Strg+Z wirkt bis zur nächsten Aktion); der Knopf im Hinweis sofort (5 s sichtbar).
ZURUECK = lambda: taste('Control_L+z')  # noqa: E731
SCHRITTE = [
    ('Löschen (Entf)', lambda: taste('Delete'), 'gelöscht', ZURUECK, True),
    ('Spam (Umschalt+Strg+J)', lambda: taste('Control_L+Shift_L+j'), 'Spam', ZURUECK, True),
    ('Verschieben (Menü)', menue, 'verschoben', ZURUECK, True),
    ('Verschieben (Ziehen)', ziehen_auf_ordner, 'verschoben', ZURUECK, True),
    ('Löschen, Knopf „Rückgängig“', lambda: taste('Delete'), 'gelöscht', rueckgaengig_knopf, False),
    # Zuletzt: bei Gmail bleibt danach das Banner „Kein Zielordner“ über der Liste stehen.
    ('Archivieren (Strg+Alt+A)', lambda: taste('Control_L+Alt_L+a'), 'archiviert', ZURUECK, True),
]
KONVERSATION = [('Konversation löschen (⌫)', lambda: taste('BackSpace'), 'gelöscht', ZURUECK, True)]


def schritt(name, bedienung, was, zurueck, am_server, n, vorher, erstes):
    ab = len(log_zeilen())
    bedienung()
    if was == 'archiviert' and 'gmail' in KONTO:
        # Grenze: Gmail hat keinen Archivordner (ESource leer, kein \\Archive; imapx verwirft \\All), wie Evolution.
        t = warte(r'^Banner: Kein Zielordner', ab, 10)
        (ok if t and warte(r'archiviert nicht möglich, Ziel fehlt', ab, 1) else falsch)('%s: ohne Archivordner %s' % (name, t))
        (ok if ort() == vorher else falsch)('%s: Server unverändert' % name)
        return
    t = warte(r'^Aktion: %s %s \S.* → (.*): (\d+) von (\d+) verschoben' % (re.escape(LOGKEY), re.escape(was)), ab, 90)
    m = t and re.search(r' → (.*): (\d+) von (\d+) verschoben', t)
    if not m or int(m.group(2)) != n or int(m.group(3)) != n:
        falsch('%s: Log „Aktion: … %d von %d verschoben“ fehlt (%s)' % (name, n, n, t))
        return
    ziel = m.group(1)
    ok('%s: %s' % (name, t))
    auswahl = warte(r'^Aktion: Zeile \d+ weg, gewählt', ab, 5)
    (ok if auswahl and (' gewählt 4294967295 ' not in auswahl or auswahl.endswith(' von 0')) else falsch)('%s: nächste Nachricht gewählt (%s)' % (name, auswahl))
    if erstes:
        time.sleep(0.5)
        bild('hinweis')
    if am_server:
        nachher = ort()
        orte = {(nr, o) for nr, o, _ in nachher}
        passt = all((nr, kurz(HEIM)) not in orte and (nr, kurz(ziel)) in orte for nr in {v[0] for v in vorher})
        (ok if passt else falsch)('%s: Server %s (vorher %s)' % (name, nachher, vorher))
    ab = len(log_zeilen())
    zurueck()
    t = warte(r'^Aktion: %s rückgängig \S.* → %s: %d von %d verschoben' % (re.escape(LOGKEY), re.escape(HEIM), n, n), ab, 90)
    (ok if t else falsch)('%s rückgängig: %s' % (name, t or 'Log „rückgängig … verschoben“ fehlt'))
    nachher = ort()
    (ok if nachher == vorher else falsch)('%s rückgängig: Server %s (vorher %s)' % (name, nachher, vorher))
    warte(r'^Nachrichten: %s Änderung' % re.escape(LOGKEY), ab, 20)
    time.sleep(1)


def main():
    global app_pid
    log = open(LOG, 'w')
    app = subprocess.Popen([BIN], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
    app_pid = app.pid
    leser = threading.Thread(target=filtern, args=(app.stdout, log), daemon=True)
    leser.start()
    try:
        global FENSTER
        w = FENSTER = fenster()
        if w is None:
            falsch('kein Fenster')
            return 1
        w.configure(width=1200, height=900)
        d.sync()
        if warte(r'^Nachrichten: %s vollständig' % re.escape(LOGKEY), 0, 120) is None:
            falsch('Liste nicht geladen (%s)' % ORDNER)
            return 1
        # Neue Mail im eben angelegten Ordner: microsoft365 liefert sie mitunter erst beim nächsten Abgleich.
        if warte(r'^Nachrichten: %s Reihenfolge: .* gewählt [^-]' % re.escape(LOGKEY), 0, 30) is None:
            taste('F5')
            if warte(r'^Nachrichten: %s Reihenfolge: .* gewählt [^-]' % re.escape(LOGKEY), 0, 90) is None:
                falsch('Testmail nicht gewählt (%s)' % ORDNER)
                return 1
        ok('Liste geladen: %s' % ORDNER)
        time.sleep(2)
        vorher = ort()
        n = len({v[0] for v in vorher})
        if WAS == 'aktion' and re.search(os.environ.get('NUR', ''), 'Ziel = Quelle'):
            ziehen_auf_eigenen_ordner()
            klick(ZEILE)
        schritte = KONVERSATION if WAS == 'konversation' else SCHRITTE
        schritte = [x for x in schritte if re.search(os.environ.get('NUR', ''), x[0])]
        for i, (name, bedienung, was, zurueck, am_server) in enumerate(schritte):
            if i > 0:
                klick(ZEILE)
            schritt(name, bedienung, was, zurueck, am_server, n, vorher, i == 0)
        w.send_event(protocol.event.ClientMessage(
            window=w, client_type=d.intern_atom('WM_PROTOCOLS'),
            data=(32, [d.intern_atom('WM_DELETE_WINDOW'), X.CurrentTime, 0, 0, 0])), event_mask=X.NoEventMask)
        d.sync()
        try:
            rc = app.wait(30)
            (ok if rc == 0 else falsch)('Beenden: Exit %d' % rc)
        except subprocess.TimeoutExpired:
            falsch('Beenden: nach 30 s nicht beendet')
    finally:
        if app.poll() is None:
            app.kill()
            app.wait()
        leser.join(5)
        log.close()
    zeilen = open(LOG, encoding='utf-8', errors='replace').read().splitlines()
    rest = [z for z in zeilen if not z.startswith(('(revolution:', 'libEGL warning:', '[graph] '))
            or (z.startswith('(revolution:') and 'revolution-DEBUG' not in z)]
    rest = [z for z in rest if not re.match(r'^\(process:\d+\): revolution-DEBUG: ', z)]
    # Bekannte Grenze EDS imapx (Prüf-Lauf #363 R1, docs/ENTWICKLUNG.md „#363“): Untagged EXPUNGE außerhalb von COPY/MOVE/IDLE
    # verwirft imapx (camel-imapx-server.c:789-829), neue Nachrichten aus dem NOTIFY-Rückstand einer zweiten
    # Verbindung trägt es ein (1336-1481); schon wegverschobene UIDs erscheinen dann als Geisterzeilen, eine
    # gewählte meldet „Empty cache file“. Ausdrücklich als BEKANNT ausgewiesen, nur bei imapx, nur diese Meldung.
    bekannt = [z for z in rest if not M365 and 'Nachricht nicht geladen' in z and 'Empty cache file' in z]
    rest = [z for z in rest if z not in bekannt]
    # Geisterzeilen nur im eigenen Ordner (genau eine Mail) und nur bei imapx.
    geister = [t for t in log_zeilen() if WAS == 'aktion' and not M365 and
               re.search(r'^Nachrichten: %s Änderung .*Liste [2-9]' % re.escape(LOGKEY), t)]
    for z in bekannt + geister:
        wert('BEKANNT Grenze EDS imapx, Geisterzeile (camel-imapx-server.c:789-829, 1336-1481): %s' % z.strip()[:200])
    (falsch if rest else ok)('Log: %s' % ('%d unerwartete Zeilen: %s' % (len(rest), rest[:3]) if rest else 'nur DEBUG-Zeilen'))
    if M365:
        graph = [z for z in zeilen if z.startswith('[graph] ')]
        schreibend = [z for z in graph if not z.startswith('[graph] GET') and not z.endswith('/$batch')
                      and not re.search(r' POST \S*/messages/\S+/move( \(batch\))?$', z)]
        moves = sum(1 for z in graph if re.search(r' POST \S*/messages/\S+/move( \(batch\))?$', z))
        (falsch if schreibend else ok)('Graph: %d× POST …/move, %s' % (
            moves, 'schreibend sonst: %s' % schreibend[:5] if schreibend else 'kein PATCH (kein Echo)'))
    return 1 if fehler else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as e:  # #411: Abbruch zählt als FEHLER
        traceback.print_exc()
        falsch('Abbruch: %s: %s' % (type(e).__name__, str(e).replace('\n', ' ')[:300]))
        sys.exit(1)
