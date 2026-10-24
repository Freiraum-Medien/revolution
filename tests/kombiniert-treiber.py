#!/usr/bin/env python3
# kombiniert-treiber.py – Treiber für tests/kombiniert.sh (#351), läuft im Xvfb aus xvfb-run.
#
# Startet die App (BIN) mit REVOLUTION_FOLDER=revolution-favoriten/inbox („Alle Eingänge“) und
# schreibt ihr Log gefiltert nach LOG: Debug-Zeilen der App, IMAP-Befehle (CAMEL_DEBUG=imapx:command)
# ohne Anmeldung, Graph-Anfragen (M365_DEBUG=1) nur als „> METHODE Pfad“ ohne Kopfzeilen (Token).
# NAME-Endung:
#   -dunkel  nur laden, Screenshot; kein Schreibzugriff (Beleg nur lesend, tests/kombiniert.sh)
#   -zyklus  laden, dann den Store des Kontos ZYKLUS_UID aus der Session nehmen und wieder anlegen
#            (Test-Aktion app.test-store, REVOLUTION_TEST_HOOKS=1, main.c): seine Mails verschwinden aus
#            „Alle Eingänge“ und kommen genau einmal wieder (Zählung, keine doppelten UIDs); kein
#            Schreibzugriff
#   -hell    laden, Screenshot, dann im Testkonto KONTO an der Live-Mail (tests/testdaten.py --server
#            KONTO neu INBOX, vor dem Start) Gelesen nach Verzögerung, Markieren, Markierung entfernen,
#            Ungelesen; jede Änderung direkt am Server geprüft (tests/testdaten.py --live-flags, nur
#            lesend); neue Mail und Löschen am Server live (IMAP IDLE); Zähler „Alle Eingänge“.
# Ausgabe: „OK …“, „FEHLER …“, „WERT …“; Screenshots OUT/NAME-<Schritt>.png.
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

from PIL import Image
from Xlib import X, XK, display, protocol
from Xlib.ext import xtest

BIN, OUT, NAME, APP_DIR = os.environ['BIN'], os.environ['OUT'], os.environ['NAME'], os.environ['APP_DIR']
KONTO = os.environ.get('KONTO', '')
FAV = 'revolution-favoriten inbox'
# Fenster 1200×760: erste Zeile der Liste (wie tests/flags-treiber.py).
ZEILE = (420, 108)

fehler = 0
d = display.Display()
LOG = os.path.join(OUT, NAME + '.log')
ZEILE_RE = re.compile(r'^\(revolution:\d+\): revolution-DEBUG: (\d\d):(\d\d):(\d\d\.\d+): (.*)$')
# Gesendete IMAP-Befehle: je Befehl eine Zeile „completing command buffer is [N] '<Befehl>'“
# (EDS camel-imapx-command.c:528-532, CAMEL_DEBUG=imapx:command); „UID STORE“ usw. bleiben vollständig.
IMAP_RE = re.compile(r"(\[imapx:[A-Za-z]\]) completing command buffer is \[\d+\] '(.{0,160})")
GRAPH_RE = re.compile(r'^> (GET|POST|PATCH|PUT|DELETE|HEAD) (\S+)')


def ok(text):
    print('OK      %s: %s' % (NAME, text), flush=True)


def falsch(text):
    global fehler
    fehler += 1
    print('FEHLER  %s: %s' % (NAME, text), flush=True)


def wert(text):
    print('WERT    %s: %s' % (NAME, text), flush=True)


def filtern(quelle, ziel):
    """Ausgabe der App zeilenweise nach LOG, ohne Anmeldedaten und ohne HTTP-Kopf/Rumpf."""
    for roh in quelle:
        zeile = roh.decode('utf-8', 'replace')
        if 'revolution-DEBUG' in zeile or 'WARNING' in zeile or 'CRITICAL' in zeile:
            ziel.write(zeile)
        elif re.search(r'Authorization:', zeile, re.I) or \
                ('[imapx' in zeile and re.search(r'AUTHENTICATE|LOGIN|XOAUTH|PLAIN', zeile, re.I)):
            continue
        else:
            m = IMAP_RE.search(zeile)
            g = GRAPH_RE.match(zeile)
            if m:
                ziel.write('%s %s\n' % (m.group(1), m.group(2).rstrip().rstrip("'")))
            elif g:
                ziel.write('[graph] %s %s\n' % (g.group(1), g.group(2).split('?')[0]))
            elif '"requests"' in zeile and '"method"' in zeile:
                # Rumpf eines Graph-$batch: nur die Methoden und Pfade der Teilanfragen.
                for methode, url in re.findall(r'"method"\s*:\s*"([A-Z]+)"\s*,\s*"url"\s*:\s*"([^"?]*)', zeile):
                    ziel.write('[graph-batch] %s %s\n' % (methode, url))
        ziel.flush()


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
    rx = re.compile(muster)
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        for t, text in log_zeilen()[ab:]:
            if rx.search(text):
                return t, text
        time.sleep(0.2)
    return None


