#!/usr/bin/env python3
# kalender-treiber.py – Treiber für tests/kalender.sh (#373), läuft im Xvfb aus xvfb-run.
#
# Startet die App (BIN), wechselt mit Strg+2 zum Kalender und prüft über Log (G_MESSAGES_DEBUG=revolution: „Kalender: …“
# aus src/calendar/calendar-view.c, „Termin: +|- …“ aus src/calendar/gcal/gcal-month-view.c) und Barrierefreiheitsbaum
# (AT-SPI):
#   MODUS=alle     Seitenleiste = VERGLEICH (tests/testdaten.py --kalender, eigener Prozess): je Kalender UID, Konto, Name,
#                  Farbe, nur lesend; Kopfzeilen je Konto (AT-SPI). Tastatur: Bild ab/auf (Monat), Strg+T (heute). Monat
#                  MONAT: Termine je Kalender in KALENDER_TERMINE = tests/testdaten.py --kalender-termine (Python-GI
#                  ECal.Client); für die freigegebenen Kalender vorher Testtermine von außen (einzeln + wöchentlich) und
#                  dazu = Server ohne EDS (--kalender-server). Wiederholung: alle 5 Wochen. Live: anlegen, ändern,
#                  löschen von außen (zweiter Python-GI-Prozess) in LIVE_KALENDER, Zeit bis zur App ≤ 5 s. Ein/Aus in der
#                  Seitenleiste (AUS_KALENDER): Termine weg, Neustart (Einstellungen in Datei), weiter aus; wieder an.
#                  #374: „+N“-Popover öffnen und mit Escape schließen, Monat per Mausrad scrollen (Titelwechsel).
#   MODUS=woche    #374: Testwoche (Woche nach heute) von außen im ersten Kalender aus SCHREIBBAR: drei überlappende Termine
#                  am Dienstag, ein ganztägiger am Mittwoch, ein mehrtägiger Do–Sa. Lage per AT-SPI (Ausdehnung der
#                  Termine und des Wochenrasters GcalWeekGrid): überlappende nebeneinander in der richtigen Spalte, Höhe
#                  und Lage nach Uhrzeit; ganztägig/mehrtägig in der Kopfleiste über den richtigen Tagen. Tag (Alt+3) =
#                  dieselben Termine wie die Wochenspalte. Heute-Linie (2 px hoch im Raster) in der Spalte von heute auf
#                  der aktuellen Uhrzeit. Navigation vor/zurück/Heute in Woche und Tag, Alt+1/2/3, Strg+Bild ab/auf
#                  (Log „Kalender: Ansicht <Name> Datum <Tag>“). Live anlegen/ändern/löschen von außen ≤ 5 s in der Woche.
#   MODUS=bild     nur Start, Monat MONAT, Bild; #374 dazu Woche und Tag (dunkel/schmal; kalender.sh setzt Schema und
#                  Größe).
# Umgebung: BIN, OUT, NAME, MODUS, VERGLEICH, MONAT, KALENDER_TERMINE (UIDs), SCHREIBBAR (UIDs), LIVE_KALENDER (UIDs),
#           AUS_KALENDER (UID), TERMINE (Ordner für die Termin-Dateien), TESTDATEN, TIMEOUT, LIVE_GRENZE (s, Standard 5).
# Ausgabe: „OK …“, „FEHLER …“, „WERT …“, „SCHREIBVORGANG <Kalender>“; Bilder OUT/NAME-<Schritt>.png.
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

import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi, GLib  # noqa: E402
from PIL import Image  # noqa: E402
from Xlib import X, XK, display, protocol  # noqa: E402
from Xlib.ext import xtest  # noqa: E402

BIN, OUT, NAME = os.environ['BIN'], os.environ['OUT'], os.environ['NAME']
MODUS = os.environ.get('MODUS', 'alle')
VERGLEICH = os.environ['VERGLEICH']
MONAT = os.environ['MONAT']
KALENDER_TERMINE = os.environ.get('KALENDER_TERMINE', '').split()
SCHREIBBAR = os.environ.get('SCHREIBBAR', '').split()
LIVE_KALENDER = os.environ.get('LIVE_KALENDER', '').split()
AUS_KALENDER = os.environ.get('AUS_KALENDER', '')
TERMINE = os.environ.get('TERMINE', OUT)
TESTDATEN = os.environ['TESTDATEN']
TIMEOUT = int(os.environ.get('TIMEOUT', '120'))
LIVE_GRENZE = float(os.environ.get('LIVE_GRENZE', '5'))
LOG = os.path.join(OUT, NAME + '.log')
MONATE = ('Januar', 'Februar', 'März', 'April', 'Mai', 'Juni', 'Juli', 'August', 'September', 'Oktober',
          'November', 'Dezember')

fehler = 0
d = display.Display()
app = None
log_datei = None
log_start = 0  # Beginn des Logs des laufenden App-Starts (Neustart hängt an)
schreibvorgaenge = {}
# #375: Schreibvorgänge der vorigen Teile desselben kalender.sh-Laufs („UID=n …“), Grenze 20 je Kalender und Lauf.
SCHREIB_VORHER = {k: int(v) for k, v in (x.split('=') for x in os.environ.get('SCHREIB_VORHER', '').split() if '=' in x)}


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
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        m = re.search(muster, log_text()[ab:], re.M)
        if m:
            return m
        time.sleep(0.1)
    return None


def testdaten(*args):
    """tests/testdaten.py in eigenem Prozess; Ausgabe zurück, Abbruch → FEHLER. Höchstens 20 Schreibvorgänge je
    Kalender und Lauf (Briefing Zusatz M7): vor dem 21. bricht der Treiber ab."""
    if args[0] == '--termin-live' and args[1] != 'pruefen':
        # #375: Grenze je Lauf über alle Teile (kalender.sh: SCHREIB_VORHER aus den vorigen Teilen).
        if schreibvorgaenge.get(args[2], 0) + SCHREIB_VORHER.get(args[2], 0) >= 20:
            raise RuntimeError('20 Schreibvorgänge in %s erreicht (Lauf)' % args[2])
    r = subprocess.run([sys.executable, TESTDATEN] + list(args), capture_output=True, text=True, timeout=300)
    for z in (r.stdout + r.stderr).splitlines():
        print('        testdaten %s: %s' % (args[0], z), flush=True)
        if z.startswith('SCHREIBVORGANG '):
            schreibvorgaenge[z.split()[1]] = schreibvorgaenge.get(z.split()[1], 0) + 1
            print(z, flush=True)
    if r.returncode != 0:
        raise RuntimeError('tests/testdaten.py %s: Exit %d' % (' '.join(args), r.returncode))
    return r.stdout


def taste(*namen):
    codes = [d.keysym_to_keycode(XK.string_to_keysym(n)) for n in namen]
    for c in codes:
        xtest.fake_input(d, X.KeyPress, c)
    for c in reversed(codes):
        xtest.fake_input(d, X.KeyRelease, c)
    d.sync()
    time.sleep(0.3)


def klick(x, y):
    xtest.fake_input(d, X.MotionNotify, x=x, y=y)
    xtest.fake_input(d, X.ButtonPress, 1)
    xtest.fake_input(d, X.ButtonRelease, 1)
    d.sync()
    time.sleep(0.5)


def bild(schritt):
    root = d.screen().root
    g = root.get_geometry()
    pfad = os.path.join(OUT, '%s-%s.png' % (NAME, schritt))
    Image.frombytes('RGB', (g.width, g.height), root.get_image(0, 0, g.width, g.height, X.ZPixmap,
                                                               0xffffffff).data, 'raw', 'BGRX').save(pfad)
    wert('Bild %s' % pfad)


def fenster(titel='Revolution', zeit=20):
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        for w in d.screen().root.query_tree().children:
            try:
                if w.get_wm_name() == titel and w.get_attributes().map_state == X.IsViewable:
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
    if o is None:
        return None
    try:
        if (rolle is None or o.get_role_name() == rolle) and (name is None or o.get_name() == name) and \
                o.get_state_set().contains(Atspi.StateType.SHOWING):
            return o
        n = o.get_child_count()
    except Exception:
        return None
    for i in range(n):
        k = o.get_child_at_index(i)
        t = k and finde(k, rolle, name)
        if t:
            return t
    return None


def kalenderliste():
    return finde(wurzel(), 'list', 'Kalender')


def eintraege(liste):
    """[(Rolle, Name, angehakt)] der Kinder der Kalenderliste (Kopfzeilen „row header“, Einträge „list item“)."""
    out = []
    for i in range(liste.get_child_count()):
        k = liste.get_child_at_index(i)
        if k is None:
            continue
        box = finde(k, 'check box', None)
        out.append((k.get_role_name(), k.get_name(),
                    box.get_state_set().contains(Atspi.StateType.CHECKED) if box is not None else None))
    return out


# GTK 4.22 liefert unter Xvfb ohne Fenstermanager für SCREEN nur 0,0; WINDOW-Koordinaten sind gegen das Bild um den
# Rand der Fensterdekoration verschoben (gemessen 29.10.2026: Knopf „Termin hinzufügen“ WINDOW 1339,839 ↔ Bild 1354,854).
VERSATZ = int(os.environ.get('VERSATZ', '15'))


def mitte(o):
    r = o.get_component_iface().get_extents(Atspi.CoordType.WINDOW)
    return r.x + r.width // 2 + VERSATZ, r.y + r.height // 2 + VERSATZ


# ------------------------------------------------------------------ Log der Termine

def vergleich():
    """{UID: (Konto / Name, Farbe, nurlesend)} aus tests/testdaten.py --kalender."""
    out = {}
    for z in open(VERGLEICH):
        m = re.match(r'^KALENDER (\S+) (.*) farbe (\S+) nurlesend (\d)$', z.strip())
        if m:
            out[m.group(1)] = (m.group(2), m.group(3), m.group(4) == '1')
    return out


def termine_app():
    """{Termin-UID: (Kalender, Komponenten-UID, Beginn, Ende, ganztags)} nach allen +/- im Log dieses Starts."""
    stand = {}
    for m in re.finditer(r'Termin: ([+-]) (\S+) (\S+) (\S+) (\S+) ganztags (\d)', log_text()[log_start:]):
        if m.group(1) == '+':
            stand[m.group(3)] = (m.group(2), m.group(3)[len(m.group(2)) + 1:].split(':')[0], m.group(4), m.group(5),
                                 m.group(6) == '1')
        else:
            stand.pop(m.group(3), None)
    return stand


def norm(zeit, ganztags):
    """App-Log (UTC ISO 8601) → wie tests/testdaten.py ical_zeit: Datum bei ganztägigen Terminen mit DATE-Werten
    (GcalEvent legt sie in UTC an, gcal-event.c get_timezone_from_ical), sonst der Zeitpunkt in UTC. GNOME Calendar
    zeigt auch Termine von Mitternacht bis Mitternacht ihrer Zeitzone als ganztägig (gcal-event.c:375-405); deren
    Zeitpunkt vergleicht der Treiber wie Python-GI als UTC-Zeit."""
    t = datetime.datetime.fromisoformat(zeit.replace('Z', '+00:00')).astimezone(datetime.timezone.utc)
    if ganztags and t.strftime('%H:%M:%S') == '00:00:00':
        return t.strftime('%Y-%m-%d')
    return t.strftime('%Y-%m-%dT%H:%M:%SZ')


def grenzen(monat=None):
    j, m = (int(x) for x in (monat or MONAT).split('-'))
    return datetime.datetime(j, m, 1).astimezone(), datetime.datetime(j + (m == 12), m % 12 + 1, 1).astimezone()


def im_monat(start, ende, ganztags, monat=None):
    a, e = grenzen(monat)
    if ganztags and len(start) == 10:
        s = datetime.date.fromisoformat(start[:10])
        en = datetime.date.fromisoformat(ende[:10])
        return s < e.date() and en > a.date()
    s = datetime.datetime.fromisoformat(start.replace('Z', '+00:00'))
    en = datetime.datetime.fromisoformat(ende.replace('Z', '+00:00'))
    return s < e and (en > a or (en == s and s >= a))


def app_monat(kal, monat=None):
    """[(Komponenten-UID, Beginn, Ende, ganztags)] des Kalenders im Monat, wie tests/testdaten.py sortiert."""
    out = []
    for cal, uid, s, e, g in termine_app().values():
        s, e = norm(s, g), norm(e, g)
        if cal == kal and im_monat(s, e, g, monat):
            out.append((uid, s, e))
    return sorted(out)


def zeilen_termine(text, kopf):
    return sorted((m.group(1), m.group(2), m.group(3)) for m in
                  re.finditer(r'^%s (\S+) (\S+) (\S+) ganztags (\d)$' % kopf, text, re.M))


def warte_gleich(kal, soll, zeit=TIMEOUT, monat=None):
    ende = time.monotonic() + zeit
    while True:
        ist = app_monat(kal, monat)
        if ist == soll or time.monotonic() > ende:
            return ist
        time.sleep(0.5)


def monat_titel():
    j, m = (int(x) for x in MONAT.split('-'))
    return '%s %d' % (MONATE[m - 1], j)


# ------------------------------------------------------------------ Schritte

def seitenleiste(soll):
    ende = time.monotonic() + TIMEOUT
    while time.monotonic() < ende:
        log = {m.group(1): (m.group(2), m.group(3), m.group(4) == '1') for m in re.finditer(
            r'Kalender: (\S+) (.*) farbe (\S+) nurlesend (\d) sichtbar \d$', log_text(), re.M)}
        if len(log) >= len(soll):
            break
        time.sleep(0.5)
    (ok if log == soll else falsch)('Seitenleiste: %d Kalender = Registry (Python-GI %d, UID, Konto / Name, Farbe, '
                                    'nur lesend)%s' % (len(log), len(soll), '' if log == soll else ': fehlt %s, anders %s' % (
                                        sorted(set(soll) - set(log)),
                                        sorted(u for u in log if u in soll and log[u] != soll[u]))))
    if MODUS == 'bild':
        return  # schmal: Seitenleiste eingeklappt (≤ 1000 sp, calendar-view.blp)
    liste = kalenderliste()
    if liste is None:
        falsch('Seitenleiste: Liste „Kalender“ nicht im Barrierefreiheitsbaum')
        return
    z = eintraege(liste)
    namen = sorted(n for r, n, _ in z if r != 'row header')
    koepfe = [n for r, n, _ in z if r == 'row header']
    soll_namen = sorted(t.split(' / ', 1)[1] for t, _, _ in soll.values())
    soll_koepfe = set(t.split(' / ', 1)[0] for t, _, _ in soll.values())
    (ok if namen == soll_namen else falsch)('Seitenleiste (AT-SPI): %d Einträge mit Namen = Registry' % len(namen))
    (ok if set(koepfe) == soll_koepfe and len(koepfe) == len(soll_koepfe) else falsch)(
        'Seitenleiste (AT-SPI): Kopfzeilen je Konto %s' % koepfe)
    (ok if koepfe and koepfe[0] == 'Auf diesem Rechner' else falsch)('Seitenleiste: „Auf diesem Rechner“ zuerst')
    (ok if all(c for r, _, c in z if r != 'row header') else falsch)('Seitenleiste: alle Kalender angehakt (sichtbar)')


def titel_ist(text, zeit=10):
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        if finde(wurzel(), 'label', text) is not None:
            return True
        time.sleep(0.3)
    return False


