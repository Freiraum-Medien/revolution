#!/usr/bin/env python3
# netz-treiber.py – Treiber für tests/netz.sh (#408), läuft im Xvfb aus xvfb-run im Netz-Namensraum.
#
# Startet die App (BIN) mit Log, schaltet das Netz über tests/netz-relay.py (CTRL: „up“/„down“) und
# prüft je Schritt Log, Verbindungen nach außen (Log des Relais, RELAIS_LOG) und Screenshot.
#   NAME …-vorbereiten  Start mit Netz: Ordner ORDNER und Nachricht REVOLUTION_MESSAGE laden (Cache)
#   NAME …              Start ohne Netz: Ordner aller Konten und Nachricht aus dem Cache, Pille
#                       „Offline“; Netz zurück: online nach 5 s, Konten verbunden, Liste vom Server,
#                       Pille weg; Netz weg zur Laufzeit: offline, F5 aus dem Cache; Netz zurück.
#   PHASEN=kaputt (#421) Start mit Netzmonitor „online“, aber jede Verbindung scheitert (Relais
#                       „kaputt“): Ordner, Liste und Nachricht aus dem Cache, Banner „Keine
#                       Verbindung“, Zähler wie am Ende des vorigen Laufs (VORHER); Verbindung zurück
#                       (Relais „up“ mit Netzwechsel): Konten verbunden, Ordner und Liste vom Server.
#   PHASEN=kaputt-f5     wie kaputt, aber Relais „heil“ ohne Netzwechsel: nichts verbindet von selbst;
#                       F5 verbindet die Konten, Ordner und Liste vom Server, Banner weg.
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
import traceback

from PIL import Image
from Xlib import X, XK, display, protocol
from Xlib.ext import xtest

BIN, OUT, NAME, APP_DIR = os.environ['BIN'], os.environ['OUT'], os.environ['NAME'], os.environ['APP_DIR']
CTRL, RELAIS_LOG, REF = os.environ['CTRL'], os.environ['RELAIS_LOG'], os.environ['REF']
RELAIS_SOCK, KONTO = os.environ['RELAIS_SOCK'], os.environ['KONTO']
PHASEN = os.environ.get('PHASEN', 'start,abbruch,m365,flag,ordner').split(',')
ORDNER_KONTO = os.environ.get('ORDNER_KONTO', '')
# Sekunden ohne Netz je Zyklus: Gmail zählt getrennte Verbindungen einige Minuten weiter und lehnt
# ab 15 neue Anmeldungen ab („Too many simultaneous connections“, gemessen #408 R2: frei etwa
# 2–3 min nach der letzten Verbindung); mit der Pause bleibt der Test darunter.
PAUSE = float(os.environ.get('PAUSE', '20'))
# Konto, dessen Store nach dem Start ohne Netz aus der Session genommen wird (Test-Aktion
# app.test-store, REVOLUTION_TEST_HOOKS=1, main.c): Gmail nimmt nach vielen Netzwechseln keine
# Anmeldung mehr an (gemessen #408 R2); es hat einen eigenen Lauf mit wenigen Wechseln.
OHNE = os.environ.get('OHNE', '')
# #421: Konten, die die App nicht lädt (REVOLUTION_SKIP_ACCOUNTS, mail-account.c), und Log des vorigen
# Laufs für den Vergleich der Zähler.
SKIP = [u for u in os.environ.get('REVOLUTION_SKIP_ACCOUNTS', '').split(',') if u]
VORHER = os.environ.get('VORHER', '')
ORDNER = os.environ['REVOLUTION_FOLDER']
LOGKEY = ORDNER.replace('/', ' ', 1)
EINLADUNG = '[Revolution-Test] Einladung: Abstimmung Testdaten'
# Fenster 1200×1100, erste Zeile der Liste unter der Pille „Offline“ (m4-408-1-offline-start.png).
ZEILE_OFFLINE = (420, 170)
# Seitenleiste (Zeilen 30 px, m4-408-7-ordner-neu.png): Favoriten, dann NUR_LESEND_3@example.invalid
# (microsoft365, nur lesend) mit Posteingang und KAR, dann KONTO_M365@example.invalid mit Revolution-Test.
M365_POSTEINGANG = (120, 264)
M365_KAR = (120, 474)
OUTLOOK_REVOLUTION_TEST = (120, 1030)