def letzte(muster):
    rx = re.compile(muster)
    treffer = [rx.search(t) for _, t in log_zeilen()]
    treffer = [m for m in treffer if m]
    return treffer[-1] if treffer else None


def ungelesen_fav():
    m = letzte(r'^Ungelesen: %s (\d+)$' % FAV)
    return int(m.group(1)) if m else None


def ruhig(sekunden=5, zeit=180):
    """Warten, bis sekunden lang keine Zeile zu „Favoriten“ oder „Nachrichten: … inbox“ dazukam."""
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        n = marke()
        time.sleep(sekunden)
        neu = [t for _, t in log_zeilen()[n:] if t.startswith(('Favoriten', 'Nachrichten: ' + FAV, 'Ungelesen: '))]
        if not neu:
            return True
    return False


# ------------------------------------------------------------------ Server (unabhängig)

def server_flags():
    r = subprocess.run([sys.executable, os.path.join(APP_DIR, 'tests', 'testdaten.py'), '--live-flags', KONTO],
                       capture_output=True, text=True, timeout=180)
    zeilen = [z.split() for z in r.stdout.splitlines() if z.startswith('FLAGS ')]
    if len(zeilen) != 1:
        raise RuntimeError('Live-Mail nicht eindeutig: %s %s' % (r.stdout, r.stderr))
    return int(zeilen[0][1]), int(zeilen[0][2])


def server_aendern(aktion):
    r = subprocess.run([sys.executable, os.path.join(APP_DIR, 'tests', 'testdaten.py'), '--server', KONTO, aktion, 'INBOX'],
                       capture_output=True, text=True, timeout=180)
    # #411: Exit ≠ 0 von testdaten.py (Traceback, ABBRUCH) ist ein Abbruch des Schritts.
    (wert if r.returncode == 0 else falsch)('Server %s: %s%s' % (
        aktion, (r.stdout + r.stderr).strip().replace('\n', ' | '), '' if r.returncode == 0 else ' (Exit %d)' % r.returncode))


def pruefe_server(schritt, gelesen=None, markiert=None):
    ist = server_flags()
    soll = (ist[0] if gelesen is None else gelesen, ist[1] if markiert is None else markiert)
    (ok if ist == soll else falsch)('%s → Server %s gelesen=%d markiert=%d%s' % (
        schritt, KONTO, ist[0], ist[1], '' if ist == soll else ' (erwartet %d/%d)' % soll))


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
    g = root.get_geometry()
    raw = root.get_image(0, 0, g.width, g.height, X.ZPixmap, 0xffffffff).data
    pfad = os.path.join(OUT, '%s-%s.png' % (NAME, schritt))
    Image.frombytes('RGB', (g.width, g.height), raw, 'raw', 'BGRX').save(pfad)
    wert('Screenshot %s' % pfad)


def fenster():
    for _ in range(150):
        for w in d.screen().root.query_tree().children:
            try:
                if w.get_wm_name() == 'Revolution' and w.get_attributes().map_state == X.IsViewable:
                    return w
            except Exception:
                pass
        time.sleep(0.2)
    return None


# ------------------------------------------------------------------ Schritte

def geladen():
    """Alle angezeigten Konten: Posteingang abgeglichen und Quellen in den Favoriten."""
    ende = time.monotonic() + 300
    while time.monotonic() < ende:
        zeilen = [t for _, t in log_zeilen()]
        konten = {}
        for t in zeilen:
            m = re.match(r'^Mail-Konto (\S+) „.*“: (.*)$', t)
            if m:
                konten[m.group(1)] = m.group(2) == 'angezeigt'
        gezeigt = {u for u, a in konten.items() if a}
        posteingang = {m.group(1) for m in (re.match(r'^Posteingang: (\S+) .* (nicht )?aktualisiert', t) for t in zeilen) if m}
        fertig = {m.group(1) for m in (re.match(r'^Favoriten: (\S+) fertig$', t) for t in zeilen) if m}
        if gezeigt and gezeigt <= posteingang and gezeigt <= fertig:
            ok('%d Konten: Posteingang abgeglichen, Quellen in den Favoriten' % len(gezeigt))
            return gezeigt
        time.sleep(1)
    falsch('nicht alle Konten geladen (Posteingang/Favoriten)')
    return None