def tastatur():
    heute = datetime.date.today()
    jetzt = '%s %d' % (MONATE[heute.month - 1], heute.year)
    (ok if titel_ist(jetzt) else falsch)('Strg+2: Kalender mit Titel „%s“' % jetzt)
    for name in ('Zurück', 'Vor', 'Heute', 'Termin hinzufügen', 'Seitenleiste ein-/ausblenden', 'Monat', 'Woche',
                 'Tag', 'Kalender', 'Hauptmenü'):
        o = finde(wurzel(), 'button', name) or finde(wurzel(), 'toggle button', name) or finde(wurzel(), None, name)
        (ok if o is not None else falsch)('a11y: Bedienelement „%s“' % name)
    nm = heute.replace(day=1) + datetime.timedelta(days=32)
    naechster = '%s %d' % (MONATE[nm.month - 1], nm.year)
    taste('Next')
    (ok if titel_ist(naechster) else falsch)('Bild ab: „%s“' % naechster)
    taste('Prior')
    taste('Prior')
    vm = heute.replace(day=1) - datetime.timedelta(days=1)
    (ok if titel_ist('%s %d' % (MONATE[vm.month - 1], vm.year)) else falsch)('Bild auf (2×): Vormonat')
    taste('Control_L', 't')
    (ok if titel_ist(jetzt) else falsch)('Strg+T: zurück zu heute „%s“' % jetzt)
    taste('Alt_L', 'Right')
    (ok if titel_ist(naechster) else falsch)('Alt+Rechts: „%s“' % naechster)
    taste('Home')
    (ok if titel_ist(jetzt) else falsch)('Pos1: heute „%s“' % jetzt)


def zum_monat():
    heute = datetime.date.today()
    j, m = (int(x) for x in MONAT.split('-'))
    schritte = (j - heute.year) * 12 + m - heute.month
    for _ in range(abs(schritte)):
        taste('Next' if schritte > 0 else 'Prior')
    (ok if titel_ist(monat_titel()) else falsch)('Monat %s gewählt (Titel „%s“)' % (MONAT, monat_titel()))


def datei(kal, art):
    return os.path.join(TERMINE, 'termin-%s-%s.txt' % (kal[:8], art))


def testtermine_anlegen():
    """Je freigegebenem Kalender ein Einzeltermin, im ersten dazu ein wöchentlicher (Wiederholung)."""
    j, m = (int(x) for x in MONAT.split('-'))
    erster_montag = next(datetime.date(j, m, t) for t in range(1, 8) if datetime.date(j, m, t).weekday() == 0)
    for i, kal in enumerate(SCHREIBBAR):
        testdaten('--termin-live', 'anlegen', kal, datei(kal, 'einzel'), '%04d-%02d-%02dT1%d:00' % (j, m, 10 + i, i))
    kal = SCHREIBBAR[0]
    testdaten('--termin-live', 'woechentlich', kal, datei(kal, 'woche'), '%sT08:30' % erster_montag.isoformat())
    return erster_montag


def termine_vergleichen(mit_server, monat=None):
    monat = monat or MONAT
    for kal in KALENDER_TERMINE:
        aus = testdaten('--kalender-termine', kal, monat)
        soll = zeilen_termine(aus, 'TERMIN')
        t = time.monotonic()
        ist = warte_gleich(kal, soll, 90, monat)
        (ok if ist == soll else falsch)('Termine %s %s: App %d = Python-GI %d (UIDs, Beginn, Ende)%s, nach %.1f s'
                                        % (kal[:8], monat, len(ist), len(soll),
                                           '' if ist == soll else ': nur App %s, nur Python-GI %s' % (
                                               sorted(set(ist) - set(soll))[:3], sorted(set(soll) - set(ist))[:3]),
                                           time.monotonic() - t))
        if mit_server and kal in SCHREIBBAR and kal != 'system-calendar':
            aus = testdaten('--kalender-server', kal, monat)
            server = zeilen_termine(aus, 'SERVER-TERMIN')
            # Graph liefert seine Ereignis-ID, CalDAV die iCalendar-UID: verglichen werden UIDs, Beginn, Ende.
            (ok if server == ist else falsch)('Termine %s %s: App %d = Server ohne EDS %d%s' % (
                kal[:8], monat, len(ist), len(server),
                '' if server == ist else ': nur App %s, nur Server %s' % (sorted(set(ist) - set(server))[:3],
                                                                         sorted(set(server) - set(ist))[:3])))


def wiederholung(erster_montag):
    kal = SCHREIBBAR[0]
    uid = open(datei(kal, 'woche')).read().split()[2]
    tage = sorted(s[:10] for u, s, _ in app_monat(kal) if u == uid)
    soll = [(erster_montag + datetime.timedelta(weeks=w)).isoformat() for w in range(5)
            if (erster_montag + datetime.timedelta(weeks=w)).month == erster_montag.month]
    (ok if tage == soll else falsch)('Wiederholung (wöchentlich, COUNT=5): Vorkommen am %s, erwartet %s' % (tage, soll))


def live():
    j, m = (int(x) for x in MONAT.split('-'))
    for i, kal in enumerate(LIVE_KALENDER):
        pfad = datei(kal, 'live')
        t = time.monotonic()
        ab = marke()
        aus = testdaten('--termin-live', 'anlegen', kal, pfad, '%04d-%02d-%02dT1%d:30' % (j, m, 20 + i, i))
        dauer_td = time.monotonic() - t
        uid = re.search(r'^ANGELEGT (\S+) (\S+)', aus, re.M).group(1)
        t = time.monotonic()
        mt = warte(r'Termin: \+ %s %s:%s \S+ \S+ ganztags \d UI-Thread (\S+)' % (re.escape(kal), re.escape(kal),
                                                                                 re.escape(uid)), ab, 60)
        dauer = time.monotonic() - t
        (ok if mt and dauer <= LIVE_GRENZE else falsch)(
            'Live anlegen %s: in der Monatsansicht nach %.1f s (≤ %.0f s, nach Ende des Schreibprozesses; Schreiben %.1f s), '
            'UI-Thread %s' % (kal[:8], dauer, LIVE_GRENZE, dauer_td, mt and mt.group(1)))
        time.sleep(1)
        bild('live-neu-%s' % kal[:8])
        ab = marke()
        aus = testdaten('--termin-live', 'aendern', kal, pfad)
        neu = re.search(r'^GEAENDERT \S+ (\S+)', aus, re.M).group(1)
        t = time.monotonic()
        mt = warte(r'Termin: \+ %s %s:%s %s' % (re.escape(kal), re.escape(kal), re.escape(uid), re.escape(neu[:16])), ab, 60)
        dauer = time.monotonic() - t
        (ok if mt and dauer <= LIVE_GRENZE else falsch)('Live ändern %s: neuer Beginn %s nach %.1f s' % (kal[:8], neu, dauer))
        ab = marke()
        testdaten('--termin-live', 'loeschen', kal, pfad)
        t = time.monotonic()
        mt = warte(r'Termin: - %s %s:%s ' % (re.escape(kal), re.escape(kal), re.escape(uid)), ab, 60)
        dauer = time.monotonic() - t
        (ok if mt and dauer <= LIVE_GRENZE and uid not in [u for u, _, _ in app_monat(kal)] else falsch)(
            'Live löschen %s: aus der Monatsansicht nach %.1f s' % (kal[:8], dauer))


def check_box(name):
    liste = kalenderliste()
    if liste is None:
        return None
    for i in range(liste.get_child_count()):
        k = liste.get_child_at_index(i)
        if k is not None and k.get_role_name() != 'row header' and k.get_name() == name:
            return finde(k, 'check box', None)
    return None


def aus_an(soll, an):
    name = soll[AUS_KALENDER][0].split(' / ', 1)[1]
    box = check_box(name)
    if box is None:
        falsch('Ein/Aus: Ankreuzfeld „%s“ nicht gefunden' % name)
        return
    ab = marke()
    klick(*mitte(box))
    wert('Ein/Aus: Klick auf „%s“ bei %s' % (name, mitte(box)))
    m = warte(r'Kalender: %s sichtbar %d$' % (re.escape(AUS_KALENDER), int(an)), ab, 10)
    (ok if m else falsch)('Ein/Aus: „%s“ %s (Log)' % (name, 'an' if an else 'aus'))
    ende = time.monotonic() + 30
    while (len(app_monat(AUS_KALENDER)) > 0) != an and time.monotonic() < ende:
        time.sleep(0.5)
    n = len(app_monat(AUS_KALENDER))
    (ok if (n > 0) == an else falsch)('Ein/Aus: Termine von „%s“ im Monat %s: %d' % (name, MONAT, n))
    box = check_box(name)
    (ok if box is not None and box.get_state_set().contains(Atspi.StateType.CHECKED) == an else falsch)(
        'Ein/Aus: Ankreuzfeld %s (AT-SPI)' % ('angehakt' if an else 'leer'))


def starten():
    global app, log_datei, log_start
    log_start = marke()
    log_datei = open(LOG, 'a')
    app = subprocess.Popen([BIN], stdout=log_datei, stderr=subprocess.STDOUT)
    if not fenster():
        falsch('kein Fenster')
        return False
    xtest.fake_input(d, X.MotionNotify, x=900, y=500)
    d.sync()
    time.sleep(2)
    ab = marke()
    taste('Control_L', '2')
    m = warte(r'Kalender: Start mit (\d+) Kalendern', ab, 30)
    (ok if m else falsch)('Strg+2: Kalender verbinden (%s Kalender der Registry)' % (m and m.group(1)))
    # Fokus in den Hauptbereich (Kürzel der Kalender-Ansicht wirken lokal, calendar-view.blp).
    klick(900, 60)
    return True


def beenden():
    w = fenster()
    rc = schliessen(w) if w else None
    (ok if rc == 0 else falsch)('Beenden: Exit %s' % rc)
    log_datei.close()


def andere_zeilen():
    andere = [z for z in log_text().splitlines()
              if z.strip() and 'revolution-DEBUG' not in z and not z.startswith('libEGL warning')]
    (ok if not andere else falsch)('keine Warnungen/Criticals im Log%s' % ('' if not andere else ': %s' % andere[:3]))


# ------------------------------------------------------------------ #374 Popover, Scrollen, Woche und Tag

def alle(o, rolle, out=None):
    """Alle sichtbaren Nachfahren von o mit der Rolle (None = alle)."""
    out = [] if out is None else out
    try:
        if (rolle is None or o.get_role_name() == rolle) and o.get_state_set().contains(Atspi.StateType.SHOWING):
            out.append(o)
        for i in range(o.get_child_count()):
            k = o.get_child_at_index(i)
            if k is not None:
                alle(k, rolle, out)
    except Exception:
        pass
    return out


def ausdehnung(o):
    r = o.get_component_iface().get_extents(Atspi.CoordType.WINDOW)
    return r.x, r.y, r.width, r.height


def popover_und_scrollen():
    """„+N“-Popover (GcalMonthPopover) öffnen, Escape schließt; Mausrad blättert den Monat (Titel wechselt)."""
    knoepfe = [b for b in alle(wurzel(), 'toggle button') if (b.get_name() or '').startswith('+')]
    if knoepfe:
        klick(*mitte(knoepfe[0]))
        time.sleep(1.5)
        (ok if finde(wurzel(), None, 'Neuer Termin …') else falsch)(
            '„%s“ geklickt: Popover mit „Neuer Termin …“ sichtbar' % knoepfe[0].get_name())
        bild('popover')
        taste('Escape')
        time.sleep(1)
        (ok if not finde(wurzel(), None, 'Neuer Termin …') else falsch)('Escape schließt das Popover')
    else:
        falsch('kein „+N“-Knopf im aktuellen Monat sichtbar')
    heute = datetime.date.today()
    jetzt = '%s %d' % (MONATE[heute.month - 1], heute.year)
    for _ in range(12):
        xtest.fake_input(d, X.MotionNotify, x=900, y=500)
        xtest.fake_input(d, X.ButtonPress, 5)
        xtest.fake_input(d, X.ButtonRelease, 5)
        d.sync()
        time.sleep(0.15)
    time.sleep(1.5)
    (ok if not titel_ist(jetzt, 2) else falsch)('Scrollen (12× Mausrad ab): Titel nicht mehr „%s“' % jetzt)
    taste('Control_L', 't')
    (ok if titel_ist(jetzt) else falsch)('Strg+T nach dem Scrollen: „%s“' % jetzt)


def ansicht(name, tag, ab, zeit=10):
    """Log „Kalender: Ansicht <name> Datum <tag>“ seit ab."""
    return warte(r'^.*Kalender: Ansicht %s Datum %s$' % (name, tag.isoformat()), ab, zeit) is not None


def taste_ansicht(tasten, name, tag, text):
    ab = marke()
    taste(*tasten)
    (ok if ansicht(name, tag, ab) else falsch)('%s: Ansicht %s, Datum %s (Log)' % (text, name, tag))


def raster(seite):
    """(Ansicht, GcalWeekGrid) der sichtbaren Woche bzw. des Tags: das Raster ist die Fläche über 24 Stunden
    (Höhe > 1000 px), deren Kinder die Termine und die Heute-Linie sind."""
    v = finde(wurzel(), 'panel', seite)
    if v is None:
        return None, None
    kandidaten = [p for p in alle(v, 'panel') if ausdehnung(p)[3] > 1000 and
                  any(p.get_child_at_index(i) is not None and p.get_child_at_index(i).get_role_name() == 'toggle button'
                      for i in range(p.get_child_count()))]
    return v, (kandidaten[0] if kandidaten else None)


def termine_in(v, gitter):
    """{Name: [(x, y, b, h, im Raster)]} aller Termine der Ansicht v."""
    out = {}
    kinder = set()
    if gitter is not None:
        for i in range(gitter.get_child_count()):
            k = gitter.get_child_at_index(i)
            if k is not None:
                kinder.add(ausdehnung(k) + (k.get_name(),))
    for b in alle(v, 'toggle button'):
        name = b.get_name() or ''
        if name.startswith('Zusätzliche Termine'):
            continue
        e = ausdehnung(b)
        out.setdefault(name, []).append(e + (e + (name,) in kinder,))
    return out