fehler = 0
# #421: erwartete Warnungen (Phase kaputt: Anmeldung scheitert), im Abschluss-Check „Log“ ausgenommen.
erlaubt = set()
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


def testdaten(*args):
    """tests/testdaten.py im normalen Netz (über außen), unabhängig vom Netz der App."""
    r = subprocess.run([sys.executable, os.path.join(APP_DIR, 'tests', 'netz-relay.py'), 'testdaten', RELAIS_SOCK]
                       + list(args), capture_output=True, text=True, timeout=320)
    # #411: Exit ≠ 0 von testdaten.py (letzte Zeile von netz-relay.py) ist ein Abbruch des Schritts.
    rc = re.search(r'^TESTDATEN-EXIT (\d+)$', r.stdout, re.M)
    if rc is None or rc.group(1) != '0':
        falsch('tests/testdaten.py %s: %s' % (' '.join(args), (r.stdout + r.stderr).strip().replace('\n', ' | ')[-300:]))
    return r.stdout


def server_markiert():
    for zeile in testdaten('--flags', KONTO).splitlines():
        m = re.match(r'^FLAGS (\d) (\d) (.*?)( \(flag\.flagStatus=.*\))?$', zeile)
        if m and m.group(3) == EINLADUNG:
            return int(m.group(2))
    return None


def warte_server(soll, zeit):
    start = time.monotonic()
    while time.monotonic() - start < zeit:
        if server_markiert() == soll:
            return time.monotonic() - start
        time.sleep(3)
    return None


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

def klick(p, pause=0.5):
    xtest.fake_input(d, X.MotionNotify, x=p[0], y=p[1])
    xtest.fake_input(d, X.ButtonPress, 1)
    xtest.fake_input(d, X.ButtonRelease, 1)
    d.sync()
    time.sleep(pause)


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
               and not re.match(r'^Ordner: \S+ (\d+ geladen|nicht |abgeglichen|neu |entfernt )', t)}
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
    fehlt = warte_alle(r'^Ordner: %s (abgeglichen, \d+ neu, \d+ entfernt|\d+ geladen)$', uids, ab, 120)
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
    pruefe('%s: Session' % schritt, r'^Netz: nicht verfügbar, offline, \d+ Stores trennen$', ab, 20)
    pruefe('%s: Pille' % schritt, r'^Hinweis Offline: sichtbar$', ab, 10)
    fehlt = warte_alle(r'^offline: %s, Ordner aus dem Cache', uids, ab, 60)
    (falsch if fehlt else ok)('%s: %s' % (schritt, 'nicht offline: %s' % sorted(fehlt) if fehlt
                                          else 'alle %d Stores offline' % len(uids)))
    ab2 = marke()
    taste('F5')
    pruefe('%s: F5 ohne Netz' % schritt, r'^Aktualisieren: %s fertig$' % re.escape(LOGKEY), ab2, 30)
    time.sleep(3)
    ohne_warnungen(schritt, ab_datei)
    tcp = relais_zeilen()[tcp_vorher:]
    (ok if not tcp else falsch)('%s: keine Verbindung nach außen ohne Netz (%d)' % (schritt, len(tcp)))


def app_verbindungen():
    """TCP-Verbindungen der App nach außen (ss im Netz-Namensraum der App, Ziel wie beim connect)."""
    r = subprocess.run(['ss', '-tnpH'], capture_output=True, text=True)
    out = []
    for zeile in r.stdout.splitlines():
        teile = zeile.split()
        if '"revolution"' in zeile and len(teile) >= 5 and not teile[4].startswith('127.'):
            out.append('%s %s' % (teile[0], teile[4]))
    return out


def verbindungen_zu(schritt, vorher):
    """Nach „down“: die App hält keine Verbindung nach außen mehr (Beleg: kein App-Fehler, wenn ein
    Server wie Gmail Verbindungen noch zählt)."""
    time.sleep(2)
    nachher = app_verbindungen()
    wert('%s: Verbindungen der App vor „down“ %d: %s' % (schritt, len(vorher), sorted(vorher)[:8]))
    (ok if not nachher else falsch)('%s: Verbindungen der App 2 s nach „down“: %d %s' % (schritt, len(nachher), nachher[:8]))