def flag_schritt(schritt, aktion, was, gelesen=None, markiert=None):
    ab = marke()
    aktion()
    t = warte(r'^Flags: %s \S+ %s gespeichert nach' % (re.escape(FAV), re.escape(was)), ab)
    if t is None:
        falsch('%s: Log „Flags: %s … %s gespeichert“ fehlt' % (schritt, FAV, was))
        return
    ok('%s: %s' % (schritt, t[1]))
    pruefe_server(schritt, gelesen, markiert)


def testkonto():
    u0 = ungelesen_fav()
    wert('Ungelesen „Alle Eingänge“ vor der Auswahl: %s' % u0)
    ab = marke()
    klick(ZEILE)
    t = warte(r'^Flags: %s \S+ gelesen \(automatisch\) gespeichert' % re.escape(FAV), ab, 30)
    (ok if t else falsch)('Auswahl der Live-Mail → gelesen nach Verzögerung: %s' % (t[1] if t else 'fehlt'))
    pruefe_server('Auswahl', gelesen=1)
    if warte(r'^Ungelesen: %s %d$' % (re.escape(FAV), (u0 or 0) - 1), ab, 15):
        ok('Zähler „Alle Eingänge“ %d → %d' % (u0, u0 - 1))
    else:
        falsch('Zähler „Alle Eingänge“ nach Gelesen nicht %s - 1 (Log: %s)' % (u0, ungelesen_fav()))
    bild('2-gelesen')

    ab = marke()
    flag_schritt('Umschalt+Strg+L', lambda: taste('Control_L+Shift_L+l'), 'markiert', markiert=1)
    m = warte(r'^Favoriten: Anzahl flagged \S+ \S+ \d+$', ab, 15)
    wert('Markiert nach dem Markieren: %s' % ' | '.join(
        t for _, t in log_zeilen()[ab:] if t.startswith('Favoriten: Anzahl flagged')))
    (ok if m else falsch)('„Markiert“ folgt der Markierung (Log „Favoriten: Anzahl flagged …“)')
    bild('3-markiert')
    flag_schritt('Umschalt+Strg+L', lambda: taste('Control_L+Shift_L+l'), 'nicht markiert', markiert=0)
    flag_schritt('Strg+Umschalt+K', lambda: taste('Control_L+Shift_L+k'), 'ungelesen', gelesen=0)

    # Neue Mail am Server (IMAP IDLE am Posteingang des Testkontos) und Löschen.
    u1 = ungelesen_fav()
    ab = marke()
    server_aendern('neu')
    t = warte(r'^Nachrichten: %s Änderung 1 neu' % re.escape(FAV), ab, 120)
    (ok if t else falsch)('neue Mail am Server → „Alle Eingänge“ ohne F5: %s' % (t[1] if t else 'fehlt'))
    if warte(r'^Ungelesen: %s %d$' % (re.escape(FAV), (u1 or 0) + 1), ab, 30):
        ok('Zähler „Alle Eingänge“ live %d → %d' % (u1, u1 + 1))
    else:
        falsch('Zähler „Alle Eingänge“ nach neuer Mail nicht %s + 1 (Log: %s)' % (u1, ungelesen_fav()))
    bild('4-neu')
    ab = marke()
    server_aendern('weg')
    ende = time.monotonic() + 120
    entfernt = 0
    while time.monotonic() < ende and entfernt < 2:
        entfernt = sum(int(m.group(1)) for m in (re.match(r'^Nachrichten: %s Änderung \d+ neu, (\d+) entfernt' % re.escape(FAV), t)
                                                for _, t in log_zeilen()[ab:]) if m)
        time.sleep(0.5)
    (ok if entfernt == 2 else falsch)('Live-Mails am Server gelöscht → %d entfernt (erwartet 2)' % entfernt)


def liste():
    """(Zeilen, verschiedene UIDs) des letzten Stands der Liste „Alle Eingänge“."""
    m = letzte(r'^Nachrichten: %s (?:vollständig |.*Liste )(\d+)(?: nach [^,]*)?, (\d+) verschiedene UIDs' % re.escape(FAV))
    return (int(m.group(1)), int(m.group(2))) if m else (None, None)


