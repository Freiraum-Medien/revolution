#!/usr/bin/env python3
# suche-treiber.py – Treiber für tests/suche.sh (#365), läuft im Xvfb aus xvfb-run.
#
# Startet die App (BIN) im Ordner ORDNER (REVOLUTION_FOLDER, flach, mark-seen=false), wählt eine Nachricht
# (AT-SPI, Liste „Nachrichten“), sucht SUCHWORT (Strg+F, Tippen per XTest) im Ordner und mit ALLE=1 in allen
# Ordnern (Alt+A), öffnet den ersten Treffer, beendet die Suche (Escape) und prüft Liste und Auswahl wie
# vorher; danach die Filter aus FILTER über das Filter-Menü (AT-SPI). Treffer = Zeilenzahl der Liste (Log
# „Nachrichten: … vollständig N“), verglichen mit tests/testdaten.py --suche-zahlen (Datei ZAHLEN, mit ALLE
# zusätzlich ZAHLEN_ALLE: Summe „…/* text N“ der Testkonten). Bilder OUT/NAME-<Schritt>.png.
# Umgebung: BIN, OUT, NAME, UID, ORDNER (Camel-Name), ORDNER_REF (Name in ZAHLEN), INBOX (1: Posteingang),
# ZAHLEN, ZAHLEN_ALLE, ALLE, FILTER, TIMEOUT. Ausgabe: „OK …“, „FEHLER …“, „WERT …“.
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

import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi  # noqa: E402
from PIL import Image  # noqa: E402
from Xlib import X, XK, display, protocol  # noqa: E402
from Xlib.ext import xtest  # noqa: E402

BIN, OUT, NAME = os.environ['BIN'], os.environ['OUT'], os.environ['NAME']
UID, ORDNER = os.environ['UID'], os.environ['ORDNER']
ZAHLEN, ZAHLEN_ALLE = os.environ['ZAHLEN'], os.environ.get('ZAHLEN_ALLE', '')
ALLE = os.environ.get('ALLE') == '1'
FILTER = os.environ.get('FILTER', 'unread starred attachment').split()
TIMEOUT = int(os.environ.get('TIMEOUT', '300'))
SUCHWORT = 'Quittenbrotkiste'
LOG = os.path.join(OUT, NAME + '.log')
LOGKEY = re.escape('%s %s' % (UID, ORDNER))
# Reihenfolge im Filter-Menü (mail-view.blp filter_menu) und Teil des Ausdrucks (mail-search.c).
MENUE = {'all': 0, 'unread': 1, 'starred': 2, 'attachment': 3}
AUSDRUCK = {'unread': 'not (system-flag "Seen")', 'starred': 'system-flag "Flagged"',
            'attachment': 'system-flag "Attachments"'}
SPALTE = {'unread': 'ungelesen', 'starred': 'markiert', 'attachment': 'anhang'}

fehler = 0
d = display.Display()
app = None


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


def marke():
    return len(log_text())


def warte(muster, ab, zeit=TIMEOUT):
    """Erster Treffer von muster im Log ab Position ab (Match oder None)."""
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        m = re.search(muster, log_text()[ab:], re.M)
        if m:
            return m
        time.sleep(0.2)
    return None


def zahlen(datei):
    """{(Ordner, Spalte): N} aus „SUCHE <UID>/<Ordner> text N …“."""
    out = {}
    for z in open(datei):
        m = re.match(r'^SUCHE \S+?/(\S+) (.*)$', z.strip())
        if m:
            teile = m.group(2).split()
            for i in range(0, len(teile) - 1, 2):
                out[(m.group(1), teile[i])] = out.get((m.group(1), teile[i]), 0) + int(teile[i + 1])
    return out


def taste(*namen):
    codes = [d.keysym_to_keycode(XK.string_to_keysym(n)) for n in namen]
    for c in codes:
        xtest.fake_input(d, X.KeyPress, c)
    for c in reversed(codes):
        xtest.fake_input(d, X.KeyRelease, c)
    d.sync()
    time.sleep(0.3)


def tippe(text):
    for ch in text:
        if ch.isupper():
            taste('Shift_L', ch)
        else:
            taste(ch)


def bild(schritt):
    root = d.screen().root
    g = root.get_geometry()
    pfad = os.path.join(OUT, '%s-%s.png' % (NAME, schritt))
    Image.frombytes('RGB', (g.width, g.height), root.get_image(0, 0, g.width, g.height, X.ZPixmap,
                                                               0xffffffff).data, 'raw', 'BGRX').save(pfad)
    wert('Bild %s' % pfad)


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


