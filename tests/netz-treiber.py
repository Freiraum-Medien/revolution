#!/usr/bin/env python3
# netz-treiber.py – Treiber für tests/netz.sh (#408), läuft im Xvfb aus xvfb-run im Netz-Namensraum.
#
# Startet die App (BIN) mit Log, schaltet das Netz über tests/netz-relay.py (CTRL: „up“/„down“) und
# prüft je Schritt Log, Verbindungen nach außen (Log des Relais, RELAIS_LOG) und Screenshot.
#   NAME …-vorbereiten  Start mit Netz: Ordner ORDNER und Nachricht REVOLUTION_MESSAGE laden (Cache)
#   NAME …              Start ohne Netz: Ordner aller Konten und Nachricht aus dem Cache, Pille
#                       „Offline“; Netz zurück: online nach 5 s, Konten verbunden, Liste vom Server,
#                       Pille weg; Netz weg zur Laufzeit: offline, F5 aus dem Cache; Netz zurück.
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

BIN, OUT, NAME, APP_DIR = os.environ['BIN'], os.environ['OUT'], os.environ['NAME'], os.environ['APP_DIR']
CTRL, RELAIS_LOG, REF = os.environ['CTRL'], os.environ['RELAIS_LOG'], os.environ['REF']
ORDNER = os.environ['REVOLUTION_FOLDER']
LOGKEY = ORDNER.replace('/', ' ', 1)

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


def warte(muster, ab, zeit=60):
    """Erste neue Debug-Zeile ab Marke, die muster (regex) enthält: (Zeit, Text) oder None."""
    rx = re.compile(muster)
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        for t, text in log_zeilen()[ab:]:
            if rx.search(text):
                return t, text
        time.sleep(0.2)
    return None


def warte_alle(muster, uids, ab, zeit=120):
    """Je UID eine Zeile muster % uid ab Marke; Rückgabe: fehlende UIDs."""
    ende = time.monotonic() + zeit
    fehlt = set(uids)
    while fehlt and time.monotonic() < ende:
        texte = [t for _, t in log_zeilen()[ab:]]
        fehlt = {u for u in fehlt if not any(re.search(muster % re.escape(u), t) for t in texte)}
        time.sleep(0.2)
    return fehlt


def pruefe(schritt, muster, ab, zeit=60):
    treffer = warte(muster, ab, zeit)
    (ok if treffer else falsch)('%s: %s' % (schritt, treffer[1] if treffer else 'Log „%s“ fehlt' % muster))
    return treffer


def warnungen(ab_zeile):
    """Zeilen der App außer DEBUG ab Zeilennummer der Datei (Warnungen, Criticals)."""
    with open(LOG, encoding='utf-8', errors='replace') as f:
        zeilen = f.readlines()[ab_zeile:]
    return [z.strip() for z in zeilen if z.startswith('(revolution:') and 'revolution-DEBUG' not in z]


def dateizeilen():
    with open(LOG, encoding='utf-8', errors='replace') as f:
        return len(f.readlines())


# ------------------------------------------------------------------ Netz (Relais)

def relais_zeilen():
    with open(RELAIS_LOG, encoding='utf-8') as f:
        return [z.strip() for z in f if ' TCP ' in z]


def netz(kommando):
    r = subprocess.run([sys.executable, os.path.join(APP_DIR, 'tests', 'netz-relay.py'), 'steuern', CTRL, kommando],
                       capture_output=True, text=True, timeout=30)
    (ok if r.stdout.strip() == 'ok ' + kommando else falsch)('Netz %s: %s' % (kommando, (r.stdout + r.stderr).strip()))


# ------------------------------------------------------------------ Vergleichsliste (testdaten.py --ordner)

def referenz():
    namen, anzahl = {}, {}
    with open(REF, encoding='utf-8') as f:
        for zeile in f:
            teile = zeile.rstrip('\n').split(' ', 2)
            if teile[0] == 'ORDNER':
                namen.setdefault(teile[1], set()).add(teile[2])
            elif teile[0] == 'ANZAHL':
                anzahl[teile[1]] = int(teile[2].split()[0])
    return namen, anzahl


# ------------------------------------------------------------------ XTest