def anzahl_konto(uid, ab=0):
    """Letzte Zahl „Favoriten: Anzahl inbox <uid> …“ ab Marke, None ohne Zeile."""
    werte = [int(m.group(1)) for m in (re.match(r'^Favoriten: Anzahl inbox %s \S+ (\d+)$' % uid, t)
                                       for _, t in log_zeilen()[ab:]) if m]
    return werte[-1] if werte else None


def test_store(uid, vorhanden):
    pfad = '/' + os.environ['APP_ID'].replace('.', '/').replace('-', '_')
    r = subprocess.run(['gdbus', 'call', '--session', '-d', os.environ['APP_ID'], '-o', pfad, '-m',
                        'org.gtk.Actions.Activate', 'test-store',
                        "[<('%s', %s)>]" % (uid, 'true' if vorhanden else 'false'), '{}'],
                       capture_output=True, text=True, timeout=30)
    (wert if r.returncode == 0 else falsch)('app.test-store %s %s: %s' % (uid, vorhanden, (r.stdout + r.stderr).strip()))


def zyklus():
    uid = os.environ['ZYKLUS_UID']
    gesamt, verschieden = liste()
    n = anzahl_konto(uid)
    wert('vorher: Liste %s, %s verschiedene UIDs, Konto %s: %s' % (gesamt, verschieden, uid, n))
    if gesamt is None or n is None or gesamt != verschieden:
        falsch('Ausgangsstand unklar')
        return
    ab = marke()
    test_store(uid, False)
    t = warte(r'^Favoriten: inbox ohne %s .*\(Store entfernt\)$' % uid, ab, 60)
    (ok if t else falsch)('Store entfernt → Quelle aus „Alle Eingänge“: %s' % (t[1] if t else 'fehlt'))
    ruhig(3, 60)
    ohne, ohne_v = liste()
    (ok if ohne == gesamt - n and ohne_v == ohne else falsch)(
        'ohne Konto: Liste %s, %s verschiedene UIDs (erwartet %d)' % (ohne, ohne_v, gesamt - n))
    rest = anzahl_konto(uid, ab)
    (ok if rest is None else falsch)('ohne Konto: keine Zählung mehr für %s (%s)' % (uid, rest))
    bild('2-ohne-konto')
    ab = marke()
    test_store(uid, True)
    t = warte(r'^Favoriten: %s fertig$' % uid, ab, 120)
    (ok if t else falsch)('Store neu angelegt → Quellen wieder hinzugefügt: %s' % (t[1] if t else 'fehlt'))
    ruhig(3, 120)
    wieder, wieder_v = liste()
    n2 = anzahl_konto(uid, ab)
    (ok if wieder == gesamt and wieder_v == wieder and n2 == n else falsch)(
        'wieder da: Liste %s, %s verschiedene UIDs, Konto %s (vorher %s/%s/%s)' % (wieder, wieder_v, n2, gesamt, verschieden, n))
    quellen = [t for _, t in log_zeilen()[ab:] if re.match(r'^Favoriten: inbox aus %s ' % uid, t)]
    (ok if len(quellen) == 1 else falsch)('Posteingang genau einmal wieder hinzugefügt (%d×)' % len(quellen))
    bild('3-wieder')


def main():
    log = open(LOG, 'w')
    app = subprocess.Popen([BIN], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    leser = threading.Thread(target=filtern, args=(app.stdout, log), daemon=True)
    leser.start()
    try:
        w = fenster()
        if w is None:
            falsch('kein Fenster')
            return 1
        w.configure(width=1200, height=760)
        d.sync()
        if geladen() is None:
            return 1
        if not ruhig():
            falsch('Favoriten kommen nicht zur Ruhe')
        wert('Liste: %s' % (letzte(r'^Nachrichten: %s .*' % re.escape(FAV)).group(0)))
        wert('Ungelesen „Alle Eingänge“: %s' % ungelesen_fav())
        bild('1-geladen')
        if NAME.endswith('-zyklus'):
            zyklus()
        if NAME.endswith('-hell'):
            testkonto()
            ruhig(3, 60)
            bild('5-ende')
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
    return 1 if fehler else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as e:  # #411: Abbruch (Traceback, Zeitlimit eines Aufrufs) zählt als FEHLER
        traceback.print_exc()
        falsch('Abbruch: %s: %s' % (type(e).__name__, str(e).replace('\n', ' ')[:300]))
        sys.exit(1)