def schliessen(w):
    w.send_event(protocol.event.ClientMessage(
        window=w, client_type=d.intern_atom('WM_PROTOCOLS'),
        data=(32, [d.intern_atom('WM_DELETE_WINDOW'), X.CurrentTime, 0, 0, 0])), event_mask=X.NoEventMask)
    d.sync()
    try:
        return app.wait(30)
    except subprocess.TimeoutExpired:
        return None


# ------------------------------------------------------------------ Barrierefreiheitsbaum (AT-SPI)

def wurzel():
    desktop = Atspi.get_desktop(0)
    for i in range(desktop.get_child_count()):
        a = desktop.get_child_at_index(i)
        if a is not None and a.get_process_id() == app.pid:
            return a
    return None


def finde(o, rolle, name):
    if o.get_role_name() == rolle and o.get_name() == name:
        return o
    for i in range(o.get_child_count()):
        k = o.get_child_at_index(i)
        t = k and finde(k, rolle, name)
        if t:
            return t
    return None


def alle(o, rolle, out):
    if o.get_role_name() == rolle:
        out.append(o)
    for i in range(o.get_child_count()):
        k = o.get_child_at_index(i)
        if k:
            alle(k, rolle, out)
    return out


def zeile_waehlen(i):
    liste = finde(wurzel(), 'list', 'Nachrichten')
    if liste is None or not liste.select_child(i):
        falsch('Liste „Nachrichten“: Zeile %d nicht gewählt' % i)


def filter_waehlen(was):
    knopf = finde(wurzel(), 'toggle button', 'Filter')
    knopf.do_action(0)
    time.sleep(1)
    menue = finde(wurzel(), 'menu', 'Filter')
    eintraege = alle(menue, 'menu item', []) if menue else []
    if len(eintraege) < 4:
        falsch('Filter-Menü: %d Einträge statt 5' % len(eintraege))
        return
    if was == FILTER[0]:
        bild('filter')
    eintraege[MENUE[was]].do_action(0)


# ------------------------------------------------------------------ Schritte

def liste_nach(ab, was):
    m = warte(r'Nachrichten: %s vollständig (\d+) ' % LOGKEY, ab)
    if m is None:
        falsch('%s: Liste nicht geladen in %d s' % (was, TIMEOUT))
        return None
    return int(m.group(1))


def vergleiche(was, ist, soll):
    (ok if ist == soll else falsch)('%s: %s Treffer, Server %s' % (was, ist, soll))


def suchen(ref, ref_alle):
    ab = marke()
    taste('Control_L', 'f')
    tippe(SUCHWORT)
    if not warte(r'Suche: \(and .*body-contains "%s".*\) in Ordner$' % SUCHWORT, ab):
        falsch('Suche im Ordner nicht gestartet')
        return
    vergleiche('Suche „%s“ im Ordner' % SUCHWORT, liste_nach(ab, 'Suche'), ref.get((ORDNER_REF, 'text')))
    zeile_waehlen(0)
    m = warte(r'Nachricht: %s (\S+) geladen' % LOGKEY, ab, 60)
    (ok if m else falsch)('Treffer im Lesebereich geöffnet%s' % (' (UID %s)' % m.group(1) if m else ''))
    if not ALLE:
        time.sleep(2)
        bild('suche')
        return
    ab = marke()
    taste('Alt_L', 'a')
    if not warte(r'Suche: \(and .*\) in allen Ordnern$', ab, 30):
        falsch('Alt+A: Suchbereich „Alle Ordner“ nicht gewählt')
        return
    ok('Alt+A wählt „Alle Ordner“')
    m = warte(r'Suche: alle Ordner, (\d+) Ordner, (\d+) Treffer nach (\d+) ms', ab)
    if m:
        wert('alle Ordner: %s Ordner durchsucht, %s ms' % (m.group(1), m.group(3)))
    soll = sum(n for (o, s), n in ref_alle.items() if o == '*' and s == 'text')
    vergleiche('Suche „%s“ in allen Ordnern' % SUCHWORT, liste_nach(ab, 'Alle Ordner'), soll)
    zeile_waehlen(0)
    m = warte(r'Nachricht: revolution-favoriten suche (\S+) geladen', ab, 60)
    (ok if m else falsch)('Treffer aus „Alle Ordner“ im Lesebereich (Suchordner, UID %s)' % (m and m.group(1)))
    time.sleep(2)
    bild('alle')
    ab = marke()
    taste('Alt_L', 'o')
    (ok if warte(r'Suche: \(and .*\) in Ordner$', ab, 30) else falsch)('Alt+O wählt „Dieser Ordner“')
    liste_nach(ab, 'Dieser Ordner')