def taste(kombi, pause=0.3):
    codes = [d.keysym_to_keycode(XK.string_to_keysym(k)) for k in kombi.split('+')]
    for c in codes:
        xtest.fake_input(d, X.KeyPress, c)
    for c in reversed(codes):
        xtest.fake_input(d, X.KeyRelease, c)
    d.sync()
    time.sleep(pause)


def bild(schritt):
    xtest.fake_input(d, X.MotionNotify, x=0, y=0)
    d.sync()
    time.sleep(1)
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

def ordner_wie_server(schritt, uids, ab):
    """Ordnerbaum je Konto (Log „Ordner: UID Name“ und „… N geladen“) = testdaten.py --ordner."""
    namen, anzahl = referenz()
    texte = [t for _, t in log_zeilen()[ab:]]
    for uid in uids:
        n = [int(m.group(1)) for t in texte for m in [re.match(r'^Ordner: %s (\d+) geladen$' % re.escape(uid), t)] if m]
        app = {t[len('Ordner: %s ' % uid):] for t in texte if t.startswith('Ordner: %s ' % uid)
               and not re.match(r'^Ordner: \S+ (\d+ geladen|nicht |vom Server)', t)}
        gleich = n and n[-1] == anzahl.get(uid) and app == namen.get(uid, set())
        (ok if gleich else falsch)('%s: Ordner %s: App %s, Server %s%s' % (
            schritt, uid[:8], n[-1] if n else 'keine', anzahl.get(uid),
            '' if gleich else ' (nur App: %s, nur Server: %s)' % (sorted(app - namen.get(uid, set()))[:5],
                                                                  sorted(namen.get(uid, set()) - app)[:5])))


def ohne_warnungen(schritt, ab_zeile, erlaubt=()):
    rest = [z for z in warnungen(ab_zeile) if not any(e in z for e in erlaubt)]
    (falsch if rest else ok)('%s: %s' % (schritt, '%d Warnungen: %s' % (len(rest), rest[:3]) if rest
                                          else 'keine Warnung/Fehlerzeile im Log (Sammel-Banner leer)'))


def online_schalten(schritt, uids):
    ab, ab_datei = marke(), dateizeilen()
    tcp_vorher = len(relais_zeilen())
    netz('up')
    start = time.monotonic()
    verfuegbar = pruefe('%s: Netzmonitor' % schritt, r'^Netz: verfügbar, online in 5 s$', ab, 20)
    online = pruefe('%s: Session' % schritt, r'^Netz: online, \d+ Stores verbinden$', ab, 30)
    if verfuegbar and online:
        abstand = online[0] - verfuegbar[0]
        (ok if 4.5 <= abstand <= 7 else falsch)('%s: online %.1f s nach „verfügbar“ (Evolution 5 s)' % (schritt, abstand))
    pruefe('%s: Pille' % schritt, r'^Hinweis Offline: weg$', ab, 30)
    fehlt = warte_alle(r'^verbunden: %s$', uids, ab, 120)
    (falsch if fehlt else ok)('%s: %s' % (schritt, 'nicht verbunden: %s' % sorted(fehlt) if fehlt
                                          else 'alle %d Konten verbunden' % len(uids)))
    fehlt = warte_alle(r'^Ordner: %s (vom Server, Ordnerbaum bleibt|\d+ geladen)$', uids, ab, 120)
    (falsch if fehlt else ok)('%s: %s' % (schritt, 'Ordnerliste nicht vom Server: %s' % sorted(fehlt) if fehlt
                                          else 'Ordnerliste aller Konten vom Server geholt'))
    pruefe('%s: Liste vom Server' % schritt, r'^Aktualisieren: %s fertig$' % re.escape(LOGKEY), ab, 120)
    tcp = relais_zeilen()[tcp_vorher:]
    (ok if tcp else falsch)('%s: %d Verbindungen nach außen über das Relais, z. B. %s' % (schritt, len(tcp), tcp[:3]))
    wert('%s: Dauer bis Liste vom Server %.1f s' % (schritt, time.monotonic() - start))
    ohne_warnungen(schritt, ab_datei)


