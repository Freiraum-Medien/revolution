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
from gi.repository import Atspi  # noqa: E402
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
        if schreibvorgaenge.get(args[2], 0) >= 20:
            raise RuntimeError('20 Schreibvorgänge in %s erreicht' % args[2])
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
        v, g = raster(seite)
        t = termine_in(v, g) if v is not None else {}
        if all((n in t) != weg for n in namen) or time.monotonic() > ende:
            return v, g, t
        time.sleep(0.2)


def testwoche_anlegen(kal, montag):
    """Drei überlappende (Di 10:00, 10:30, 10:45, je 1 h), ganztägig (Mi), mehrtägig (Do 14:00 – Sa 16:00)."""
    namen = {}
    di, mi, do = (montag + datetime.timedelta(days=i) for i in (1, 2, 3))
    for art, aktion, wann in (('ueber1', 'anlegen', '%sT10:00' % di), ('ueber2', 'anlegen', '%sT10:30' % di),
                              ('ueber3', 'anlegen', '%sT10:45' % di), ('ganztags', 'ganztags', '%sT00:00' % mi),
                              ('mehrtaegig', 'mehrtaegig', '%sT14:00' % do)):
        aus = testdaten('--termin-live', aktion, kal, datei(kal, art), wann)
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
    kal = SCHREIBBAR[0]
    heute = datetime.date.today()
    montag_heute = heute - datetime.timedelta(days=heute.weekday())
    montag = montag_heute + datetime.timedelta(days=7)
    namen = testwoche_anlegen(kal, montag)
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
    # Tag: Alt+3 zeigt den gewählten Tag (Donnerstag der Testwoche), Alt+Links 2× = Dienstag.
    taste_ansicht(('Alt_L', '3'), 'day', heute + datetime.timedelta(days=7), 'Alt+3')
    di = montag + datetime.timedelta(days=1)
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
    # Live von außen (zweiter Python-GI-Prozess), Mittwoch 15:00 der Testwoche.
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
    _, g, t = warte_termine('Woche', [name + ' geändert'], 30)
    dauer = time.monotonic() - t0
    e = t.get(name + ' geändert', [None])[0]
    beginn = round((e[1] - ausdehnung(g)[1]) / ausdehnung(g)[3] * 1440) if e else None
    (ok if e and dauer <= LIVE_GRENZE and abs(beginn - 16 * 60) <= 3 and name not in t else falsch)(
        'Live ändern: neuer Titel und Beginn %s min (soll 960 = 16:00) nach %.1f s' % (beginn, dauer))
    testdaten('--termin-live', 'loeschen', kal, pfad)
    t0 = time.monotonic()
    _, g, t = warte_termine('Woche', [name + ' geändert'], 30, weg=True)
    dauer = time.monotonic() - t0
    (ok if name + ' geändert' not in t and dauer <= LIVE_GRENZE else falsch)(
        'Live löschen: aus der Woche nach %.1f s' % dauer)


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