def beenden(n0, gewaehlt, wie):
    ab = marke()
    if wie == 'escape':
        taste('Control_L', 'f')
        taste('Escape')
    else:
        filter_waehlen('all')
    if not warte(r'Suche: aus in Ordner$', ab, 30):
        falsch('%s: Suche nicht beendet' % wie)
        return
    vergleiche('%s: Liste wie vorher' % wie, liste_nach(ab, 'Ende der Suche'), n0)
    m = warte(r'Suche: beendet, Auswahl (\S+) wieder gewählt', ab, 60)
    (ok if m and m.group(1) == gewaehlt else falsch)(
        '%s: Auswahl wie vorher (%s, vorher %s)' % (wie, m and m.group(1), gewaehlt))
    m = warte(r'Nachricht: %s %s geladen' % (LOGKEY, re.escape(gewaehlt)), ab, 60)
    (ok if m else falsch)('%s: Lesebereich zeigt wieder die vorher gewählte Nachricht' % wie)


def filtern(ref):
    for was in FILTER:
        ab = marke()
        filter_waehlen(was)
        if not warte(r'Suche: \(and \(match-all \(%s\)\)\) in Ordner$' % re.escape(AUSDRUCK[was]), ab, 30):
            falsch('Filter %s nicht gewählt' % was)
            continue
        vergleiche('Filter %s' % was, liste_nach(ab, 'Filter %s' % was), ref.get((ORDNER_REF, SPALTE[was])))


def pruefen(w):
    ab = 0
    # Posteingang: den Abgleich beim Start abwarten (main.c), sonst zeigt die Liste den Cache (Abgleich läuft schon).
    if os.environ.get('INBOX') == '1':
        if not warte(r'Posteingang: %s \S+ aktualisiert' % re.escape(UID), ab):
            falsch('Posteingang nicht abgeglichen in %d s' % TIMEOUT)
    n0 = liste_nach(ab, 'Start')
    if n0 is None:
        return
    time.sleep(3)
    n0 = int(re.findall(r'Nachrichten: %s .*?(?:vollständig|Liste) (\d+)' % LOGKEY, log_text())[-1])
    wert('Liste vor der Suche: %d Zeilen' % n0)
    # Zeiger ins Fenster: ohne Fenstermanager hat das Fenster unter dem Zeiger den Tastaturfokus.
    xtest.fake_input(d, X.MotionNotify, x=1000, y=600)
    d.sync()
    ab = marke()
    zeile_waehlen(1)
    m = warte(r'Nachricht: %s (\S+) geladen' % LOGKEY, ab, 60)
    if m is None:
        falsch('Auswahl vor der Suche: Nachricht nicht geladen')
        return
    gewaehlt = m.group(1)
    wert('Auswahl vor der Suche: UID %s' % gewaehlt)
    ref = zahlen(ZAHLEN)
    suchen(ref, zahlen(ZAHLEN_ALLE) if ALLE else {})
    beenden(n0, gewaehlt, 'escape')
    filtern(ref)
    beenden(n0, gewaehlt, 'Filter Alle')
    andere = [z for z in log_text().splitlines() if z.startswith('(revolution:') and 'revolution-DEBUG' not in z]
    (ok if not andere else falsch)('keine Warnungen/Criticals im Log%s' % ('' if not andere else ': %s' % andere[:3]))


# Ordnername in der Ausgabe von --suche-zahlen (Kurzname wie dort übergeben).
ORDNER_REF = os.environ.get('ORDNER_REF', ORDNER)


def main():
    global app
    log = open(LOG, 'w')
    app = subprocess.Popen([BIN], stdout=log, stderr=subprocess.STDOUT)
    try:
        w = fenster()
        if w is None:
            falsch('kein Fenster')
            return 1
        pruefen(w)
        rc = schliessen(w)
        (ok if rc == 0 else falsch)('Beenden: Exit %s' % rc)
    finally:
        if app.poll() is None:
            app.kill()
            app.wait()
        log.close()
    return 1 if fehler else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception:
        traceback.print_exc()
        falsch('Abbruch: %s' % traceback.format_exc().strip().splitlines()[-1])
        sys.exit(1)