def offline_laufzeit(schritt, uids):
    ab, ab_datei = marke(), dateizeilen()
    netz('down')
    tcp_vorher = len(relais_zeilen())
    pruefe('%s: Session' % schritt, r'^Netz: nicht verfügbar, offline$', ab, 20)
    pruefe('%s: Pille' % schritt, r'^Hinweis Offline: sichtbar$', ab, 10)
    fehlt = warte_alle(r'^offline: %s( mit Fehler: .*)?$', uids, ab, 60)
    (falsch if fehlt else ok)('%s: %s' % (schritt, 'nicht offline: %s' % sorted(fehlt) if fehlt
                                          else 'alle %d Stores offline' % len(uids)))
    ab2 = marke()
    taste('F5')
    pruefe('%s: F5 ohne Netz' % schritt, r'^Aktualisieren: %s fertig$' % re.escape(LOGKEY), ab2, 30)
    time.sleep(3)
    ohne_warnungen(schritt, ab_datei)
    tcp = relais_zeilen()[tcp_vorher:]
    (ok if not tcp else falsch)('%s: keine Verbindung nach außen ohne Netz (%d)' % (schritt, len(tcp)))


def vorbereiten():
    pruefe('Nachricht', r'^Nachricht: %s \S+ geladen nach' % re.escape(LOGKEY), 0, 120)


def wechsel(uids):
    # 1. Start ohne Netz: Ordner und Nachricht aus dem Cache, Pille sichtbar, kein Verbindungsfehler.
    pruefe('Start ohne Netz', r'^Netz: nicht verfügbar, Start offline$', 0, 30)
    pruefe('Start ohne Netz: Pille', r'^Hinweis Offline: sichtbar$', 0, 10)
    fehlt = warte_alle(r'^Ordner: %s \d+ geladen$', uids, 0, 60)
    (falsch if fehlt else ok)('Start ohne Netz: %s' % ('keine Ordner: %s' % sorted(fehlt) if fehlt
                                                        else 'Ordner aller %d Konten aus dem Cache' % len(uids)))
    fehlt = warte_alle(r'^offline: %s, Ordner aus dem Cache$', uids, 0, 10)
    (falsch if fehlt else ok)('Start ohne Netz: %s' % ('ohne Log „offline“: %s' % sorted(fehlt) if fehlt
                                                        else 'kein Verbindungsversuch (Log „offline: … Ordner aus dem Cache“)'))
    ordner_wie_server('Start ohne Netz', uids, 0)
    pruefe('Start ohne Netz: Liste aus dem Cache', r'^Nachrichten: %s vollständig' % re.escape(LOGKEY), 0, 60)
    pruefe('Start ohne Netz: Nachricht aus dem Cache', r'^Nachricht: %s \S+ geladen nach' % re.escape(LOGKEY), 0, 60)
    time.sleep(3)
    ohne_warnungen('Start ohne Netz', 0)
    (ok if not relais_zeilen() else falsch)('Start ohne Netz: keine Verbindung nach außen (%d)' % len(relais_zeilen()))
    bild('1-offline-start')

    # 2. Netz zurück.
    online_schalten('Netz zurück', uids)
    time.sleep(2)
    bild('2-online')

    # 3. Netz weg zur Laufzeit.
    offline_laufzeit('Netz weg', uids)
    bild('3-offline-laufzeit')

    # 4. Netz wieder zurück.
    online_schalten('Netz wieder zurück', uids)
    time.sleep(2)
    bild('4-online')


def main():
    _, anzahl = referenz()
    uids = sorted(anzahl)
    log = open(LOG, 'w')
    app = subprocess.Popen([BIN], stdout=log, stderr=subprocess.STDOUT)
    try:
        w = fenster()
        if w is None:
            falsch('kein Fenster')
            return 1
        w.configure(width=1200, height=760)
        d.sync()
        if NAME.endswith('-vorbereiten'):
            vorbereiten()
        else:
            wechsel(uids)
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
        log.close()
    rest = [z.strip() for z in open(LOG, encoding='utf-8', errors='replace')
            if not z.startswith('(revolution:') and not z.startswith('libEGL warning:') and z.strip()]
    rest += warnungen(0)
    (falsch if rest else ok)('Log: %s' % ('%d unerwartete Zeilen: %s' % (len(rest), rest[:3]) if rest else 'nur DEBUG-Zeilen'))
    return 1 if fehler else 0


if __name__ == '__main__':
    sys.exit(main())