def offline_schalten(schritt, uids):
    ab = marke()
    vorher = app_verbindungen()
    netz('down')
    pruefe('%s: Session' % schritt, r'^Netz: nicht verfügbar, offline, \d+ Stores trennen$', ab, 20)
    fehlt = warte_alle(r'^offline: %s, Ordner aus dem Cache', uids, ab, 60)
    (falsch if fehlt else ok)('%s: %s' % (schritt, 'nicht offline: %s' % sorted(fehlt) if fehlt
                                          else 'alle %d Stores offline' % len(uids)))
    verbindungen_zu(schritt, vorher)
    time.sleep(PAUSE)


def abbruch(uids, verzoegerung):
    """Netz weg, während die Stores nach „Netz zurück“ verbinden; danach müssen alle wieder verbinden."""
    schritt = 'Netz weg beim Verbinden %.1f s' % verzoegerung
    offline_schalten(schritt + ': vorher', uids)
    ab, ab_datei = marke(), dateizeilen()
    netz('up')
    if pruefe('%s: Session' % schritt, r'^Netz: online, \d+ Stores verbinden$', ab, 30) is None:
        return
    time.sleep(verzoegerung)
    ab2 = marke()
    vorher = app_verbindungen()
    netz('down')
    pruefe('%s: offline' % schritt, r'^Netz: nicht verfügbar, offline, \d+ Stores trennen$', ab2, 20)
    fehlt = warte_alle(r'^offline: %s, Ordner aus dem Cache', uids, ab2, 60)
    (falsch if fehlt else ok)('%s: %s' % (schritt, 'nicht offline: %s' % sorted(fehlt) if fehlt
                                          else 'alle %d Stores offline' % len(uids)))
    verbindungen_zu(schritt, vorher)
    time.sleep(PAUSE)
    abgebrochen = [t for _, t in log_zeilen()[ab:] if 'abgebrochen' in t]
    wert('%s: %d Aufträge abgebrochen, z. B. %s' % (schritt, len(abgebrochen), abgebrochen[:2]))
    ohne_warnungen(schritt, ab_datei)
    online_schalten(schritt + ': Netz zurück', uids)


def flag_offline(uids):
    """Markieren ohne Netz (nur lokal), nach „Netz zurück“ am Server; danach Ausgangszustand."""
    schritt = 'Flag ohne Netz %s' % KONTO
    vorher = server_markiert()
    if vorher != 0:
        falsch('%s: Einladung am Server markiert=%s, erwartet 0; übersprungen' % (schritt, vorher))
        return
    offline_schalten(schritt, uids)
    ab = marke()
    klick(ZEILE_OFFLINE)
    taste('Shift_L+Control_L+l')
    pruefe('%s: lokal' % schritt, r'^Flags: %s \S+ markiert lokal gespeichert \(offline\)$' % re.escape(LOGKEY), ab, 30)
    m = server_markiert()
    (ok if m == 0 else falsch)('%s: Server ohne Netz markiert=%s (erwartet 0)' % (schritt, m))
    bild('6-flag-offline')
    online_schalten(schritt + ': Netz zurück', uids)
    t = warte_server(1, 180)
    (ok if t is not None else falsch)('%s: Server markiert=1 nach Netz zurück%s' % (
        schritt, ' (%.0f s nach Liste vom Server)' % t if t is not None else ' fehlt nach 180 s'))
    ab = marke()
    taste('Shift_L+Control_L+l')
    pruefe('%s: zurück' % schritt, r'^Flags: %s \S+ nicht markiert gespeichert nach' % re.escape(LOGKEY), ab, 60)
    t = warte_server(0, 120)
    (ok if t is not None else falsch)('%s: Ausgangszustand am Server markiert=0' % schritt)


def ordner_abgleich(uids):
    """Ordner am Server anlegen bzw. löschen, während die App offline ist; nach „Netz zurück“ im
    Baum neu bzw. weg, ohne Ersetzen des Baums (Auswahl bleibt, kein neues Laden der Liste)."""
    uid = ORDNER_KONTO.split()[1]
    adresse = ORDNER_KONTO.split()[0]
    for aktion, log, nr in (('anlegen', 'neu', '7-ordner-neu'), ('loeschen', 'entfernt', '8-ordner-entfernt')):
        schritt = 'Ordner %s %s' % (aktion, adresse)
        offline_schalten(schritt, uids)
        out = testdaten('--testordner', adresse, aktion)
        name = re.search(r'^TESTORDNER (.*)$', out, re.M)
        if name is None:
            falsch('%s: am Server: %s' % (schritt, out.strip()[-300:]))
            return
        ok('%s: am Server %s' % (schritt, name.group(1)))
        ab = marke()
        online_schalten(schritt + ': Netz zurück', uids)
        pruefe('%s: Baum' % schritt, r'^Ordner: %s %s %s$' % (re.escape(uid), log, re.escape(name.group(1))), ab, 30)
        neu_geladen = [t for _, t in log_zeilen()[ab:] if t.startswith('Nachrichten: ') and t.endswith(' lädt')]
        (falsch if neu_geladen else ok)('%s: Auswahl bleibt (%s)' % (schritt, neu_geladen or 'Liste nicht neu geladen'))
        bild(nr)