def spalte(gitter, x):
    gx, _, gb, _ = ausdehnung(gitter)
    return int((x - gx) // (gb / NSPALTEN[0]))


NSPALTEN = [7]


def warte_termine(seite, namen, zeit=60, weg=False):
    ende = time.monotonic() + zeit
    while True:
        try:
            v, g = raster(seite)
            t = termine_in(v, g) if v is not None else {}
        except GLib.Error:
            # #375: Termin-Widget verschwand während der Abfrage (Live-Änderung), neu lesen.
            v, g, t = None, None, {}
        if all((n in t) != weg for n in namen) or time.monotonic() > ende:
            return v, g, t
        time.sleep(0.2)


def testwoche_anlegen(kal, montag, kal_kopf=None):
    """Drei überlappende (Di 10:00, 10:30, 10:45, je 1 h), ganztägig (Mi), mehrtägig (Do 14:00 – Sa 16:00).
    #375: überlappende in kal, ganztägig/mehrtägig in kal_kopf (Schreibvorgänge auf zwei Kalender verteilt)."""
    namen = {}
    di, mi, do = (montag + datetime.timedelta(days=i) for i in (1, 2, 3))
    for art, aktion, wann in (('ueber1', 'anlegen', '%sT10:00' % di), ('ueber2', 'anlegen', '%sT10:30' % di),
                              ('ueber3', 'anlegen', '%sT10:45' % di), ('ganztags', 'ganztags', '%sT00:00' % mi),
                              ('mehrtaegig', 'mehrtaegig', '%sT14:00' % do)):
        k = kal_kopf if kal_kopf and art in ('ganztags', 'mehrtaegig') else kal
        aus = testdaten('--termin-live', aktion, k, datei(k, art), wann)
        namen[art] = re.search(r'^ANGELEGT \S+ \S+ (.+)$', aus, re.M).group(1)
    return namen


def lage_pruefen(seite, namen, tag_spalte, spalten):
    """Überlappende nebeneinander in tag_spalte, Lage/Höhe nach Uhrzeit; ganztägig/mehrtägig in der Kopfleiste."""
    NSPALTEN[0] = spalten
    v, g, t = warte_termine(seite, namen.values())
    if g is None or not all(n in t for n in namen.values()):
        falsch('%s: Testtermine nicht alle sichtbar (%s)' % (seite, sorted(set(namen.values()) - set(t))))
        return v, g, t
    gx, gy, gb, gh = ausdehnung(g)
    pro_minute = gh / 1440.0
    ueber = sorted((t[namen[a]][0] for a in ('ueber1', 'ueber2', 'ueber3')), key=lambda e: e[0])
    wert('%s: Raster x %d y %d Breite %d Höhe %d; überlappende %s' % (seite, gx, gy, gb, gh, [e[:4] for e in ueber]))
    (ok if all(e[4] for e in ueber) else falsch)('%s: überlappende Termine im Zeitraster (nicht Kopfleiste)' % seite)
    spalten_ist = [(spalte(g, e[0] + 2), spalte(g, e[0] + e[2] - 2)) for e in ueber]
    (ok if all(s == (tag_spalte, tag_spalte) for s in spalten_ist) else falsch)(
        '%s: überlappende Termine in Spalte %d: %s' % (seite, tag_spalte, spalten_ist))
    neben = all(ueber[i][0] + ueber[i][2] <= ueber[i + 1][0] + 2 for i in range(2))
    (ok if neben else falsch)('%s: überlappende Termine nebeneinander (x-Bereiche getrennt: %s)' % (
        seite, [(e[0], e[0] + e[2]) for e in ueber]))
    soll_y = {'ueber1': 600, 'ueber2': 630, 'ueber3': 645}
    for a, minuten in soll_y.items():
        e = t[namen[a]][0]
        dy, dh = e[1] - gy - minuten * pro_minute, e[3] - 60 * pro_minute
        (ok if abs(dy) <= 4 and abs(dh) <= 4 else falsch)(
            '%s: %s Beginn %02d:%02d bei y %+.1f px, Höhe 60 min %+.1f px (Toleranz 4 px)' % (
                seite, a, minuten // 60, minuten % 60, dy, dh))
    for a, von, bis in (('ganztags', 2, 2), ('mehrtaegig', 3, 5)):
        if a not in namen:
            continue
        es = t[namen[a]]
        anfang = min(spalte(g, e[0] + 2) for e in es)
        ende = max(spalte(g, e[0] + e[2] - 2) for e in es)
        (ok if not any(e[4] for e in es) and e[1] < gy + gh and (anfang, ende) == (von, bis) else falsch)(
            '%s: %s in der Kopfleiste über Spalte %d–%d (soll %d–%d)' % (seite, a, anfang, ende, von, bis))
    return v, g, t


def heute_linie(seite, spalte_heute):
    v, g = raster(seite)
    if g is None:
        falsch('%s: Raster nicht gefunden' % seite)
        return
    gx, gy, gb, gh = ausdehnung(g)
    jetzt = datetime.datetime.now()
    linien = []
    for i in range(g.get_child_count()):
        k = g.get_child_at_index(i)
        if k is not None and k.get_role_name() == 'panel' and k.get_state_set().contains(Atspi.StateType.SHOWING):
            e = ausdehnung(k)
            if e[3] <= 3:
                linien.append(e)
    soll = (jetzt.hour * 60 + jetzt.minute) * gh / 1440.0
    passt = [e for e in linien if spalte(g, e[0] + 2) == spalte_heute and abs(e[1] - gy - soll) <= 2 * gh / 1440.0 + 2]
    (ok if len(linien) == 1 and passt else falsch)(
        '%s: Heute-Linie %s in Spalte %d bei %02d:%02d (soll y %.0f)' % (seite, linien, spalte_heute, jetzt.hour,
                                                                         jetzt.minute, gy + soll))


def woche():
    # #375: Testwoche und Live im lokalen Kalender (ohne Server), ganztägig/mehrtägig im ersten (Gmail): ein voller Lauf
    # bleibt so unter 20 Schreibvorgängen je Kalender (PM 29.10.).
    kal = SCHREIBBAR[-1]
    heute = datetime.date.today()
    montag_heute = heute - datetime.timedelta(days=heute.weekday())
    montag = montag_heute + datetime.timedelta(days=7)
    namen = testwoche_anlegen(kal, montag, SCHREIBBAR[0])
    taste_ansicht(('Alt_L', '2'), 'week', heute, 'Alt+2')
    taste_ansicht(('Alt_L', 'Right'), 'week', heute + datetime.timedelta(days=7), 'Woche Alt+Rechts')
    v = finde(wurzel(), 'panel', 'Woche')
    tage = [int(x.get_name()) for x in alle(v, 'label') if (x.get_name() or '').isdigit()] if v else []
    soll_tage = [(montag + datetime.timedelta(days=i)).day for i in range(7)]
    (ok if tage[:7] == soll_tage else falsch)('Woche: Kopf zeigt Tage %s (soll %s, Montag zuerst)' % (tage[:7], soll_tage))
    v, g, t = lage_pruefen('Woche', namen, 1, 7)
    time.sleep(1)
    bild('woche')
    spalte_di = sorted((n, round((e[1] - ausdehnung(g)[1]) / ausdehnung(g)[3] * 1440)) for n, es in t.items()
                       for e in es if e[4] and spalte(g, e[0] + 2) == 1) if g else []
    kopf_di = sorted(n for n, es in t.items() for e in es if not e[4] and g and
                     spalte(g, e[0] + 2) <= 1 <= spalte(g, e[0] + e[2] - 2))
    # Tag: Alt+3 zeigt den gewählten Tag (heute + 7), dann zum Donnerstag der Testwoche, Alt+Links, Bild auf = Dienstag.
    # #375: unabhängig vom Wochentag des Laufs (vorher nur donnerstags richtig).
    taste_ansicht(('Alt_L', '3'), 'day', heute + datetime.timedelta(days=7), 'Alt+3')
    di = montag + datetime.timedelta(days=1)
    schritte = (heute + datetime.timedelta(days=7) - (di + datetime.timedelta(days=2))).days
    for _ in range(abs(schritte)):
        taste('Alt_L', 'Left' if schritte > 0 else 'Right')
    taste_ansicht(('Alt_L', 'Left'), 'day', di + datetime.timedelta(days=1), 'Tag Alt+Links')
    taste_ansicht(('Prior',), 'day', di, 'Tag Bild auf')
    v2, g2, t2 = lage_pruefen('Tag', {a: n for a, n in namen.items() if a.startswith('ueber')}, 0, 1)
    tag_raster = sorted((n, round((e[1] - ausdehnung(g2)[1]) / ausdehnung(g2)[3] * 1440)) for n, es in t2.items()
                        for e in es if e[4]) if g2 else []
    tag_kopf = sorted(n for n, es in t2.items() for e in es if not e[4])
    (ok if tag_raster == spalte_di and spalte_di else falsch)(
        'Tag %s = Wochenspalte: %d Termine im Raster (Name, Beginn in min)%s' % (
            di, len(tag_raster), '' if tag_raster == spalte_di else ': Tag %s, Woche %s' % (tag_raster, spalte_di)))
    (ok if tag_kopf == kopf_di else falsch)('Tag %s = Wochenspalte: Kopfleiste %s' % (di, tag_kopf))
    time.sleep(1)
    bild('tag')
    taste_ansicht(('Alt_L', 'Right'), 'day', di + datetime.timedelta(days=1), 'Tag Alt+Rechts')
    taste_ansicht(('Next',), 'day', di + datetime.timedelta(days=2), 'Tag Bild ab')
    taste_ansicht(('Control_L', 't'), 'day', heute, 'Tag Strg+T')
    NSPALTEN[0] = 1
    heute_linie('Tag', 0)
    taste_ansicht(('Control_L', 'Page_Up'), 'week', heute, 'Strg+Bild auf')
    NSPALTEN[0] = 7
    heute_linie('Woche', heute.weekday())
    taste_ansicht(('Next',), 'week', heute + datetime.timedelta(days=7), 'Woche Bild ab')
    taste_ansicht(('Alt_L', 'Left'), 'week', heute, 'Woche Alt+Links')
    taste_ansicht(('Prior',), 'week', heute - datetime.timedelta(days=7), 'Woche Bild auf')
    taste_ansicht(('Home',), 'week', heute, 'Woche Pos1')
    taste_ansicht(('Control_L', 'Page_Down'), 'day', heute, 'Strg+Bild ab')
    ab = marke()
    taste('Alt_L', '1')
    (ok if warte(r'Kalender: Ansicht month Datum ', ab, 10) else falsch)('Alt+1: Ansicht month (Log)')
    taste_ansicht(('Alt_L', '2'), 'week', heute, 'Alt+2 aus dem Monat')
    taste_ansicht(('Alt_L', 'Right'), 'week', heute + datetime.timedelta(days=7), 'Woche Alt+Rechts (Testwoche)')
    # Live von außen (zweiter Python-GI-Prozess), Mittwoch 15:00 der Testwoche (lokal; in outlook.com kam die neue Zeit
    # in der Woche nicht an, nur der Titel: m7-375-r3-final/-final2, offen als Folgepunkt).
    pfad = datei(kal, 'live')
    aus = testdaten('--termin-live', 'anlegen', kal, pfad, '%sT15:00' % (montag + datetime.timedelta(days=2)))
    name = re.search(r'^ANGELEGT \S+ \S+ (.+)$', aus, re.M).group(1)
    t0 = time.monotonic()
    _, g, t = warte_termine('Woche', [name], 30)
    dauer = time.monotonic() - t0
    sp = spalte(g, t[name][0][0] + 2) if g is not None and name in t else None
    (ok if name in t and dauer <= LIVE_GRENZE and sp == 2 else falsch)(
        'Live anlegen: in der Woche (Mittwoch, Spalte %s) nach %.1f s (≤ %.0f s, nach Ende des Schreibprozesses)' % (
            sp, dauer, LIVE_GRENZE))
    testdaten('--termin-live', 'aendern', kal, pfad)
    t0 = time.monotonic()
    # #375 Runde 3: Titel und Zeit können getrennt ankommen (outlook.com: erst der Titel, gemessen m7-375-r3-final);
    # gewartet wird auf beides.
    while True:
        _, g, t = warte_termine('Woche', [name + ' geändert'], 30)
        e = t.get(name + ' geändert', [None])[0]
        try:
            beginn = round((e[1] - ausdehnung(g)[1]) / ausdehnung(g)[3] * 1440) if e else None
        except GLib.Error:
            beginn = None
        if (beginn is not None and abs(beginn - 16 * 60) <= 3) or time.monotonic() - t0 > 30:
            break
        time.sleep(0.2)
    dauer = time.monotonic() - t0
    (ok if e and dauer <= LIVE_GRENZE and abs(beginn - 16 * 60) <= 3 and name not in t else falsch)(
        'Live ändern: neuer Titel und Beginn %s min (soll 960 = 16:00) nach %.1f s' % (beginn, dauer))
    testdaten('--termin-live', 'loeschen', kal, pfad)
    t0 = time.monotonic()
    _, g, t = warte_termine('Woche', [name + ' geändert'], 30, weg=True)
    dauer = time.monotonic() - t0
    (ok if name + ' geändert' not in t and dauer <= LIVE_GRENZE else falsch)(
        'Live löschen: aus der Woche nach %.1f s' % dauer)


# ------------------------------------------------------------------ #375 Termine anlegen, bearbeiten, löschen, Erinnerung

TASTEN = {' ': ('space',), '-': ('minus',), '.': ('period',), ',': ('comma',), ':': ('Shift_L', 'semicolon'),
          '/': ('slash',), '@': ('Shift_L', '2'), '+': ('Shift_L', 'equal')}


def tippe(text):
    """Text über die Tastatur (XTEST, US-Belegung des Xvfb)."""
    for ch in text:
        namen = TASTEN.get(ch) or (('Shift_L', ch.lower()) if ch.isupper() else (ch,))
        codes = [d.keysym_to_keycode(XK.string_to_keysym(n)) for n in namen]
        for c in codes:
            xtest.fake_input(d, X.KeyPress, c)
        for c in reversed(codes):
            xtest.fake_input(d, X.KeyRelease, c)
        d.sync()
        time.sleep(0.03)
    time.sleep(0.3)


def app_schreibt(kal, was):
    """Vor jedem Schreibvorgang der App: nur freigegebene Kalender, höchstens 20 je Kalender und Lauf (alle Teile)."""
    if kal not in SCHREIBBAR:
        raise RuntimeError('Schreiben in %s nicht freigegeben (%s)' % (kal, was))
    if schreibvorgaenge.get(kal, 0) + SCHREIB_VORHER.get(kal, 0) >= 20:
        raise RuntimeError('20 Schreibvorgänge in %s erreicht (Lauf)' % kal)
    schreibvorgaenge[kal] = schreibvorgaenge.get(kal, 0) + 1
    print('SCHREIBVORGANG %s (App: %s)' % (kal, was), flush=True)


def warte_obj(fn, zeit=10):
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        o = fn()
        if o is not None:
            return o
        time.sleep(0.2)
    return None


def dialog(zeit=10):
    """Editor (Adw.Dialog), erst wenn sein Inhalt im Barrierefreiheitsbaum steht (Kalenderwahl)."""
    def fertig():
        dlg = finde(wurzel(), 'dialog', None)
        return dlg if dlg is not None and finde(dlg, 'combo box', 'Kalender') is not None else None
    return warte_obj(fertig, zeit)


def druecke(o):
    """Knopf über AT-SPI auslösen (Popover-Flächen haben eigene Koordinaten), sonst Klick."""
    try:
        a = o.get_action_iface()
        if a is not None and a.get_n_actions() > 0:
            a.do_action(0)
            time.sleep(0.5)
            return
    except Exception:
        pass
    klick(*mitte(o))


def zeigen(dlg, o):
    """Element in den sichtbaren Teil des Editors scrollen (Mausrad über dem Inhalt), sonst träfe der Klick daneben."""
    sp = warte_obj(lambda: finde(dlg, 'scroll pane', None), 5)
    for _ in range(30 if sp is not None else 0):
        sx, sy, sb, sh = ausdehnung(sp)
        ox, oy, ob, oh = ausdehnung(o)
        if oy >= sy and oy + oh <= sy + sh:
            return
        xtest.fake_input(d, X.MotionNotify, x=sx + sb // 2 + VERSATZ, y=sy + sh // 2 + VERSATZ)
        knopf = 5 if oy + oh > sy + sh else 4
        xtest.fake_input(d, X.ButtonPress, knopf)
        xtest.fake_input(d, X.ButtonRelease, knopf)
        d.sync()
        time.sleep(0.15)
    time.sleep(0.3)


def feld_setzen(dlg, name, text):
    """Textfeld per Maus wählen, Inhalt per Tastatur ersetzen, Tab (Datum/Zeit werten beim Verlassen aus)."""
    o = finde(dlg, 'text', name)
    if o is None:
        raise RuntimeError('Feld „%s“ nicht gefunden' % name)
    zeigen(dlg, o)
    klick(*mitte(o))
    taste('Control_L', 'a')
    tippe(text)
    taste('Tab')


def kombi_auswahl(dlg, name):
    c = finde(dlg, 'combo box', name)
    lst = c and finde(c, 'list', None)
    namen = [x.get_name() for x in alle(lst, 'label')] if lst else []
    return namen[0] if namen else None


def kombi_eintraege(c):
    """Einträge (ohne Abschnittsköpfe) des offenen Popovers einer Adw.ComboRow in Reihenfolge."""
    sp = finde(c, 'scroll pane', None)
    out = []
    for lab in alle(sp, 'label') if sp else []:
        p, eintrag = lab.get_parent(), False
        for _ in range(5):
            if p is None:
                break
            if p.get_role_name() == 'list item':
                eintrag = True
                break
            p = p.get_parent()
        if eintrag:
            out.append(lab.get_name())
    return out


def kombi_waehlen(dlg, name, ziel, nummer=0):
    """Adw.ComboRow: Popover per Maus öffnen, Eintrag per Tastatur (Pos1, Ab, Eingabe); nummer = n-ter gleichnamiger."""
    c = finde(dlg, 'combo box', name)
    zeigen(dlg, c)
    klick(*mitte(c))
    time.sleep(1)
    eintraege = kombi_eintraege(c)
    treffer = [i for i, n in enumerate(eintraege) if n == ziel]
    if len(treffer) <= nummer:
        taste('Escape')
        raise RuntimeError('„%s“ nicht in %s: %s' % (ziel, name, eintraege))
    taste('Home')
    for _ in range(treffer[nummer]):
        taste('Down')
    taste('Return')
    time.sleep(0.5)
    return eintraege


def erinnerung_waehlen(dlg, text):
    knopf = finde(dlg, 'button', 'Erinnerung hinzufügen')
    zeigen(dlg, knopf)
    klick(*mitte(knopf))  # Gtk.ListBox::row-activated (Klick)
    time.sleep(1)
    o = warte_obj(lambda: finde(wurzel(), 'menu item', text), 5)
    if o is None:
        raise RuntimeError('Erinnerung „%s“ nicht im Popover' % text)
    druecke(o)
    time.sleep(0.5)


def speichern_und_warten(kal, art, ab, zeit=60):
    """Strg+Enter im Editor; Log „Kalender: <art> <kal> fertig“ bzw. „Fehler“."""
    app_schreibt(kal, art)
    taste('Control_L', 'Return')
    return warte(r'Kalender: %s %s (fertig|Fehler)(?: (\S+))?$' % (art, re.escape(kal)), ab, zeit)


def zeit_utc(tag, hh, mm):
    return datetime.datetime(tag.year, tag.month, tag.day, hh, mm).astimezone().astimezone(
        datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def felder(text, kopf):
    return {m.group(1): m.group(2) for m in re.finditer(r'^%s (\S+) (.*)$' % kopf, text, re.M) if m.group(1) != 'ausnahme'}


def ausnahmen(text, kopf):
    return sorted(re.findall(r'^%s ausnahme (\S+) titel (.*) beginn (\S+)$' % kopf, text, re.M))


def gegenprobe(kal, uid, soll, text, mit_server=True, ausnahme_soll=None):
    """Zweiter Prozess (Python-GI) und Server ohne EDS zeigen die Felder wie im Editor."""
    t0 = time.monotonic()
    while True:
        gi = testdaten('--termin-details', kal, uid)
        ist = felder(gi, 'GI')
        if all(ist.get(k) == v for k, v in soll.items()) or time.monotonic() - t0 > 60:
            break
        time.sleep(3)
    abw = {k: (ist.get(k), v) for k, v in soll.items() if ist.get(k) != v}
    (ok if not abw else falsch)('%s %s: Python-GI = Editor (%s)%s' % (text, kal[:8], ', '.join(sorted(soll)),
                                                                      '' if not abw else ': %s' % abw))
    if ausnahme_soll is not None:
        a = [(r, t) for r, t, _ in ausnahmen(gi, 'GI')]
        (ok if a == ausnahme_soll else falsch)('%s %s: Python-GI Ausnahmen %s (soll %s)' % (text, kal[:8], a, ausnahme_soll))
    if not mit_server or kal == 'system-calendar':
        return
    t0 = time.monotonic()
    while True:
        srv = testdaten('--termin-server', kal, uid)
        ist = felder(srv, 'SERVER')
        if all(ist.get(k) == v for k, v in soll.items()) or time.monotonic() - t0 > 90:
            break
        time.sleep(5)
    abw = {k: (ist.get(k), v) for k, v in soll.items() if ist.get(k) != v}
    (ok if not abw else falsch)('%s %s: Server ohne EDS = Editor%s' % (text, kal[:8], '' if not abw else ': %s' % abw))
    if ausnahme_soll is not None:
        a = [(r, t) for r, t, _ in ausnahmen(srv, 'SERVER')]
        (ok if a == ausnahme_soll else falsch)('%s %s: Server Ausnahmen %s (soll %s)' % (text, kal[:8], a, ausnahme_soll))


def termin_widget(name, zeit=30):
    return warte_obj(lambda: next((b for b in alle(wurzel(), 'toggle button') if b.get_name() == name), None), zeit)


def popover_oeffnen(name, eigene=None, zeit=30):
    """Klick auf den Termin; Log „Kalender: Popover <UID>“. eigene: erlaubte Termin-UIDs (Komponenten-UID)."""
    w = termin_widget(name, zeit)
    if w is None:
        raise RuntimeError('Termin „%s“ nicht sichtbar' % name)
    ab = marke()
    klick(*mitte(w))
    m = warte(r'Kalender: Popover (\S+)$', ab, 10)
    if m is None:
        raise RuntimeError('kein Popover für „%s“' % name)
    if eigene is not None and m.group(1).split(':')[1] not in eigene:
        taste('Escape')
        raise RuntimeError('Popover zeigt fremden Termin %s' % m.group(1))
    time.sleep(0.8)
    return m.group(1)


def bearbeiten(name, eigene, bilder=None):
    """Popover, „Bearbeiten“ (Fokus liegt auf dem Knopf, gcal-event-popover.c set_event_internal) → Editor."""
    termin = popover_oeffnen(name, eigene)
    if bilder:
        bild_beide(bilder)
    ab = marke()
    taste('Return')
    m = warte(r'Kalender: Editor bearbeiten (\S+) (\S+)$', ab, 10)
    if m is None or m.group(1) not in SCHREIBBAR or m.group(2) != termin:
        raise RuntimeError('Editor nicht für %s geöffnet (%s)' % (termin, m and m.group(0)))
    return dialog(), m.group(1)


def neuer_termin(dlg, kal_name, titel, ort, notiz, ganztags, tag, von=None, bis=None, erinnerung=None, woechentlich=False,
                 per_tastatur=True):
    """Felder im Editor setzen (Titel/Ort per Tastatur bzw. Maus, Kalender, Zeit, Wiederholung, Erinnerung, Notiz)."""
    if per_tastatur:
        tippe(titel)          # Fokus steht im Titel (gcal-summary-section)
        taste('Tab')
        tippe(ort)
    else:
        feld_setzen(dlg, 'Titel', titel)
        feld_setzen(dlg, 'Ort', ort)
    kombi_waehlen(dlg, 'Kalender', kal_name)
    if not ganztags:
        klick(*mitte(finde(dlg, 'radio button', 'Zeitfenster')))
        time.sleep(0.5)
        # Reihenfolge: Daten, Ende, Beginn (GcalEventSchedule verschiebt sonst das Ende bzw. den Beginn mit).
        feld_setzen(dlg, 'Anfangsdatum', tag.strftime('%d.%m.%Y'))
        feld_setzen(dlg, 'Enddatum', tag.strftime('%d.%m.%Y'))
        feld_setzen(dlg, 'Endzeit', bis)
        feld_setzen(dlg, 'Anfangszeit', von)
    else:
        feld_setzen(dlg, 'Anfangsdatum', tag.strftime('%d.%m.%Y'))
        feld_setzen(dlg, 'Enddatum', tag.strftime('%d.%m.%Y'))
    if woechentlich:
        kombi_waehlen(dlg, 'Wiederholen', 'Wöchentlich')
        kombi_waehlen(dlg, 'Wiederholung beenden', 'Anzahl Wiederholungen')
        # Runde 2: 3 Vorkommen (Spinbutton „Anzahl Wiederholungen“, Vorgabe 2), für „Nachfolgende Termine“.
        spin = finde(dlg, 'spin button', None)
        zeigen(dlg, spin)
        klick(*mitte(spin))
        taste('Control_L', 'a')
        tippe('3')
        taste('Tab')
    if erinnerung:
        erinnerung_waehlen(dlg, erinnerung)
    # Notiz über AT-SPI (EditableText), das Feld liegt meist unter dem sichtbaren Teil.
    finde(dlg, 'text', 'Notizen').get_editable_text_iface().set_text_contents(notiz)
    gewaehlt = kombi_auswahl(dlg, 'Kalender')
    if gewaehlt != kal_name:
        taste('Escape')
        raise RuntimeError('Kalenderwahl „%s“ statt „%s“, nicht gespeichert' % (gewaehlt, kal_name))


def eigene_datei(kal, art, uid, titel):
    with open(datei(kal, art), 'w') as f:
        f.write('TERMIN %s %s %s\n' % (kal, uid, titel))


def notify_mitschnitt():
    """dbus-monitor nur lesend auf Notify (fdo) und AddNotification (org.gtk.Notifications, GNotification von
    evolution-alarm-notify, e-alarm-notify.c:345)."""
    pfad = os.path.join(OUT, NAME + '-notify.txt')
    f = open(pfad, 'w')
    p = subprocess.Popen(['dbus-monitor', '--session', "interface='org.freedesktop.Notifications',member='Notify'",
                          "interface='org.gtk.Notifications',member='AddNotification'"], stdout=f, stderr=subprocess.STDOUT)
    return p, f, pfad


def zur_woche(heute, wochen=0):
    """Woche von heute (+ wochen): Monat, Strg+T, Woche, Alt+Rechts (Log je Schritt nicht nötig)."""
    taste('Alt_L', '1')
    taste('Control_L', 't')
    taste('Alt_L', '2')
    for _ in range(wochen):
        taste('Alt_L', 'Right')
    ab = marke()
    time.sleep(1.5)
    return ab


def thema(name):
    """Design der laufenden Test-App umschalten (app.theme über org.gtk.Actions, nur die Devel-Instanz dieses Laufs)."""
    app_id = os.environ['APP_ID']
    subprocess.run(['gdbus', 'call', '--session', '-d', app_id, '-o', '/' + app_id.replace('.', '/'), '-m',
                    'org.gtk.Actions.Activate', 'theme', "[<'%s'>]" % name, '{}'], capture_output=True, timeout=10)
    time.sleep(1.2)


def bild_beide(schritt):
    """Bild hell und dunkel (#375 DoD Screenshots), danach wieder hell."""
    bild(schritt + '-hell')
    thema('dunkel')
    bild(schritt + '-dunkel')
    thema('hell')


def doppelklick(x, y):
    xtest.fake_input(d, X.MotionNotify, x=x, y=y)
    d.sync()
    time.sleep(0.2)
    for _ in range(2):
        xtest.fake_input(d, X.ButtonPress, 1)
        xtest.fake_input(d, X.ButtonRelease, 1)
        d.sync()
        time.sleep(0.08)
    time.sleep(1)


def editor_felder():
    """Anfangsdatum, Enddatum, Anfangszeit, Endzeit (Text) des offenen Editors."""
    dlg = dialog()
    out = {}
    for name in ('Anfangsdatum', 'Enddatum', 'Anfangszeit', 'Endzeit'):
        o = finde(dlg, 'text', name) if dlg else None
        out[name] = Atspi.Text.get_text(o, 0, -1) if o else None
    return out


def editor_zu(ab, text):
    for _ in range(3):
        if dialog(1) is None:
            break
        taste('Escape')
    (ok if not warte(r'Kalender: anlegen ', ab, 1) else falsch)('%s: Escape, nichts geschrieben' % text)


def einfach_doppel(heute, montag):
    """Runde 2/3 (PM): leere Flächen der Testwoche (Samstag): Einfachklick öffnet keinen Editor, Doppelklick öffnet
    „Neuer Termin“ mit dem Tag (Raster, Kopfleiste, Monat); Ziehen über einen Bereich öffnet ihn mit dem Bereich
    (Raster 07:30–09:00, Monat Fr–Sa). Jeweils Escape, nichts geschrieben."""
    sa = montag + datetime.timedelta(days=5)
    fr = montag + datetime.timedelta(days=4)
    zur_woche(heute, 2)
    v, g = raster('Woche')
    gx, gy, gb, gh = ausdehnung(g)
    xs = tag_x(v, sa)
    zeile = lambda minuten: int(gy + minuten * gh / 1440.0) + VERSATZ
    lab = next((x for x in alle(v, 'label') if x.get_name() == str(sa.day)), None)
    kopf_y = ausdehnung(lab)[1] + ausdehnung(lab)[3] + 22 + VERSATZ
    faelle = [('Raster', xs, zeile(450) + 10), ('Kopfleiste', xs, kopf_y), ('Monat', None, None)]
    for ansicht, x, y in faelle:
        if ansicht == 'Monat':
            taste('Alt_L', '1')
            time.sleep(2)
            zellen = {c.get_name(): c for c in alle(finde(wurzel(), 'table', 'Monat'), 'table cell')}
            if str(sa.day) not in zellen or str(fr.day) not in zellen:
                falsch('Monat: Zellen %s/%s nicht gefunden' % (fr, sa))
                break
            cx, cy, cb, ch = ausdehnung(zellen[str(sa.day)])
            x, y = cx + cb // 2 + VERSATZ, cy + ch - 12 + VERSATZ
            fx = ausdehnung(zellen[str(fr.day)])[0] + cb // 2 + VERSATZ
        ab = marke()
        klick(x, y)
        time.sleep(1.5)
        (ok if not warte(r'Kalender: Editor neu', ab, 1) and dialog(1) is None else falsch)(
            '%s: Einfachklick auf leere Fläche öffnet keinen Editor' % ansicht)
        ab = marke()
        doppelklick(x, y)
        f = editor_felder()
        (ok if warte(r'Kalender: Editor neu', ab, 5) and f['Anfangsdatum'] == sa.strftime('%d.%m.%Y') else falsch)(
            '%s: Doppelklick öffnet „Neuer Termin“ am %s (Anfangsdatum %s)' % (ansicht, sa, f['Anfangsdatum']))
        bild('doppelklick-%s' % ansicht.lower())
        editor_zu(ab, ansicht + ' Doppelklick')
        if ansicht == 'Kopfleiste':
            continue
        # Ziehen über einen Bereich (Vorlage): Raster Sa 07:30 bis 08:45 (Ende der Zelle 09:00), Monat Sa → Fr.
        ab = marke()
        ziehen((x, y), (x, zeile(525) + 10) if ansicht == 'Raster' else (fx, y))
        f = editor_felder()
        if ansicht == 'Raster':
            soll = {'Anfangsdatum': sa.strftime('%d.%m.%Y'), 'Anfangszeit': '07:30', 'Endzeit': '09:00'}
        else:
            soll = {'Anfangsdatum': fr.strftime('%d.%m.%Y'), 'Enddatum': sa.strftime('%d.%m.%Y')}
        (ok if warte(r'Kalender: Editor neu', ab, 5) and all(f.get(k) == w for k, w in soll.items()) else falsch)(
            '%s: Ziehen über einen Bereich öffnet „Neuer Termin“ %s (soll %s)' % (
                ansicht, {k: f.get(k) for k in soll}, soll))
        bild('bereich-%s' % ansicht.lower())
        editor_zu(ab, ansicht + ' Bereich')
    taste('Alt_L', '2')
    time.sleep(1)


def monat_ziehen(kal, uid, titel, montag):
    """Runde 2 (PM): Termin in der Monatsansicht auf einen anderen Tag ziehen (gcal-month-cell.c move_event)."""
    so = montag + datetime.timedelta(days=6)
    taste('Alt_L', '1')
    time.sleep(2)
    tab = finde(wurzel(), 'table', 'Monat')
    w = next((b for b in alle(tab, 'toggle button') if b.get_name() == titel), None)
    zelle = None
    for c in alle(tab, 'table cell'):
        if c.get_name() == str(so.day):
            zelle = c
    if w is None or zelle is None:
        falsch('Ziehen im Monat: Termin oder Zelle nicht gefunden')
        taste('Alt_L', '2')
        return
    cx, cy, cb, ch = ausdehnung(zelle)
    ab = marke()
    app_schreibt(kal, 'verschieben Monat')
    ziehen(mitte(w), (cx + cb // 2 + VERSATZ, cy + ch - 12 + VERSATZ))
    m = warte(r'Kalender: ändern %s (fertig|Fehler)' % re.escape(kal), ab, 60)
    (ok if m and m.group(1) == 'fertig' else falsch)('Ziehen im Monat nach So: %s' % (m and m.group(0)))
    xtest.fake_input(d, X.MotionNotify, x=150, y=310)  # Zeiger weg: kein Tooltip im Bild
    d.sync()
    time.sleep(1)
    bild('monat-gezogen')
    gegenprobe(kal, uid, {'beginn': so.isoformat(), 'ende': (so + datetime.timedelta(days=1)).isoformat()}, 'Ziehen Monat')
    taste('Alt_L', '2')
    time.sleep(1)


def termine():
    soll_kal = vergleich()
    G, M, L = SCHREIBBAR[0], SCHREIBBAR[1], SCHREIBBAR[-1]
    name_von = {u: v[0].split(' / ', 1)[1] for u, v in soll_kal.items()}
    stamp = datetime.datetime.now().strftime('%H%M%S')
    heute = datetime.date.today()
    montag = heute - datetime.timedelta(days=heute.weekday()) + datetime.timedelta(days=14)
    eigene = set()

    # 1. Kalenderwahl nur schreibbare Kalender (EDS readonly), Escape schließt ohne Schreiben.
    ab = marke()
    taste('Control_L', 'n')
    dlg = dialog()
    (ok if dlg is not None and warte(r'Kalender: Editor neu ', ab, 5) else falsch)('Strg+N: Editor „Neuer Termin“')
    c = finde(dlg, 'combo box', 'Kalender')
    klick(*mitte(c))
    time.sleep(1)
    liste = sorted(kombi_eintraege(c))
    soll = sorted(name_von[u] for u, v in soll_kal.items() if not v[2])
    (ok if liste == soll else falsch)('Kalenderwahl = %d schreibbare Kalender (ohne %s)%s' % (
        len(liste), sorted(name_von[u] for u, v in soll_kal.items() if v[2]), '' if liste == soll else ': %s' % liste))
    bild('kalenderwahl')
    taste('Escape')
    taste('Escape')
    time.sleep(1)
    (ok if dialog(1) is None and not warte(r'Kalender: anlegen ', ab, 1) else falsch)('Escape schließt den Editor ohne Schreiben')

    # 1b. Runde 2 (PM): Einfachklick auf leere Fläche legt nicht an, Doppelklick öffnet den Editor (Raster, Monat).
    einfach_doppel(heute, montag)

    # 2. Erinnerung: Termin heute im lokalen Kalender, Beginn in 7 min, Erinnerung 5 min vorher (≈ 2 min).
    #    ERINNERUNG=0 (Umgebung) lässt Erinnerung und Schließen-Test aus (Entwicklung).
    mit_erinnerung = os.environ.get('ERINNERUNG', '1') != '0'
    mon, mon_f, mon_pfad = notify_mitschnitt() if mit_erinnerung else (None, None, None)
    jetzt = datetime.datetime.now() + datetime.timedelta(minutes=7)
    titel_e = 'Revolution-Test 375 erinnerung %s' % stamp
    ab = marke()
    m = None
    if mit_erinnerung:
        taste('Control_L', 'n')
        dlg = dialog()
        neuer_termin(dlg, name_von[L], titel_e, 'Testort', 'Erinnerungstest', False, jetzt.date(), jetzt.strftime('%H:%M'),
                     (jetzt + datetime.timedelta(minutes=30)).strftime('%H:%M'), '5 Minuten')
        m = speichern_und_warten(L, 'anlegen', ab)
    erinnerung_um = jetzt - datetime.timedelta(minutes=5)
    if m and m.group(1) == 'fertig':
        eigene.add(m.group(2))
        uid_e = m.group(2)
        eigene_datei(L, 'erinnerung', m.group(2), titel_e)
        ok('Erinnerung: Testtermin lokal angelegt (Beginn %s, Erinnerung %s)' % (jetzt.strftime('%H:%M'),
                                                                                 erinnerung_um.strftime('%H:%M')))
        gegenprobe(L, m.group(2), {'titel': titel_e, 'erinnerung': '5', 'ganztags': '0'}, 'Erinnerung')
    elif mit_erinnerung:
        falsch('Erinnerung: anlegen %s' % (m and m.group(0)))

    # 3. Anlegen: lokal ganztägig (Tastatur), Gmail wöchentlich 3× (Maus), outlook.com Zeitfenster (Tastatur).
    zur_woche(heute, 2)
    faelle = (
        # Runde 3: lokal ohne Erinnerung (dieser Termin wechselt später nach outlook.com; kein Termin mit Erinnerung
        # gelangt per Wechsel an einen Server).
        (L, 'lokal', True, montag, None, None, None, False, True, {'ganztags': '1', 'beginn': montag.isoformat(),
         'ende': (montag + datetime.timedelta(days=1)).isoformat(), 'erinnerung': '-', 'rrule': '-'}),
        (G, 'gmail', False, montag + datetime.timedelta(days=1), '10:00', '11:00', '10 Minuten', True, False,
         {'ganztags': '0', 'beginn': zeit_utc(montag + datetime.timedelta(days=1), 10, 0),
          'ende': zeit_utc(montag + datetime.timedelta(days=1), 11, 0), 'erinnerung': '10', 'rrule': 'FREQ=WEEKLY;COUNT=3'}),
        (M, 'outlook', False, montag + datetime.timedelta(days=2), '09:00', '10:00', '30 Minuten', False, True,
         {'ganztags': '0', 'beginn': zeit_utc(montag + datetime.timedelta(days=2), 9, 0),
          'ende': zeit_utc(montag + datetime.timedelta(days=2), 10, 0), 'erinnerung': '30', 'rrule': '-'}),
    )
    titel = {}
    for kal, art, ganz, tag, von, bis, erin, woe, tast, soll in faelle:
        t = 'Revolution-Test 375 %s %s' % (art, stamp)
        titel[kal] = t
        ab = marke()
        if tast:
            taste('Control_L', 'n')
        else:
            klick(*mitte(finde(wurzel(), 'button', 'Termin hinzufügen')))
        dlg = dialog()
        try:
            neuer_termin(dlg, name_von[kal], t, 'Ort %s' % art, 'Notiz %s' % art, ganz, tag, von, bis, erin, woe, tast)
        except RuntimeError as e:
            falsch('Anlegen %s: %s' % (art, e))
            bild('fehler-%s' % art)
            for _ in range(3):
                if dialog(1) is None:
                    break
                taste('Escape')
            continue
        bild_beide('editor-neu-%s' % art) if art == 'outlook' else bild('editor-neu-%s' % art)
        if tast:
            m = speichern_und_warten(kal, 'anlegen', ab)
        else:
            app_schreibt(kal, 'anlegen')
            druecke(finde(dlg, 'button', 'Speichern'))
            m = warte(r'Kalender: anlegen %s (fertig|Fehler)(?: (\S+))?$' % re.escape(kal), ab, 60)
        if not m or m.group(1) != 'fertig':
            falsch('Anlegen %s: %s' % (art, m and m.group(0)))
            bild('fehler-%s' % art)
            for _ in range(3):
                if dialog(1) is None:
                    break
                taste('Escape')
            continue
        eigene.add(m.group(2))
        eigene_datei(kal, 'editor', m.group(2), t)
        ok('Anlegen %s per %s: %s' % (art, 'Tastatur' if tast else 'Maus', m.group(2)[:24]))
        soll = dict(soll, titel=t, ort='Ort %s' % art, notiz='Notiz %s' % art)
        gegenprobe(kal, m.group(2), soll, 'Anlegen')
        mt = warte(r'Termin: \+ %s %s:%s' % (re.escape(kal), re.escape(kal), re.escape(m.group(2))), ab, 30)
        (ok if mt else falsch)('Anlegen %s: Termin in der Ansicht (Log, ECalClientView)' % art)
    bild('woche-neu')
    uid = {kal: open(datei(kal, 'editor')).read().split()[2] for kal in (L, G, M) if os.path.exists(datei(kal, 'editor'))}

    # 4. Ändern: Zeit und Titel (outlook.com), Wiederholung „nur dieser“ und „alle“ (Gmail).
    if M in uid:
        dlg, _ = bearbeiten(titel[M], eigene, 'popover')
        time.sleep(0.5)
        bild_beide('editor-bearbeiten')
        neu = titel[M] + ' geaendert'
        feld_setzen(dlg, 'Titel', neu)
        feld_setzen(dlg, 'Endzeit', '12:00')
        feld_setzen(dlg, 'Anfangszeit', '11:00')
        ab = marke()
        m = speichern_und_warten(M, 'ändern', ab)
        (ok if m and m.group(1) == 'fertig' else falsch)('Ändern outlook.com (Titel, Zeit): %s' % (m and m.group(0)))
        tag = montag + datetime.timedelta(days=2)
        gegenprobe(M, uid[M], {'titel': neu, 'beginn': zeit_utc(tag, 11, 0), 'ende': zeit_utc(tag, 12, 0)}, 'Ändern')
        titel[M] = neu
    if G in uid:
        di1 = montag + datetime.timedelta(days=1)
        di2 = di1 + datetime.timedelta(days=7)
        taste('Alt_L', 'Right')
        dlg, _ = bearbeiten(titel[G], eigene)
        feld_setzen(dlg, 'Titel', titel[G] + ' nur dieser')
        ab = marke()
        app_schreibt(G, 'ändern nur dieser')
        taste('Control_L', 'Return')
        frage = warte_obj(lambda: finde(wurzel(), 'button', 'Nur diesen Termin'), 10)
        bild_beide('wiederholung-frage')
        (ok if frage else falsch)('Wiederholung: Abfrage „Nur diesen Termin / Nachfolgende / Alle“ (AdwAlertDialog)')
        if frage:
            druecke(frage)
        m = warte(r'Kalender: ändern %s (fertig|Fehler)' % re.escape(G), ab, 60)
        mm = warte(r'Kalender: ändern %s \S+ mod (\d+) ' % re.escape(G), ab, 1)
        (ok if m and m.group(1) == 'fertig' and mm and mm.group(1) == '1' else falsch)(
            'Wiederholung „nur dieser“: %s, mod %s (E_CAL_OBJ_MOD_THIS = 1)' % (m and m.group(1), mm and mm.group(1)))
        gegenprobe(G, uid[G], {'titel': titel[G], 'rrule': 'FREQ=WEEKLY;COUNT=3'}, 'Nur dieser',
                   ausnahme_soll=[(zeit_utc(di2, 10, 0), titel[G] + ' nur dieser')])
        taste('Alt_L', 'Left')
        dlg, _ = bearbeiten(titel[G], eigene)
        feld_setzen(dlg, 'Titel', titel[G] + ' alle')
        ab = marke()
        app_schreibt(G, 'ändern alle')
        taste('Control_L', 'Return')
        frage = warte_obj(lambda: finde(wurzel(), 'button', 'Alle Termine'), 10)
        if frage:
            druecke(frage)
        m = warte(r'Kalender: ändern %s (fertig|Fehler)' % re.escape(G), ab, 60)
        mm = warte(r'Kalender: ändern %s \S+ mod (\d+) ' % re.escape(G), ab, 1)
        (ok if m and m.group(1) == 'fertig' and mm and mm.group(1) == '7' else falsch)(
            'Wiederholung „alle“: %s, mod %s (E_CAL_OBJ_MOD_ALL = 7, Hauptkomponente im Worker)' % (
                m and m.group(1), mm and mm.group(1)))
        titel[G] += ' alle'
        gegenprobe(G, uid[G], {'titel': titel[G], 'rrule': 'FREQ=WEEKLY;COUNT=3'}, 'Alle')
        # Runde 2: „Nachfolgende Termine“ (E_CAL_OBJ_MOD_THIS_AND_FUTURE = 4) am dritten Vorkommen.
        di3 = di1 + datetime.timedelta(days=14)
        for _ in range(2):
            taste('Alt_L', 'Right')
        dlg, _ = bearbeiten(titel[G], eigene)
        feld_setzen(dlg, 'Titel', titel[G] + ' folgende')
        ab = marke()
        app_schreibt(G, 'ändern nachfolgende (alte Serie kürzen)')
        app_schreibt(G, 'ändern nachfolgende (neue Serie anlegen, e-cal-meta-backend.c:2033/2052)')
        taste('Control_L', 'Return')
        frage = warte_obj(lambda: finde(wurzel(), 'button', 'Nachfolgende Termine'), 10)
        if frage:
            druecke(frage)
        m = warte(r'Kalender: ändern %s (fertig|Fehler)' % re.escape(G), ab, 60)
        mm = warte(r'Kalender: ändern %s \S+ mod (\d+) ' % re.escape(G), ab, 1)
        (ok if frage and m and m.group(1) == 'fertig' and mm and mm.group(1) == '4' else falsch)(
            'Wiederholung „Nachfolgende Termine“: %s, mod %s (E_CAL_OBJ_MOD_THIS_AND_FUTURE = 4)' % (
                m and m.group(1), mm and mm.group(1)))
        # EDS teilt die Serie (e-cal-meta-backend.c:2041-2053): alte Serie endet vor dem Vorkommen, ab dort neue Serie mit
        # neuer UID. Deren UID aus dem Log der App (Termin: + …, von dieser Änderung), gespeichert fürs Aufräumen.
        mt = warte(r'Termin: \+ %s %s:(\S+):\S+ %s ' % (re.escape(G), re.escape(G), zeit_utc(di3, 10, 0)), ab, 60)
        neu = mt.group(1) if mt and mt.group(1) != uid[G] else None
        (ok if neu else falsch)('Nachfolgende: neue Serie ab %s in der Ansicht (UID %s)' % (di3, neu and neu[:16]))
        if neu:
            eigene.add(neu)
            eigene_datei(G, 'geteilt', neu, titel[G] + ' folgende')
            gegenprobe(G, neu, {'titel': titel[G] + ' folgende', 'beginn': zeit_utc(di3, 10, 0),
                                'ende': zeit_utc(di3, 11, 0)}, 'Nachfolgende (neue Serie)')
        gi = testdaten('--termin-details', G, uid[G])
        vork = sorted(re.findall(r'^GI vorkommen (\S+) titel', gi, re.M))
        (ok if vork == [zeit_utc(di1, 10, 0), zeit_utc(di1 + datetime.timedelta(days=7), 10, 0)] else falsch)(
            'Nachfolgende: alte Serie endet vor dem dritten Vorkommen (Python-GI %s)' % vork)
        srv = testdaten('--termin-server', G, uid[G])
        regel = re.findall(r'^SERVER rrule (.*)$', srv, re.M)
        # Genau zwei Vorkommen: COUNT=2 oder UNTIL vor dem dritten Vorkommen (e_cal_recur_ensure_end_dates).
        until = re.match(r'^FREQ=WEEKLY;UNTIL=(\d{8})', regel[0]) if regel else None
        (ok if regel == ['FREQ=WEEKLY;COUNT=2'] or (until and until.group(1) < di3.strftime('%Y%m%d')) else falsch)(
            'Nachfolgende: alte Serie am Server (CalDAV) noch da, Regel %s' % re.findall(r'^SERVER rrule (.*)$', srv, re.M))
        for _ in range(2):
            taste('Alt_L', 'Left')

    # 5. Verschieben per Ziehen: outlook.com im Raster (Mi 11:00 → Do 08:00), lokal in der Kopfleiste (Mo → Fr).
    if M in uid:
        v, g = raster('Woche')
        w = termin_widget(titel[M])
        if g is not None and w is not None:
            gx, gy, gb, gh = ausdehnung(g)
            zx = tag_x(v, montag + datetime.timedelta(days=3))
            zy = int(gy + (8 * 60 + 10) * gh / 1440.0) + VERSATZ
            ab = marke()
            app_schreibt(M, 'verschieben')
            ziehen(mitte(w), (zx, zy))
            m = warte(r'Kalender: ändern %s (fertig|Fehler)' % re.escape(M), ab, 60)
            (ok if m and m.group(1) == 'fertig' else falsch)('Ziehen outlook.com im Raster nach Do 08:00: %s' % (m and m.group(0)))
            do = montag + datetime.timedelta(days=3)
            gegenprobe(M, uid[M], {'beginn': zeit_utc(do, 8, 0), 'ende': zeit_utc(do, 9, 0)}, 'Ziehen')
        else:
            falsch('Ziehen: Raster oder Termin nicht gefunden')
    if L in uid:
        v, g = raster('Woche')
        w = termin_widget(titel[L])
        if g is not None and w is not None:
            gx, gy, gb, gh = ausdehnung(g)
            wx, wy = mitte(w)
            ab = marke()
            app_schreibt(L, 'verschieben')
            ziehen((wx, wy), (tag_x(v, montag + datetime.timedelta(days=4)), wy))
            m = warte(r'Kalender: ändern %s (fertig|Fehler)' % re.escape(L), ab, 60)
            (ok if m and m.group(1) == 'fertig' else falsch)('Ziehen lokal (ganztägig) in der Kopfleiste nach Fr: %s' % (
                m and m.group(0)))
            fr = montag + datetime.timedelta(days=4)
            gegenprobe(L, uid[L], {'beginn': fr.isoformat(), 'ende': (fr + datetime.timedelta(days=1)).isoformat()}, 'Ziehen')
    bild('woche-gezogen')
    if L in uid:
        monat_ziehen(L, uid[L], titel[L], montag)

    # 5b. Runde 3: Kalender im Editor wechseln outlook.com → lokal (gcal_manager_move_event_to_source im Worker). Ziel lokal:
    #     kein Termin mit Erinnerung gelangt per Wechsel an einen Server (outlook.com setzt bei neuen Terminen ohne VALARM
    #     selbst 15 min, gemessen Runde 3, m7-375-r3a).
    wechsel = None
    if M in uid:
        zur_woche(heute, 2)
        dlg, _ = bearbeiten(titel[M], eigene)
        kombi_waehlen(dlg, 'Kalender', name_von[L])
        if kombi_auswahl(dlg, 'Kalender') == name_von[L]:
            ab = marke()
            app_schreibt(L, 'verschieben anlegen')
            app_schreibt(M, 'verschieben löschen')
            taste('Control_L', 'Return')
            m = warte(r'Kalender: verschieben (fertig (\S+)|Fehler)$', ab, 60)
            v = warte(r'Kalender: verschieben %s \S+ nach %s$' % (re.escape(M), re.escape(L)), ab, 1)
            (ok if m and m.group(2) and v else falsch)('Kalender wechseln outlook.com → lokal: %s' % (m and m.group(0)))
            if m and m.group(2):
                wechsel = m.group(2)
                eigene.add(wechsel)
                eigene_datei(L, 'wechsel', wechsel, titel[M])
                do = montag + datetime.timedelta(days=3)
                gegenprobe(L, wechsel, {'titel': titel[M], 'beginn': zeit_utc(do, 8, 0), 'ende': zeit_utc(do, 9, 0),
                                        'erinnerung': '30'}, 'Kalender wechseln')
                aus = testdaten('--termin-live', 'pruefen', M, datei(M, 'editor'))
                srv = testdaten('--termin-server', M, uid[M])
                (ok if 'VORHANDEN 0' in aus and 'HTTP 404' in srv else falsch)(
                    'Kalender wechseln: in outlook.com weg (Python-GI fehlt, Graph 404)')
                os.remove(datei(M, 'editor'))
        else:
            falsch('Kalender wechseln: Kalenderwahl nicht „%s“, nicht gespeichert' % name_von[L])
            for _ in range(3):
                if dialog(1) is None:
                    break
                taste('Escape')

    # 6. Nur lesend: Termin eines EDS-schreibgeschützten Kalenders (Feiertage Gmail und Arnds Konto): Popover ohne Bearbeiten,
    # Entf ohne Wirkung, Editor zeigt Details ohne Löschen.
    lesend = [u for u, v in soll_kal.items() if v[2]]
    zur_woche(heute)
    nur_lesend(lesend)

    # 7. Banner: Schreiben gegen einen inzwischen gelöschten Termin (lokal, von außen gelöscht, Editor noch offen).
    bk, bdatei = L, datei(L, 'editor')
    if L in uid:
        zur_woche(heute, 2)
        dlg, _ = bearbeiten(titel[L], eigene)
        testdaten('--termin-live', 'loeschen', bk, bdatei)
        aus = testdaten('--termin-live', 'pruefen', bk, bdatei)
        (ok if 'VORHANDEN 0' in aus else falsch)('Banner: Testtermin von außen gelöscht (Python-GI), Editor noch offen')
        feld_setzen(dlg, 'Titel', titel[L] + ' banner')
        ab = marke()
        m = speichern_und_warten(bk, 'ändern', ab)
        b = warte(r'Kalender: Banner: (.*?): (.*)$', ab, 10)
        (ok if m and m.group(1) == 'Fehler' and b else falsch)('Banner bei Schreibfehler: %s' % (b and b.group(0)))
        time.sleep(1)
        banner = finde(wurzel(), None, b.group(1)) if b else None
        (ok if banner is not None else falsch)('Banner sichtbar (AT-SPI „%s“)' % (b and b.group(1)))
        xtest.fake_input(d, X.MotionNotify, x=150, y=310)  # Zeiger auf die Seitenleiste: kein fremder Tooltip im Bild
        d.sync()
        time.sleep(1)
        bild_beide('banner')
        # „Schließen“ nur innerhalb der Fläche des Banners (die Fensterknöpfe heißen ebenso).
        bx, by, bb, bh = ausdehnung(banner) if banner else (0, 0, 0, 0)
        zu = next((k for k in alle(wurzel(), 'button') if k.get_name() == 'Schließen' and
                   by - 20 <= ausdehnung(k)[1] and ausdehnung(k)[1] + ausdehnung(k)[3] <= by + bh + 20), None)
        ab = marke()
        y_vorher = ausdehnung(finde(wurzel(), 'panel', 'Woche'))[1]
        if zu is not None:
            klick(*mitte(zu))
        time.sleep(1.5)
        y_nachher = ausdehnung(finde(wurzel(), 'panel', 'Woche'))[1]
        # Ausgeblendet: Log und die Woche rückt um die Höhe des Banners nach oben (AT-SPI).
        (ok if zu is not None and warte(r'Kalender: Banner ausgeblendet', ab, 3) and y_nachher < y_vorher else falsch)(
            'Banner: „Schließen“ blendet es aus (Woche y %d → %d)' % (y_vorher, y_nachher))

    # 8. Löschen Gmail (Wiederholung, „alle“): Entf im Popover, Rückgängig ohne Schreiben; dann löschen, nach Ablauf weg.
    if G in uid:
        zur_woche(heute, 2)
        for runde in ('rueckgaengig', 'loeschen'):
            popover_oeffnen(titel[G], eigene)
            ab = marke()
            taste('Delete')
            frage = warte_obj(lambda: finde(wurzel(), 'button', 'Alle Termine'), 10)
            if frage is None:
                falsch('Löschen: keine Abfrage bei Wiederholung')
                break
            if runde == 'loeschen':
                app_schreibt(G, 'löschen')
            druecke(frage)
            m = warte(r'Kalender: Löschen vorgemerkt %s \S+ mod (\d+)' % re.escape(G), ab, 10)
            (ok if m and m.group(1) == '7' else falsch)('Entf: Löschen vorgemerkt (alle), Toast (%s)' % (m and m.group(0)))
            time.sleep(0.5)
            (ok if termin_widget(titel[G], 0.3) is None else falsch)('Löschen: Termin sofort ausgeblendet')
            if runde == 'rueckgaengig':
                bild('toast-geloescht-hell')
            else:
                thema('dunkel')
                bild('toast-geloescht-dunkel')
                thema('hell')
            if runde == 'rueckgaengig':
                r = finde(wurzel(), 'button', 'Rückgängig')
                (ok if r else falsch)('Toast „Termin gelöscht“ mit „Rückgängig“')
                if r:
                    klick(*mitte(r))
                mr = warte(r'Kalender: Löschen rückgängig ', ab, 5)
                time.sleep(7)
                (ok if mr and not warte(r'Kalender: löschen ', ab, 1) else falsch)('Rückgängig: kein Schreibvorgang (Log)')
                aus = testdaten('--termin-live', 'pruefen', G, datei(G, 'editor'))
                (ok if 'VORHANDEN 1' in aus and termin_widget(titel[G], 10) else falsch)(
                    'Rückgängig: Termin weiter da (Python-GI) und wieder sichtbar')
            else:
                m = warte(r'(\d\d):(\d\d):(\d\d\.\d+): Kalender: löschen %s (fertig|Fehler)' % re.escape(G), ab, 30)
                v = warte(r'(\d\d):(\d\d):(\d\d\.\d+): Kalender: Löschen vorgemerkt %s ' % re.escape(G), ab, 1)
                sek = lambda x: int(x.group(1)) * 3600 + int(x.group(2)) * 60 + float(x.group(3))
                dauer = sek(m) - sek(v) if m and v else -1
                (ok if m and m.group(4) == 'fertig' and 4.5 <= dauer <= 7 else falsch)(
                    'Löschen nach Ablauf: geschrieben %.1f s nach dem Vormerken (Toast 5 s)' % dauer)
                aus = testdaten('--termin-live', 'pruefen', G, datei(G, 'editor'))
                srv = testdaten('--termin-server', G, uid[G])
                (ok if 'VORHANDEN 0' in aus and 'SERVER-FEHLT' in srv else falsch)('Gelöscht: Python-GI fehlt, Server fehlt')

    # 9. Löschen über den Editor („Termin löschen“), nach Ablauf weg (der gewechselte Termin, jetzt lokal).
    if wechsel:
        zur_woche(heute, 2)
        dlg, _ = bearbeiten(titel[M], eigene)
        ab = marke()
        app_schreibt(L, 'löschen')
        knopf = finde(dlg, 'button', 'Termin löschen')
        zeigen(dlg, knopf)
        klick(*mitte(knopf))  # Adw.ButtonRow::activated (Klick)
        m = warte(r'Kalender: löschen %s (fertig|Fehler)' % re.escape(L), ab, 30)
        (ok if m and m.group(1) == 'fertig' else falsch)('Löschen über den Editor, nach Ablauf: %s' % (m and m.group(0)))
        aus = testdaten('--termin-live', 'pruefen', L, datei(L, 'wechsel'))
        (ok if 'VORHANDEN 0' in aus else falsch)('Gelöscht: Python-GI fehlt')

    # 10. Erinnerung abwarten (Mitschnitt), dann Erinnerungstermin per Entf löschen und Fenster sofort schließen.
    if not mit_erinnerung:
        return
    rest = (erinnerung_um - datetime.datetime.now()).total_seconds() + 90
    if rest > 0:
        time.sleep(rest)
    mon.terminate()
    mon.wait()
    mon_f.close()
    mit = open(mon_pfad, encoding='utf-8', errors='replace').read()
    treffer = len([b for b in re.split(r'\n(?=method call |signal |method return |error )', mit)
                   if titel_e in b and re.search(r'member=(Notify|AddNotification)', b)])
    (ok if treffer == 1 else falsch)('Erinnerung: Session-Bus zeigt Benachrichtigung mit „%s“ (%d×, %s)' % (
        titel_e, treffer, ', '.join(sorted(set(re.findall(r'member=(Notify|AddNotification)', mit))))))
    if os.path.exists(datei(L, 'erinnerung')):
        zur_woche(heute)
        ziel = L
        popover_oeffnen(titel_e, eigene)
        ab = marke()
        app_schreibt(ziel, 'löschen beim Schließen')
        taste('Delete')
        m = warte(r'Kalender: Löschen vorgemerkt %s ' % re.escape(ziel), ab, 10)
        (ok if m else falsch)('Entf ohne Wiederholung: sofort vorgemerkt (ohne Abfrage)')
        w = fenster()
        rc = schliessen(w) if w else None
        m = warte(r'Kalender: löschen %s (fertig|Fehler)' % re.escape(ziel), ab, 10)
        (ok if rc == 0 and m and m.group(1) == 'fertig' and warte(r'Fenster geschlossen, vorgemerktes Löschen jetzt', ab, 1)
         else falsch)('Fenster schließen löscht sofort: Exit %s, %s' % (rc, m and m.group(0)))
        aus = testdaten('--termin-live', 'pruefen', ziel, datei(ziel, 'erinnerung'))
        (ok if 'VORHANDEN 0' in aus else falsch)('Nach dem Schließen: Erinnerungstermin gelöscht (Python-GI)')


def gesperrt(dlg, soll, text):
    """#452: Kalenderwahl gesperrt (soll True) bzw. wählbar (AT-SPI SENSITIVE)."""
    c = finde(dlg, 'combo box', 'Kalender')
    ist = c is not None and not c.get_state_set().contains(Atspi.StateType.SENSITIVE)
    (ok if ist == soll else falsch)('Kalenderwahl %s: %s' % (text, 'gesperrt' if ist else 'wählbar'))


def editor_zu():
    for _ in range(4):
        if dialog(1) is None:
            return
        taste('Escape')
        time.sleep(0.5)


def serie():
    """#452 (Kopf von tests/kalender.sh, Teil serie)."""
    vergleich()
    G, L = SCHREIBBAR[0], SCHREIBBAR[-1]
    heute = datetime.date.today()
    montag = heute - datetime.timedelta(days=heute.weekday()) + datetime.timedelta(days=14)
    di = [montag + datetime.timedelta(days=1 + 7 * i) for i in range(5)]

    # Gegenkontrolle S1: neuer Termin wählbar.
    taste('Control_L', 'n')
    gesperrt(dialog(), False, 'neuer Termin')
    editor_zu()

    aus = testdaten('--termin-live', 'woechentlich', G, datei(G, 'serie'), '%sT10:00' % di[0].isoformat())
    m = re.search(r'^ANGELEGT (\S+) \S+ (.*)$', aus, re.M)
    uid, titel = m.group(1), m.group(2)
    eigene = {uid}
    regel = 'FREQ=WEEKLY;COUNT=5'

    def ausnahme(i):
        """Vorkommen i (Woche 2 + i) ändern, „Nur diesen Termin“; Titel der Ausnahme zurück."""
        zur_woche(heute, 2 + i)
        dlg, _ = bearbeiten(titel, eigene)
        gesperrt(dlg, True, 'Vorkommen einer Serie')
        neu = '%s ausnahme %d' % (titel, i)
        feld_setzen(dlg, 'Titel', neu)
        ab = marke()
        app_schreibt(G, 'ändern nur dieser')
        taste('Control_L', 'Return')
        druecke(warte_obj(lambda: finde(wurzel(), 'button', 'Nur diesen Termin'), 10))
        e = warte(r'Kalender: ändern %s (fertig|Fehler)' % re.escape(G), ab, 60)
        (ok if e and e.group(1) == 'fertig' else falsch)('Ausnahme %d angelegt: %s' % (i, e and e.group(0)))
        gegenprobe(G, uid, {'titel': titel, 'rrule': regel}, 'Ausnahme %d' % i,
                   ausnahme_soll=[(zeit_utc(di[i], 10, 0), neu)])
        return neu

    def frage_loeschen(knopf_name, mod, ab):
        frage = warte_obj(lambda: finde(wurzel(), 'button', knopf_name), 10)
        (ok if frage else falsch)('Löschen der Ausnahme: Abfrage „%s“' % knopf_name)
        druecke(frage)
        v = warte(r'Kalender: Löschen vorgemerkt %s \S+ mod (\d+)' % re.escape(G), ab, 10)
        (ok if v and v.group(1) == str(mod) else falsch)('Löschen vorgemerkt mod %s (soll %d)' % (v and v.group(1), mod))
        e = warte(r'Kalender: löschen %s (fertig|Fehler)' % re.escape(G), ab, 30)
        r = warte(r'Kalender: löschen %s \S+ rid (\S+) mod %d$' % (re.escape(G), mod), ab, 1)
        return e, r

    def geloescht(i, text, weg):
        """Serie bleibt ohne Ausnahme, Vorkommen i fehlt (Python-GI), EXDATE am Server."""
        gegenprobe(G, uid, {'titel': titel, 'rrule': regel}, text, ausnahme_soll=[])
        vork = sorted(re.findall(r'^GI vorkommen (\S+) titel', testdaten('--termin-details', G, uid), re.M))
        soll = sorted(zeit_utc(di[k], 10, 0) for k in range(5) if k not in weg)
        (ok if vork == soll else falsch)('%s: %d Vorkommen, die gelöschten fehlen (Python-GI %s)' % (text, len(vork), vork))
        ex = re.findall(r'^SERVER exdate (\S+)$', testdaten('--termin-server', G, uid), re.M)
        (ok if len(ex) == len(weg) else falsch)('%s: Server EXDATE %s' % (text, ex))

    # M1 Entf im Popover.
    neu = ausnahme(1)
    zur_woche(heute, 3)
    dlg, _ = bearbeiten(neu, eigene)
    gesperrt(dlg, True, 'geänderte Ausnahme')
    editor_zu()
    time.sleep(1)
    popover_oeffnen(neu, eigene)
    ab = marke()
    app_schreibt(G, 'löschen Ausnahme (Entf)')
    taste('Delete')
    e, r = frage_loeschen('Nur diesen Termin', 1, ab)
    (ok if e and e.group(1) == 'fertig' and r and r.group(1) != '-' else falsch)(
        'Ausnahme per Entf gelöscht: %s, rid %s' % (e and e.group(0), r and r.group(1)))
    geloescht(1, 'Ausnahme per Entf gelöscht, Serie bleibt', [1])

    # M1 Löschen im Editor.
    neu = ausnahme(2)
    zur_woche(heute, 4)
    dlg, _ = bearbeiten(neu, eigene)
    ab = marke()
    app_schreibt(G, 'löschen Ausnahme (Editor)')
    knopf = finde(dlg, 'button', 'Termin löschen')
    zeigen(dlg, knopf)
    klick(*mitte(knopf))
    e, r = frage_loeschen('Nur diesen Termin', 1, ab)
    (ok if e and e.group(1) == 'fertig' and r and r.group(1) != '-' else falsch)(
        'Ausnahme im Editor gelöscht: %s, rid %s' % (e and e.group(0), r and r.group(1)))
    geloescht(2, 'Ausnahme im Editor gelöscht, Serie bleibt', [1, 2])

    # Von einer Ausnahme aus „Alle Termine“: ganze Serie (rid NULL, wie Evolution).
    neu = ausnahme(3)
    zur_woche(heute, 5)
    popover_oeffnen(neu, eigene)
    ab = marke()
    app_schreibt(G, 'löschen alle von der Ausnahme aus')
    taste('Delete')
    e, r = frage_loeschen('Alle Termine', 7, ab)
    aus = testdaten('--termin-live', 'pruefen', G, datei(G, 'serie'))
    srv = testdaten('--termin-server', G, uid)
    (ok if e and e.group(1) == 'fertig' and 'VORHANDEN 0' in aus and 'SERVER-FEHLT' in srv else falsch)(
        '„Alle Termine“ von der Ausnahme aus: Serie weg (Python-GI, Server), rid %s' % (r and r.group(1)))

    # M2 und S4: mehrtägige ganztägige Serie (lokal), nur Titel, „Alle Termine“, Fenster sofort schließen.
    aus = testdaten('--termin-live', 'ganztags-serie', L, datei(L, 'serie'), '%sT00:00' % montag.isoformat())
    m = re.search(r'^ANGELEGT (\S+) \S+ (.*)$', aus, re.M)
    uid_l, titel_l = m.group(1), m.group(2)
    soll_ende = (montag + datetime.timedelta(days=3)).isoformat()
    gegenprobe(L, uid_l, {'beginn': montag.isoformat(), 'ende': soll_ende, 'ganztags': '1'}, 'Ganztägige Serie')
    zur_woche(heute, 3)   # zweites Vorkommen: Dauer und Beginn der Serie bleiben
    dlg, _ = bearbeiten(titel_l, {uid_l})
    neu = titel_l + ' neu'
    feld_setzen(dlg, 'Titel', neu)
    ab = marke()
    app_schreibt(L, 'ändern alle')
    taste('Control_L', 'Return')
    frage = warte_obj(lambda: finde(wurzel(), 'button', 'Alle Termine'), 10)
    (ok if frage else falsch)('Ganztägige Serie: Abfrage „Alle Termine“')
    w = fenster()
    druecke(frage)
    rc = schliessen(w) if w else None
    e = warte(r'Kalender: ändern %s (fertig|Fehler)' % re.escape(L), ab, 10)
    (ok if rc == 0 and e and e.group(1) == 'fertig' else falsch)('Fenster sofort geschlossen: Exit %s, %s' % (rc, e and e.group(0)))
    gegenprobe(L, uid_l, {'titel': neu, 'beginn': montag.isoformat(), 'ende': soll_ende, 'ganztags': '1',
                          'rrule': 'FREQ=WEEKLY;COUNT=3'}, '„Alle Termine“ ganztägig: Dauer bleibt 3 Tage')


def tag_x(v, tag):
    """x-Mitte der Spalte eines Tags in der Woche (Tageszahl im Kopf, AT-SPI)."""
    lab = next((x for x in alle(v, 'label') if x.get_name() == str(tag.day)), None)
    return mitte(lab)[0] if lab else None


def ziehen(von, nach, schritte=12):
    xtest.fake_input(d, X.MotionNotify, x=von[0], y=von[1])
    d.sync()
    time.sleep(0.3)
    xtest.fake_input(d, X.ButtonPress, 1)
    d.sync()
    time.sleep(0.3)
    for i in range(1, schritte + 1):
        xtest.fake_input(d, X.MotionNotify, x=int(von[0] + (nach[0] - von[0]) * i / schritte),
                         y=int(von[1] + (nach[1] - von[1]) * i / schritte))
        d.sync()
        time.sleep(0.08)
    time.sleep(0.5)
    xtest.fake_input(d, X.ButtonRelease, 1)
    d.sync()
    time.sleep(1)


def nur_lesend(lesend):
    """Termin aus einem EDS-schreibgeschützten Kalender: Popover mit „Zusätzliche Details anzeigen“, Entf wirkungslos,
    Editor ohne „Termin löschen“ und ohne „Abbrechen“. Nur Termine, deren Kalender laut Log schreibgeschützt ist."""
    mehr = finde(finde(wurzel(), 'panel', 'Woche'), 'toggle button', 'Zusätzliche Termine anzeigen')
    if mehr is not None:
        klick(*mitte(mehr))  # Kopfleiste aufklappen (alle ganztägigen Termine, gcal-week-header.c)
        time.sleep(1)
    kandidaten = {}
    for m in re.finditer(r'Termin: \+ (\S+) (\S+) ', log_text()[log_start:]):
        if m.group(1) in lesend:
            kandidaten[m.group(2)] = m.group(1)
    for w in [b for b in alle(wurzel(), 'toggle button') if b.get_name()]:
        name = w.get_name()
        if name.startswith('Revolution-Test') or name.startswith('Zusätzliche') or name.startswith('+'):
            continue
        ab = marke()
        klick(*mitte(w))
        m = warte(r'Kalender: Popover (\S+)$', ab, 5)
        if m is None:
            continue
        if m.group(1) not in kandidaten:
            taste('Escape')
            time.sleep(0.5)
            continue
        time.sleep(0.8)
        info = finde(wurzel(), 'button', 'Zusätzliche Details anzeigen')
        bearb = finde(wurzel(), 'button', 'Zusätzliche Details bearbeiten')
        (ok if info and not bearb else falsch)('Nur lesend (%s „%s“): Popover nur „Details anzeigen“' % (
            kandidaten[m.group(1)][:8], name))
        bild('popover-nurlesend')
        ab = marke()
        taste('Delete')
        time.sleep(2)
        (ok if not warte(r'Kalender: Löschen vorgemerkt', ab, 1) else falsch)('Nur lesend: Entf löscht nicht')
        if not finde(wurzel(), 'button', 'Zusätzliche Details anzeigen'):
            klick(*mitte(w))
            time.sleep(1)
        ab = marke()
        taste('Return')
        dlg = dialog()
        (ok if dlg and not finde(dlg, 'button', 'Termin löschen') and not finde(dlg, 'button', 'Abbrechen') else falsch)(
            'Nur lesend: Editor „Termindetails“ ohne Löschen und Abbrechen')
        bild('editor-nurlesend')
        taste('Escape')
        time.sleep(1)
        return
    falsch('Nur lesend: kein Termin eines schreibgeschützten Kalenders in der Woche sichtbar')

def texte(o):
    """Namen aller Beschriftungen unter o (Titel/Untertitel der Teilnehmerzeilen)."""
    return [x.get_name() or '' for x in alle(o, 'label')]


def editor_fuer(name, uid):
    """Editor des Termins mit dieser UID; gleichnamige Termine anderer Kalender (die über die #376-Karte angenommene
    Kopie in „Persönlich“) werden übersprungen."""
    if termin_widget(name) is None:
        return None
    for i in range(6):
        knoepfe = sorted((b for b in alle(wurzel(), 'toggle button') if b.get_name() == name), key=mitte)
        if i >= len(knoepfe):
            return None
        ab = marke()
        klick(*mitte(knoepfe[i]))
        m = warte(r'Kalender: Popover (\S+)$', ab, 5)
        if m is None:
            # Klick fiel in das Schließen des vorigen Popovers (verify-377/r3: „Popover None“): einmal wiederholen.
            time.sleep(1.5)
            ab = marke()
            klick(*mitte(knoepfe[i]))
            m = warte(r'Kalender: Popover (\S+)$', ab, 5)
        wert('Termin %d/%d „%s“: Popover %s' % (i + 1, len(knoepfe), name[:30], m and m.group(1)[:60]))
        # #452: Vorkommen einer Serie: <Kalender>:<UID>:<RECURRENCE-ID>.
        if m and m.group(1).split(':')[1] == uid:
            time.sleep(0.8)
            ab = marke()
            taste('Return')
            if warte(r'Kalender: Editor bearbeiten (\S+) (\S+)$', ab, 10):
                return dialog()
            return None
        if m:
            taste('Escape')
        time.sleep(1.5)
    return None


def versand(e, ab, text):
    """Mail der App über den Postausgang: vor dem Beenden auf „gesendet“ warten (Beenden bricht den Sendelauf ab,
    main.c on_shutdown_network; die Mail bliebe bis zum nächsten Start im Postausgang)."""
    if e and 'Postausgang' in e.group(1):
        m = warte(r'Senden: \S+ (gesendet nach \d+ ms|fehlgeschlagen: .*)$', ab, 90)
        (ok if m and m.group(1).startswith('gesendet') else falsch)('%s: Postausgang %s' % (text, m and m.group(1)))


def einladung():
    """#377 (tests/einladung.sh TEILE=377): SCHRITT anlegen | aendern | loeschen eines Termins mit Teilnehmer im Kalender
    KAL (#449, TEILE=449: erinnerung | titel | entfernen). Teilnehmer per Alt+I, Adresse, Enter; Status je Zeile; Log „Einladung: REQUEST|CANCEL …“ (Server plant bzw.
    Postausgang); Gegenprobe Python-GI und Server (Beginn wie im Editor)."""
    schritt, kal, titel = os.environ['SCHRITT'], os.environ['KAL'], os.environ['TITEL']
    teilnehmer, art = os.environ['TEILNEHMER'], os.environ['ART']
    tag = datetime.date.fromisoformat(os.environ['TAG'])
    heute = datetime.date.today()
    wochen = ((tag - tag.weekday() * datetime.timedelta(days=1)) - (heute - datetime.timedelta(days=heute.weekday()))).days // 7
    uid_datei = os.path.join(TERMINE, 'eigene-%s.uid' % art)
    if schritt == 'anlegen':
        ab = marke()
        taste('Control_L', 'n')
        dlg = dialog()
        neuer_termin(dlg, os.environ['KAL_NAME'], titel, 'Testraum 377', 'Einladungstest 377', False, tag, '09:00', '09:30',
                     woechentlich=bool(os.environ.get('WOECHENTLICH')))
        (ok if not finde(dlg, 'combo box', 'Organisator') else falsch)('Ohne Teilnehmer keine Organisator-Wahl')
        # Feld per Maus (schließt das Zeit-Popover von neuer_termin), Alt+I danach aus dem Titel geprüft.
        feld = finde(dlg, 'text', 'Teilnehmer hinzufügen')
        zeigen(dlg, feld)
        klick(*mitte(feld))
        ab_t = marke()
        tippe(teilnehmer)
        taste('Return')
        m = warte(r'Kalender: Teilnehmer (\d+)$', ab_t, 3)
        if m is None:  # Enter übernahm zuerst den Vorschlag (#361), zweites Enter fügt hinzu
            taste('Return')
            m = warte(r'Kalender: Teilnehmer (\d+)$', ab_t, 3)
        (ok if m and m.group(1) == '1' else falsch)('Feld per Maus, Adresse, Enter: 1 Teilnehmer (%s)' % (m and m.group(0)))
        t = texte(dlg)
        (ok if any(teilnehmer in x for x in t) and 'Keine Antwort' in ' '.join(t) else falsch)(
            'Teilnehmerzeile mit Adresse und Status „Keine Antwort“')
        knopf = finde(dlg, 'button', '%s entfernen' % teilnehmer)
        (ok if knopf is not None and finde(dlg, 'button', None) is not None else falsch)(
            'Entfernen-Knopf mit Barrierefreiheits-Name „%s“' % (knopf and knopf.get_name()))
        if knopf is not None:
            druecke(knopf)  # Entfernen, danach wieder hinzufügen
            time.sleep(0.5)
            (ok if not any(teilnehmer in x for x in texte(dlg)) else falsch)('Entfernen: Zeile weg')
            ab_r = marke()
            klick(*mitte(feld))
            tippe(teilnehmer)
            taste('Return')
            if warte(r'Kalender: Teilnehmer 1$', ab_r, 3) is None:
                taste('Return')
            time.sleep(0.5)
            (ok if any(teilnehmer in x for x in texte(dlg)) else falsch)('Wieder hinzugefügt')
        if os.environ.get('TEILNEHMER2'):
            # #449: zweiter Teilnehmer (Plus-Adresse desselben Testkontos), damit nach dem Entfernen einer bleibt.
            ab_z = marke()
            klick(*mitte(feld))
            tippe(os.environ['TEILNEHMER2'])
            taste('Return')
            if warte(r'Kalender: Teilnehmer 2$', ab_z, 3) is None:
                taste('Return')
            m2 = warte(r'Kalender: Teilnehmer 2$', ab_z, 3)
            (ok if m2 else falsch)('Zweiter Teilnehmer hinzugefügt (Plus-Adresse)')
        titel_feld = finde(dlg, 'text', 'Titel')
        zeigen(dlg, titel_feld)
        klick(*mitte(titel_feld))
        taste('Alt_L', 'i')
        time.sleep(0.5)
        feld = finde(dlg, 'text', 'Teilnehmer hinzufügen')
        fokus = feld is not None and feld.get_state_set().contains(Atspi.StateType.FOCUSED)
        (ok if fokus else falsch)('Alt+I: Fokus im Feld „Teilnehmer hinzufügen“')
        if os.environ.get('ORGANISATOR'):
            kombi_waehlen(dlg, 'Organisator', os.environ['ORGANISATOR'])
            (ok if kombi_auswahl(dlg, 'Organisator') == os.environ['ORGANISATOR'] else falsch)(
                'Organisator-Wahl (Kalender ohne eigene Adresse): Testkonto gewählt')
        else:
            (ok if not finde(dlg, 'combo box', 'Organisator') else falsch)(
                'Keine Organisator-Wahl: Kalender hat eine eigene Adresse')
        zeigen(dlg, finde(dlg, 'text', 'Teilnehmer hinzufügen'))
        bild_beide('editor-teilnehmer-%s' % art)
        ab = marke()
        m = speichern_und_warten(kal, 'anlegen', ab)
        if not m or m.group(1) != 'fertig':
            falsch('Anlegen mit Teilnehmer: %s' % (m and m.group(0)))
            return
        uid = m.group(2)
        with open(uid_datei, 'w') as f:
            f.write(uid + '\n')
        ok('Anlegen mit Teilnehmer in %s: %s' % (kal[:8], uid[:24]))
        e = warte(r'Einladung: REQUEST (.*)$', ab, 60)
        (ok if e else falsch)('Einladung nach dem Anlegen: %s' % (e and e.group(1)))
        versand(e, ab, 'Einladung')
        gegenprobe(kal, uid, {'titel': titel, 'beginn': zeit_utc(tag, 9, 0), 'ende': zeit_utc(tag, 9, 30)}, 'Einladung')
        return
    uid = open(uid_datei).read().strip()
    zur_woche(heute, wochen)
    dlg = editor_fuer(titel, uid)
    if dlg is None:
        falsch('Editor für %s nicht geöffnet' % uid[:24])
        return
    if schritt == 'aendern':
        soll = os.environ.get('STATUS_SOLL')
        t = texte(dlg)
        if soll:
            (ok if any(teilnehmer in x and soll in x for x in t) or (soll in t and any(teilnehmer in x for x in t))
             else falsch)('Editor: Teilnehmer %s… mit Status „%s“ (%s)' % (teilnehmer[:4], soll,
                                                                           [x for x in t if soll in x or '@' in x]))
            zeigen(dlg, finde(dlg, 'text', 'Teilnehmer hinzufügen'))
            bild_beide('editor-status-%s' % art)
        feld_setzen(dlg, 'Endzeit', '10:30')
        feld_setzen(dlg, 'Anfangszeit', '10:00')
        ab = marke()
        m = speichern_und_warten(kal, 'ändern', ab)
        (ok if m and m.group(1) == 'fertig' else falsch)('Ändern (Beginn 10:00): %s' % (m and m.group(0)))
        e = warte(r'Einladung: REQUEST (.*)$', ab, 60)
        (ok if e else falsch)('Aktualisierung nach dem Ändern: %s' % (e and e.group(1)))
        versand(e, ab, 'Aktualisierung')
        gegenprobe(kal, uid, {'titel': titel, 'beginn': zeit_utc(tag, 10, 0), 'ende': zeit_utc(tag, 10, 30)},
                   'Aktualisierung')
        return
    if schritt in ('erinnerung', 'titel', 'entfernen'):
        # #449: nur Erinnerung → keine Mail, SEQUENCE gleich; Titel → genau 1 Aktualisierung; Teilnehmer entfernen →
        # CANCEL nur an ihn (bzw. „Server plant“), SEQUENCE + 1 (Python-GI, zweiter Prozess).
        vorher = felder(testdaten('--termin-details', kal, uid), 'GI')
        if schritt == 'erinnerung':
            erinnerung_waehlen(dlg, '10 Minuten')
        elif schritt == 'titel':
            feld_setzen(dlg, 'Titel', os.environ['TITEL_NEU'])
        else:
            knopf = finde(dlg, 'button', '%s entfernen' % os.environ.get('TEILNEHMER_WEG', teilnehmer))
            if knopf is None:
                falsch('Entfernen-Knopf für den Teilnehmer fehlt')
                return
            druecke(knopf)
            time.sleep(0.5)
        ab = marke()
        m = speichern_und_warten(kal, 'ändern', ab)
        (ok if m and m.group(1) == 'fertig' else falsch)('Ändern (%s): %s' % (schritt, m and m.group(0)))
        if schritt == 'entfernen':
            e = warte(r'Einladung: (CANCEL .*)$', ab, 60)
            (ok if e and e.group(1) == os.environ['CANCEL_SOLL'] else falsch)(
                'Absage an den entfernten Teilnehmer: %s (soll %s)' % (e and e.group(1), os.environ['CANCEL_SOLL']))
            versand(e, ab, 'Absage an den Entfernten')
        else:
            e = warte(r'Einladung: REQUEST (.*)$', ab, 30)
            if schritt == 'titel':
                (ok if e else falsch)('Aktualisierung nach dem Titel: %s' % (e and e.group(1)))
                versand(e, ab, 'Aktualisierung')
        time.sleep(10)
        log = log_text()[ab:]
        n_req = len(re.findall(r'Einladung: REQUEST ', log))
        n_post = len(re.findall(r'Postausgang: \S+ abgelegt', log))
        if schritt == 'erinnerung':
            (ok if n_req == 0 and n_post == 0 and 'Einladung: keine Aktualisierung, nichts für Teilnehmer geändert' in log
             else falsch)('Nur Erinnerung: keine Aktualisierung, Postausgang unberührt (REQUEST %d, Postausgang %d)' % (
                 n_req, n_post))
        elif schritt == 'titel':
            (ok if n_req == 1 and n_post == 1 else falsch)('Titel: genau 1 Aktualisierung (REQUEST %d, Postausgang %d)' % (
                n_req, n_post))
        else:
            # Mit zweitem Teilnehmer: genau 1 Aktualisierung an den verbliebenen; Server plant: nichts im Postausgang.
            server = 'Server plant' in os.environ['CANCEL_SOLL']
            req_soll = 1 if os.environ.get('TEILNEHMER2') else 0
            post_soll = 0 if server else 1 + req_soll
            (ok if n_req == req_soll and n_post == post_soll else falsch)(
                'Entfernen: REQUEST %d (soll %d), Postausgang %d (soll %d)' % (n_req, req_soll, n_post, post_soll))
            if req_soll:
                r = re.search(r'Einladung: REQUEST (.*)$', log, re.M)
                (ok if r and r.group(1) == ('Server plant' if server else 'an 1 Teilnehmer über den Postausgang')
                 else falsch)('Aktualisierung an den verbliebenen Teilnehmer: %s' % (r and r.group(1)))
                if not server:
                    versand(r, ab, 'Aktualisierung an den Verbliebenen')
        soll = {'sequenz': vorher.get('sequenz') if schritt == 'erinnerung' else str(int(vorher.get('sequenz', '0')) + 1)}
        if os.environ.get('SEQUENZ') == '0':  # microsoft365: SEQUENCE führt Exchange
            del soll['sequenz']
        if schritt == 'erinnerung':
            soll['erinnerung'] = '10'
        elif schritt == 'titel':
            soll['titel'] = os.environ['TITEL_NEU']
        else:
            soll['teilnehmer'] = '1' if os.environ.get('TEILNEHMER2') else '0'
        gegenprobe(kal, uid, soll, '#449 %s (SEQUENCE vorher %s)' % (schritt, vorher.get('sequenz')), mit_server=False)
        return
    if schritt in ('folgende-loeschen', 'folgende-aendern'):
        # #452: Serie, „Nachfolgende Termine“: CANCEL bzw. REQUEST mit RECURRENCE-ID;RANGE=THISANDFUTURE (Beleg im
        # Gesendet-Ordner, tests/testdaten.py --einladung serie), Serie geteilt mit neuer UID (eigene-<ART>-neu.uid).
        aendern = schritt == 'folgende-aendern'
        ab = marke()
        if aendern:
            feld_setzen(dlg, 'Titel', os.environ['TITEL_NEU'])
            ab = marke()
            app_schreibt(kal, 'ändern nachfolgende (alte Serie kürzen)')
            app_schreibt(kal, 'ändern nachfolgende (neue Serie)')
            taste('Control_L', 'Return')
        else:
            app_schreibt(kal, 'löschen nachfolgende')
            knopf = finde(dlg, 'button', 'Termin löschen')
            zeigen(dlg, knopf)
            klick(*mitte(knopf))
        frage = warte_obj(lambda: finde(wurzel(), 'button', 'Nachfolgende Termine'), 10)
        (ok if frage else falsch)('Abfrage „Nachfolgende Termine“')
        druecke(frage)
        art_log = 'REQUEST' if aendern else 'CANCEL'
        e = warte(r'Einladung: %s (.*)$' % art_log, ab, 60)
        (ok if e else falsch)('%s: %s' % (art_log, e and e.group(1)))
        versand(e, ab, art_log)
        m = warte(r'Kalender: %s %s (fertig|Fehler)' % ('ändern' if aendern else 'löschen', re.escape(kal)), ab, 60)
        (ok if m and m.group(1) == 'fertig' else falsch)('Nachfolgende: %s' % (m and m.group(0)))
        if aendern:
            mt = warte(r'Termin: \+ %s %s:(\S+?)(?::\S+)? %s ' % (re.escape(kal), re.escape(kal), zeit_utc(tag, 9, 0)), ab, 30)
            (ok if mt and mt.group(1) != uid else falsch)('Serie geteilt: neue UID %s' % (mt and mt.group(1)[:24]))
            if mt:
                with open(os.path.join(TERMINE, 'eigene-%s-neu.uid' % art), 'w') as f:
                    f.write(mt.group(1) + '\n')
        return
    if schritt == 'rueckgaengig':
        # Löschen über den Editor, dann „Rückgängig“ im Toast: kein CANCEL, nichts im Postausgang, Termin bleibt.
        ab = marke()
        knopf = finde(dlg, 'button', 'Termin löschen')
        zeigen(dlg, knopf)
        klick(*mitte(knopf))
        r = warte_obj(lambda: finde(wurzel(), 'button', 'Rückgängig'), 4)
        (ok if r else falsch)('Toast „Termin gelöscht“ mit „Rückgängig“')
        if r:
            klick(*mitte(r))
        mr = warte(r'Kalender: Löschen rückgängig ', ab, 5)
        time.sleep(12)
        log = log_text()[ab:]
        (ok if mr and 'Einladung: CANCEL' not in log and 'Postausgang: ' not in log and 'Kalender: löschen ' not in log
         else falsch)('Rückgängig: kein CANCEL, Postausgang unberührt, kein Löschen (Log %s)' % (mr and mr.group(0)))
        aus = testdaten('--termin-details', kal, uid)
        (ok if re.search(r'^GI titel ', aus, re.M) else falsch)('Rückgängig: Termin weiter da (Python-GI)')
        return
    ab = marke()
    app_schreibt(kal, 'löschen')
    knopf = finde(dlg, 'button', 'Termin löschen')
    zeigen(dlg, knopf)
    klick(*mitte(knopf))
    e = warte(r'Einladung: CANCEL (.*)$', ab, 60)
    (ok if e else falsch)('Absage vor dem Löschen: %s' % (e and e.group(1)))
    versand(e, ab, 'Absage')
    m = warte(r'Kalender: löschen %s (fertig|Fehler)' % re.escape(kal), ab, 60)
    (ok if m and m.group(1) == 'fertig' else falsch)('Löschen nach der Absage: %s' % (m and m.group(0)))


def pruefen():
    soll = vergleich()
    if not starten():
        return
    seitenleiste(soll)
    time.sleep(2)
    bild('start')
    if MODUS == 'bild':
        zum_monat()
        time.sleep(3)
        bild('monat')
        # #374: Woche und Tag (heute).
        taste('Control_L', 't')
        taste('Alt_L', '2')
        time.sleep(3)
        bild('woche')
        taste('Alt_L', '3')
        time.sleep(3)
        bild('tag')
        return
    if MODUS == 'woche':
        woche()
        return
    if MODUS == 'termine':
        termine()
        return
    if MODUS == 'einladung':
        einladung()
        return
    if MODUS == 'serie':
        serie()
        return
    # Erstes Laden ohne Blättern: aktueller Monat je Kalender = Python-GI (Änderungen während des ersten Ladens,
    # gcal-calendar-monitor.c, Revolution #373).
    termine_vergleichen(False, datetime.date.today().strftime('%Y-%m'))
    popover_und_scrollen()
    tastatur()
    erster_montag = testtermine_anlegen()
    zum_monat()
    termine_vergleichen(True)
    wiederholung(erster_montag)
    time.sleep(1)
    bild('monat')
    live()
    m = re.findall(r'Termin: .* UI-Thread (\S+)$', log_text(), re.M)
    (ok if m and all(x == 'ja' for x in m) else falsch)('Termine im UI-Thread an die Monatsansicht (%d Zeilen)' % len(m))
    aus_an(soll, False)
    bild('aus')
    beenden()
    # Neustart: Einstellungen in Datei (kalender.sh: GSETTINGS_BACKEND=keyfile), Kalender bleibt aus.
    ok('Neustart')
    if not starten():
        return
    m = warte(r'Kalender: %s .* sichtbar (\d)$' % re.escape(AUS_KALENDER), log_start, TIMEOUT)
    (ok if m and m.group(1) == '0' else falsch)('Nach Neustart: „%s“ weiter aus (sichtbar %s)' % (
        AUS_KALENDER[:8], m and m.group(1)))
    zum_monat()
    time.sleep(5)
    (ok if not app_monat(AUS_KALENDER) else falsch)('Nach Neustart: keine Termine von %s' % AUS_KALENDER[:8])
    box = check_box(soll[AUS_KALENDER][0].split(' / ', 1)[1])
    (ok if box is not None and not box.get_state_set().contains(Atspi.StateType.CHECKED) else falsch)(
        'Nach Neustart: Ankreuzfeld leer (AT-SPI)')
    aus_an(soll, True)


def main():
    global app
    open(LOG, 'w').close()
    try:
        pruefen()
        if app is not None and app.poll() is None:
            beenden()
        andere_zeilen()
    finally:
        if app is not None and app.poll() is None:
            app.kill()
            app.wait()
    for kal, n in sorted(schreibvorgaenge.items()):
        wert('Schreibvorgänge %s: %d' % (kal[:8], n))
    return 1 if fehler else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception:
        traceback.print_exc()
        falsch('Abbruch: %s' % traceback.format_exc().strip().splitlines()[-1])
        sys.exit(1)