def m365_laden(uids):
    """Netz weg 0,1/0,2 s nach Klick auf einen microsoft365-Ordner (Laden läuft, Prüfer #408 R2:
    soup_session_abort brach das Cancellable der Ansicht ab), je 4×; danach lädt der Ordner ohne
    Abbruch, F5 fertig, keine Warnung."""
    namen = {}
    with open(REF, encoding='utf-8') as f:
        for zeile in f:
            teile = zeile.split()
            # #409: Leerzeilen in REF (IndexError, Phase m365 brach ab)
            if len(teile) > 3 and teile[0] == 'ANZAHL':
                namen[teile[3]] = teile[1]
    uid = namen['NUR_LESEND_3@example.invalid']
    key = re.escape('%s Posteingang' % uid)
    ab_datei = dateizeilen()
    for nr, verzoegerung in enumerate((0.1, 0.2) * 4):
        schritt = 'microsoft365 laden %d (Netz weg %.1f s nach Klick)' % (nr + 1, verzoegerung)
        klick(M365_KAR)
        time.sleep(2)
        ab = marke()
        klick(M365_POSTEINGANG, verzoegerung)
        vorher = app_verbindungen()
        netz('down')
        pruefe('%s: offline' % schritt, r'^Netz: nicht verfügbar, offline', ab, 20)
        pruefe('%s: lädt' % schritt, r'^Nachrichten: %s lädt$' % key, ab, 10)
        verbindungen_zu(schritt, vorher)
        time.sleep(PAUSE)
        ab = marke()
        netz('up')
        pruefe('%s: online' % schritt, r'^Netz: online, \d+ Stores verbinden$', ab, 20)
        fehlt = warte_alle(r'^verbunden: %s$', uids, ab, 120)
        (falsch if fehlt else ok)('%s: %s' % (schritt, 'nicht verbunden: %s' % sorted(fehlt) if fehlt
                                              else 'alle %d Konten verbunden' % len(uids)))
        pruefe('%s: Liste vom Server' % schritt, r'^Aktualisieren: %s fertig$' % key, ab, 120)
    for nr in range(2):
        schritt = 'microsoft365 laden danach %d' % (nr + 1)
        klick(M365_KAR)
        time.sleep(2)
        ab = marke()
        klick(M365_POSTEINGANG)
        pruefe(schritt, r'^Nachrichten: %s vollständig' % key, ab, 60)
        time.sleep(3)
        ab2 = marke()
        taste('F5')
        pruefe('%s: F5' % schritt, r'^Aktualisieren: %s fertig$' % key, ab2, 60)
        abbruch = [t for _, t in log_zeilen()[ab:] if 'abgebrochen' in t or 'Stand des Caches' in t]
        (falsch if abbruch else ok)('%s: %s' % (schritt, 'Abbruch: %s' % abbruch[:2] if abbruch
                                                 else 'kein Abbruch, kein Cache-Stand'))
    ohne_warnungen('microsoft365 laden', ab_datei)
    bild('9-m365-laden')
    ab = marke()
    klick(OUTLOOK_REVOLUTION_TEST)
    pruefe('zurück zu %s' % ORDNER, r'^Nachrichten: %s vollständig' % re.escape(LOGKEY), ab, 60)
    time.sleep(2)


def zaehler(pfad):
    """Letzter Stand „Favoriten: Anzahl inbox UID ORDNER N“ je UID (mail-favorites.c, log_counts)."""
    out = {}
    with open(pfad, encoding='utf-8', errors='replace') as f:
        for zeile in f:
            m = re.search(r'revolution-DEBUG: \S+: Favoriten: Anzahl inbox (\S+) .* (\d+)$', zeile.rstrip('\n'))
            if m:
                out[m.group(1)] = int(m.group(2))
    return out


def kaputt(uids):
    """#421: Netzmonitor „online“, aber keine Verbindung; danach Verbindung zurück mit Netzwechsel."""
    schritt = 'Verbindung scheitert'
    fehlt = warte_alle(r'^Verbindung gescheitert: %s, Ordner aus dem Cache \(', uids, 0, 90)
    (falsch if fehlt else ok)('%s: %s' % (schritt, 'ohne Log „Verbindung gescheitert“: %s' % sorted(fehlt) if fehlt
                                          else 'alle %d Stores offline geschaltet, Ordner aus dem Cache' % len(uids)))
    fehlt = warte_alle(r'^Ordner: %s \d+ geladen$', uids, 0, 60)
    (falsch if fehlt else ok)('%s: %s' % (schritt, 'keine Ordner: %s' % sorted(fehlt) if fehlt
                                          else 'Ordner aller %d Konten aus dem Cache' % len(uids)))
    ordner_wie_server(schritt, uids, 0)
    pruefe('%s: Liste aus dem Cache' % schritt, r'^Nachrichten: %s vollständig' % re.escape(LOGKEY), 0, 60)
    pruefe('%s: Abgleich übersprungen' % schritt, r'^Nachrichten: %s offline, Stand des Caches' % re.escape(LOGKEY), 0, 60)
    pruefe('%s: Nachricht aus dem Cache' % schritt, r'^Nachricht: %s \S+ geladen nach' % re.escape(LOGKEY), 0, 60)
    time.sleep(5)
    texte = [t for _, t in log_zeilen()]
    offline = [t for t in texte if t.startswith('Netz: nicht verfügbar') or t == 'Hinweis Offline: sichtbar']
    (falsch if offline else ok)('%s: Netzmonitor online, keine Pille „Offline“ (%s)' % (schritt, offline[:2] or 'ok'))
    ohne_warnungen(schritt, 0, erlaubt=(': Anmeldung ', ))
    anmeldung = [z for z in warnungen(0) if ': Anmeldung ' in z]
    erlaubt.update(anmeldung)
    (ok if len(anmeldung) == len(uids) else falsch)('%s: %d Warnungen „Anmeldung …“ (Banner „Keine Verbindung“), '
                                                    'erwartet %d' % (schritt, len(anmeldung), len(uids)))
    (ok if not relais_zeilen() else falsch)('%s: keine Verbindung nach außen (%d)' % (schritt, len(relais_zeilen())))
    if VORHER and os.path.exists(VORHER):
        vorher, jetzt = zaehler(VORHER), zaehler(LOG)
        for uid in uids:
            if uid in vorher:
                (ok if jetzt.get(uid) == vorher[uid] else falsch)('%s: Zähler Alle Eingänge %s: %s, vorher %s' % (
                    schritt, uid[:8], jetzt.get(uid), vorher[uid]))
    bild('1-kaputt')

    ab, ab_datei = marke(), dateizeilen()
    if 'kaputt-f5' in PHASEN:
        schritt = 'Verbindung zurück ohne Netzwechsel, F5'
        netz('heil')
        time.sleep(15)
        von_selbst = [t for _, t in log_zeilen()[ab:] if t.startswith('Netz: ') or t.startswith('verbunden: ')]
        (falsch if von_selbst else ok)('%s: 15 s ohne Netzwechsel nichts verbunden (%s)' % (schritt, von_selbst[:2] or 'ok'))
        ab = marke()
        taste('F5')
        pruefe('%s: F5' % schritt, r'^Aktualisieren: %s nach erneutem Verbinden$' % re.escape(LOGKEY), ab, 20)
    else:
        schritt = 'Verbindung zurück'
        netz('up')
        pruefe('%s: Netzwechsel' % schritt, r'^Netz: Netzwechsel, erneut verbinden in 5 s$', ab, 20)
    pruefe('%s: erneut verbinden' % schritt, r'^Netz: %d Stores erneut verbinden$' % len(uids), ab, 30)
    pruefe('%s: Banner' % schritt, r'^Banner: Keine Verbindung weg$', ab, 30)
    fehlt = warte_alle(r'^verbunden: %s$', uids, ab, 120)
    (falsch if fehlt else ok)('%s: %s' % (schritt, 'nicht verbunden: %s' % sorted(fehlt) if fehlt
                                          else 'alle %d Konten verbunden' % len(uids)))
    fehlt = warte_alle(r'^Ordner: %s abgeglichen, \d+ neu, \d+ entfernt$', uids, ab, 120)
    (falsch if fehlt else ok)('%s: %s' % (schritt, 'Ordnerliste nicht abgeglichen: %s' % sorted(fehlt) if fehlt
                                          else 'Ordnerliste aller Konten vom Server abgeglichen'))
    pruefe('%s: Liste vom Server' % schritt, r'^Aktualisieren: %s fertig$' % re.escape(LOGKEY), ab, 120)
    tcp = relais_zeilen()
    (ok if tcp else falsch)('%s: %d Verbindungen nach außen über das Relais, z. B. %s' % (schritt, len(tcp), tcp[:3]))
    time.sleep(3)
    ohne_warnungen(schritt, ab_datei)
    bild('2-zurueck')


def vorbereiten():
    pruefe('Nachricht', r'^Nachricht: %s \S+ geladen nach' % re.escape(LOGKEY), 0, 120)


def test_store(uid, vorhanden):
    pfad = '/' + os.environ['APP_ID'].replace('.', '/').replace('-', '_')
    r = subprocess.run(['gdbus', 'call', '--session', '-d', os.environ['APP_ID'], '-o', pfad, '-m',
                        'org.gtk.Actions.Activate', 'test-store',
                        "[<('%s', %s)>]" % (uid, 'true' if vorhanden else 'false'), '{}'],
                       capture_output=True, text=True, timeout=30)
    wert('app.test-store %s %s: %s' % (uid, vorhanden, (r.stdout + r.stderr).strip()))


def ohne_konto(uids):
    if not OHNE or OHNE not in uids:
        return
    ab = marke()
    test_store(OHNE, False)
    pruefe('Store %s aus der Session (eigener Lauf)' % OHNE, r'^Test: Store %s entfernt$' % re.escape(OHNE), ab, 20)
    uids.remove(OHNE)


def wechsel(uids):
    # 1. Start ohne Netz: Ordner und Nachricht aus dem Cache, Pille sichtbar, kein Verbindungsfehler.
    pruefe('Start ohne Netz', r'^Netz: nicht verfügbar, Start offline$', 0, 30)
    if 'start' not in PHASEN:
        ohne_konto(uids)
        online_schalten('Netz zurück', uids)
        weitere(uids)
        return
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
    ohne_konto(uids)

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
    weitere(uids)


def weitere(uids):
    # 5. Netz weg, während die Stores verbinden (verschiedene Zeitpunkte).
    if 'abbruch' in PHASEN or 'abbruch1' in PHASEN:
        for verzoegerung in (0.2, 0.4, 1.0) if 'abbruch' in PHASEN else (0.4,):
            abbruch(uids, verzoegerung)
        bild('5-nach-abbruch')
    # 6. Netz weg kurz nach Klick auf einen microsoft365-Ordner.
    if 'm365' in PHASEN:
        m365_laden(uids)
    # 7. Markieren ohne Netz, nach „Netz zurück“ am Server.
    if 'flag' in PHASEN:
        flag_offline(uids)
    # 8. Ordner am Server neu bzw. gelöscht, während offline.
    if 'ordner' in PHASEN and ORDNER_KONTO:
        ordner_abgleich(uids)


def main():
    _, anzahl = referenz()
    uids = sorted(u for u in anzahl if u not in SKIP)
    log = open(LOG, 'w')
    app = subprocess.Popen([BIN], stdout=log, stderr=subprocess.STDOUT)
    try:
        w = fenster()
        if w is None:
            falsch('kein Fenster')
            return 1
        w.configure(width=1200, height=1100)
        d.sync()
        if NAME.endswith('-vorbereiten'):
            vorbereiten()
        elif 'kaputt' in PHASEN or 'kaputt-f5' in PHASEN:
            kaputt(uids)
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
    rest += [z for z in warnungen(0) if z not in erlaubt]
    (falsch if rest else ok)('Log: %s' % ('%d unerwartete Zeilen: %s' % (len(rest), rest[:3]) if rest else 'nur DEBUG-Zeilen'))
    return 1 if fehler else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as e:  # #411: Abbruch (Traceback, Zeitlimit eines Aufrufs) zählt als FEHLER
        traceback.print_exc()
        falsch('Abbruch: %s: %s' % (type(e).__name__, str(e).replace('\n', ' ')[:300]))
        sys.exit(1)
