#!/usr/bin/env python3
# kontakte-treiber.py – Treiber für tests/kontakte.sh (#368, #369, #370), läuft im Xvfb aus xvfb-run.
#
# Startet die App (BIN) mit REVOLUTION_BOOK=BUCH, wechselt mit Strg+3 zu den Kontakten und prüft über Log
# (G_MESSAGES_DEBUG=revolution, „Adressbuch: …“, „Kontakte: …“, contacts-view.c) und Barrierefreiheitsbaum (AT-SPI):
#   MODUS=alle    Seitenleiste = VERGLEICH (tests/testdaten.py --adressbuecher, eigener Prozess): je Buch UID, Konto,
#                 Name, Reihenfolge der Kopfzeilen, „nur lesend“; je Buch Auswahl über die Liste „Adressbücher“
#                 und Kontaktzahl = VERGLEICH; Alt+M springt (A–Z-Leiste). Danach wie live.
#   MODUS=live    Testkontakt von außen (tests/testdaten.py --kontakt-live, eigener Prozess über EDS wie GNOME
#                 Contacts) anlegen, suchen (Treffer, Nicht-Treffer, Escape), ändern, löschen; jede Änderung ohne
#                 Neuladen in der Liste (Log + AT-SPI + Bild).
#   GRAPH=1       zusätzlich Gegenprobe am Server (tests/testdaten.py --kontakt-graph, Graph ohne EDS): erscheint der
#                 Kontakt ohne Refresh? sonst nach --kontakt-live refresh (e_book_client_refresh_sync).
#   MODUS=detail  nur die Detailansicht (#369, siehe DETAIL).
#   DETAIL        #369 nach den Schritten oben (leer = aus): Testkontakt mit allen Feldern von außen anlegen
#                 (tests/testdaten.py --kontakt-live detail), über die Suche wählen; jeder Wert und jede Typ-Bezeichnung
#                 im Detail (AT-SPI) gleich dem unabhängig per Python-GI gelesenen (--kontakt-live lesen), Bild; dann
#                 voll     „Mail schreiben“ oben per Maus und je Zeile per Tab/Enter öffnet den Composer mit
#                          „Name <Adresse>“ in An (AT-SPI), Escape schließt ohne Entwurf; Feld von außen ändern
#                          (--kontakt-live feld) erscheint im Detail; löschen leert das Detail.
#                 anzeige  wie voll ohne Feldänderung.
#                 m365     ohne Composer: welche Felder das Buch übernimmt (SOLL gegen FELDER, „WERT“), löschen.
#   MODUS=schreiben  #370: NURLESEND (Buch-UID): dort Bearbeiten/Löschen nicht angeboten, Entf ohne Wirkung; Strg+N,
#                 Buch ZIEL im Formular wählen, alle Felder per Tastatur, „Hinzufügen“ per Tab/Enter; Gegenprobe Python-GI
#                 (--kontakt-live lesen) und am Server ohne EDS (--kontakt-server); Spitzname von außen (extra), Escape
#                 bricht ab, Telefon und Notiz ändern („Fertig“), Spitzname bleibt; Entf + Rückgängig (nichts geschrieben),
#                 Entf + Ablauf → weg (Python-GI) und am Server 404; LOKAL=1 dazu „Persönlich“ anlegen/löschen.
#   MODUS=absender   #370: App startet in der Testmail (REVOLUTION_FOLDER/REVOLUTION_MESSAGE), „Weitere Aktionen“ →
#                 „Absender zu Kontakten hinzufügen“ → Formular vorausgefüllt (AT-SPI), Escape, nichts geschrieben.
# Umgebung: BIN, OUT, NAME, BUCH, MODUS, VERGLEICH, KONTAKT (Datei für die UID), GRAPH_DATEI, TESTDATEN, TIMEOUT,
#           DETAIL, DETAIL_DATEI (UID des Detail-Testkontakts), DETAIL_BILD (Name des Detail-Bilds).
# Ausgabe: „OK …“, „FEHLER …“, „WERT …“; Bilder OUT/NAME-<Schritt>.png.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

import json
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

BIN, OUT, NAME, BUCH = os.environ['BIN'], os.environ['OUT'], os.environ['NAME'], os.environ['BUCH']
MODUS = os.environ.get('MODUS', 'live')
VERGLEICH, KONTAKT = os.environ['VERGLEICH'], os.environ['KONTAKT']
GRAPH = os.environ.get('GRAPH') == '1'
GRAPH_DATEI = os.environ.get('GRAPH_DATEI', '')
TESTDATEN = os.environ['TESTDATEN']
TIMEOUT = int(os.environ.get('TIMEOUT', '120'))
GRAPH_WARTEN = int(os.environ.get('GRAPH_WARTEN', '60'))
LOG = os.path.join(OUT, NAME + '.log')
DETAIL = os.environ.get('DETAIL', '')
DETAIL_DATEI = os.environ.get('DETAIL_DATEI', '')
DETAIL_BILD = os.environ.get('DETAIL_BILD', NAME + '-detail')
SUCHE = 'Revolution-Test 368'
NICHT = 'Zqxjvwq'

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
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        m = re.search(muster, log_text()[ab:], re.M)
        if m:
            return m
        time.sleep(0.2)
    return None


def testdaten(*args):
    """tests/testdaten.py in eigenem Prozess; Ausgabe zurück, Abbruch → FEHLER."""
    r = subprocess.run([sys.executable, TESTDATEN] + list(args), capture_output=True, text=True, timeout=300)
    for z in (r.stdout + r.stderr).splitlines():
        print('        testdaten %s: %s' % (args[0], z), flush=True)
    if r.returncode != 0:
        raise RuntimeError('tests/testdaten.py %s: Exit %d' % (' '.join(args), r.returncode))
    return r.stdout


TASTEN = {' ': 'space', '-': 'minus'}


def taste(*namen):
    codes = [d.keysym_to_keycode(XK.string_to_keysym(n)) for n in namen]
    for c in codes:
        xtest.fake_input(d, X.KeyPress, c)
    for c in reversed(codes):
        xtest.fake_input(d, X.KeyRelease, c)
    d.sync()
    time.sleep(0.2)


def tippe(text):
    for ch in text:
        if ch.isupper():
            taste('Shift_L', ch.lower())
        else:
            taste(TASTEN.get(ch, ch))


def bild(schritt, pfad=None):
    root = d.screen().root
    g = root.get_geometry()
    pfad = pfad or os.path.join(OUT, '%s-%s.png' % (NAME, schritt))
    Image.frombytes('RGB', (g.width, g.height), root.get_image(0, 0, g.width, g.height, X.ZPixmap,
                                                               0xffffffff).data, 'raw', 'BGRX').save(pfad)
    wert('Bild %s' % pfad)


def fenster(titel='Revolution', weg=False, zeit=20):
    """X-Fenster mit diesem Titel (sichtbar); weg: warten, bis es keins mehr gibt (True)."""
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        treffer = None
        for w in d.screen().root.query_tree().children:
            try:
                if w.get_wm_name() == titel and w.get_attributes().map_state == X.IsViewable:
                    treffer = w
            except Exception:
                pass
        if weg and treffer is None:
            return True
        if not weg and treffer is not None:
            return treffer
        time.sleep(0.2)
    return False if weg else None


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
    if o.get_role_name() == rolle and (name is None or o.get_name() == name):
        return o
    for i in range(o.get_child_count()):
        k = o.get_child_at_index(i)
        t = k and finde(k, rolle, name)
        if t:
            return t
    return None


def zeilen(liste):
    """[(Rolle, Name)] der Kinder einer Liste (Kopfzeilen „row header“, Einträge „list item“)."""
    return [(k.get_role_name(), k.get_name()) for i in range(liste.get_child_count())
            for k in [liste.get_child_at_index(i)] if k is not None]


def kontaktliste():
    return finde(wurzel(), 'list', 'Kontakte')


def fokus_liste():
    """Tastaturfokus in die Kontaktliste wie ein Klick auf ihren Rand unten (Kürzel der Kontakte-Ansicht wirken
    lokal, contacts-view.blp). AT-SPI grab_focus kann GTK 4 nicht (atspi_error); Wählen über AT-SPI gibt keinen
    Fokus. Lage der Liste aus AT-SPI (Bildschirmkoordinaten im Xvfb ohne Fenstermanager)."""
    liste = kontaktliste()
    if liste is None:
        falsch('Fokus: Liste „Kontakte“ nicht im Barrierefreiheitsbaum')
        return
    r = liste.get_component_iface().get_extents(Atspi.CoordType.SCREEN)
    x, y = r.x + r.width // 2, r.y + r.height - 4
    xtest.fake_input(d, X.MotionNotify, x=x, y=y)
    xtest.fake_input(d, X.ButtonPress, 1)
    xtest.fake_input(d, X.ButtonRelease, 1)
    d.sync()
    time.sleep(0.5)


def warte_zeile(name, da=True, zeit=15):
    """Zeile mit diesem Namen in der Liste „Kontakte“ (da) bzw. nicht mehr (not da)."""
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        liste = kontaktliste()
        namen = [n for r, n in zeilen(liste)] if liste is not None else []
        if (name in namen) == da:
            return True
        time.sleep(0.3)
    return False


# ------------------------------------------------------------------ Schritte

def vergleich():
    """{UID: (Konto / Name, nurlesend, kontakte|None)} aus tests/testdaten.py --adressbuecher."""
    out = {}
    for z in open(VERGLEICH):
        m = re.match(r'^ADRESSBUCH (\S+) (.*) nurlesend (\d) initial (\d) kontakte (\S+) listen \S+$', z.strip())
        if m:
            out[m.group(1)] = (m.group(2), m.group(3) == '1', None if m.group(5) == '-' else int(m.group(5)))
    return out


def fertig(uid, ab, zeit=TIMEOUT):
    m = warte(r'Kontakte: %s fertig, Liste (\d+), ' % re.escape(uid), ab, zeit)
    return int(m.group(1)) if m else None


def seitenleiste(soll):
    log = [(int(m.group(1)), m.group(2), m.group(3)) for m in
           re.finditer(r'Adressbuch: (\d+) (\S+) (.*)$', log_text(), re.M)]
    n = len(soll)
    log = log[-n:] if len(log) >= n else log
    ist = {u: t for _, u, t in log}
    (ok if ist == {u: v[0] for u, v in soll.items()} else falsch)(
        'Seitenleiste: %d Adressbücher wie die Registry (unabhängig per Python-GI): %s'
        % (len(ist), ', '.join('%s %s' % (u[:8], t) for _, u, t in log)))
    liste = finde(wurzel(), 'list', 'Adressbücher')
    if liste is None:
        falsch('Seitenleiste: Liste „Adressbücher“ nicht im Barrierefreiheitsbaum')
        return log
    z = zeilen(liste)
    titel = [n for r, n in z if r != 'row header']
    koepfe = [n for r, n in z if r == 'row header']
    soll_titel = [t.split(' / ', 1)[1] for _, _, t in log]
    soll_koepfe = []
    for _, _, t in log:
        if not soll_koepfe or soll_koepfe[-1] != t.split(' / ', 1)[0]:
            soll_koepfe.append(t.split(' / ', 1)[0])
    (ok if titel == soll_titel else falsch)('Seitenleiste (AT-SPI): Einträge %s' % titel)
    (ok if koepfe == soll_koepfe else falsch)('Seitenleiste (AT-SPI): Kopfzeilen je Konto %s' % koepfe)
    for u, (t, nurlesend, _) in soll.items():
        m = warte(r'Kontakte: %s verbunden, nur lesend (ja|nein)' % re.escape(u), 0, 60)
        (ok if m and (m.group(1) == 'ja') == nurlesend else falsch)(
            '%s: nur lesend %s (Python-GI %s)' % (t, m and m.group(1), 'ja' if nurlesend else 'nein'))
    return log


def buch_waehlen(position, uid):
    liste = finde(wurzel(), 'list', 'Adressbücher')
    ab = marke()
    if liste is None or not liste.select_child(position):
        falsch('Adressbuch %s (Zeile %d) nicht gewählt' % (uid, position))
        return None
    if not warte(r'Kontakte: Buch %s gewählt' % re.escape(uid), ab, 20):
        falsch('Adressbuch %s: Auswahl nicht im Log' % uid)
        return None
    return ab


def zaehlen(soll, log):
    gezaehlt = {}
    for position, uid, titel in log:
        ab = buch_waehlen(position, uid)
        if ab is None:
            continue
        erwartet = soll[uid][2]
        if erwartet is None:
            (ok if warte(r'Kontakte: %s ohne do-initial-query' % re.escape(uid), ab, 60) else falsch)(
                '%s: ohne Gesamtliste, Hinweis „Suchen, um Kontakte anzuzeigen“' % titel)
            continue
        n = gezaehlt[uid] = fertig(uid, ab)
        (ok if n == erwartet else falsch)('%s: %s Kontakte, Python-GI %s' % (titel, n, erwartet))
        if n and n > 100:
            time.sleep(1)
            bild('liste-%s' % uid[:8])
            fokus_liste()
            ab = marke()
            taste('Alt_L', 'm')
            m = warte(r'Kontakte: Sprung M → Zeile (\d+)', ab, 10)
            (ok if m else falsch)('Alt+M springt zu „M“ (Zeile %s)' % (m and m.group(1)))
            time.sleep(1)
            bild('sprung-m')
    return gezaehlt


def suchen(name, n0):
    fokus_liste()
    ab = marke()
    taste('Control_L', 'f')
    tippe(SUCHE)
    sexp = re.escape('(contains "x-evolution-any-field" "%s")' % SUCHE)
    m = warte(r'Kontakte: %s Ansicht „%s“' % (re.escape(BUCH), sexp), ab, 30)
    if m is None:
        falsch('Suche „%s“: keine Ansicht mit EBookQuery' % SUCHE)
        return
    n = fertig(BUCH, ab + m.end())
    (ok if n == 1 else falsch)('Suche „%s“ (EBookQuery any-field contains): %s Treffer, erwartet 1' % (SUCHE, n))
    (ok if warte_zeile(name) else falsch)('Suche: Testkontakt „%s“ in der Liste' % name)
    bild('suche')
    ab = marke()
    taste('Control_L', 'a')
    tippe(NICHT)
    m = warte(r'Kontakte: %s Ansicht „%s“' % (re.escape(BUCH), re.escape('(contains "x-evolution-any-field" "%s")' % NICHT)), ab, 30)
    n = fertig(BUCH, ab + m.end()) if m else None
    (ok if n == 0 else falsch)('Suche „%s“: %s Treffer, erwartet 0' % (NICHT, n))
    time.sleep(0.5)
    (ok if finde(wurzel(), 'label', 'Keine Ergebnisse') is not None else falsch)('Nicht-Treffer: „Keine Ergebnisse“ sichtbar')
    bild('keine-treffer')
    ab = marke()
    taste('Escape')
    m = warte(r'Kontakte: %s Ansicht „%s“' % (re.escape(BUCH), re.escape('(contains "x-evolution-any-field" "")')), ab, 30)
    n = fertig(BUCH, ab + m.end()) if m else None
    (ok if n == n0 else falsch)('Escape: wieder alle Kontakte (%s, vorher %s)' % (n, n0))


def live(n0):
    t = time.monotonic()
    ab = marke()
    aus = testdaten('--kontakt-live', 'anlegen', BUCH, KONTAKT)
    m = re.search(r'^ANGELEGT (\S+) (.*)$', aus, re.M)
    uid, name = m.group(1), m.group(2)
    m = warte(r'Kontakte: %s neu %s „%s“' % (re.escape(BUCH), re.escape(uid), re.escape(name)), ab, 60)
    (ok if m else falsch)('Live anlegen (von außen, EDS): „%s“ ohne Neuladen nach %.1f s' % (name, time.monotonic() - t))
    m = warte(r'Kontakte: %s neu 1, geändert 0, Liste (\d+) \(UI-Thread (ja|nein)\)' % re.escape(BUCH), ab, 10)
    (ok if m and int(m.group(1)) == n0 + 1 and m.group(2) == 'ja' else falsch)(
        'Live anlegen: Liste %s (vorher %d), Signal im UI-Thread %s' % (m and m.group(1), n0, m and m.group(2)))
    (ok if warte_zeile(name) else falsch)('Live anlegen: Zeile „%s“ (AT-SPI)' % name)
    bild('live-neu')
    if MODUS == 'alle':
        suchen(name, n0 + 1)
    t = time.monotonic()
    ab = marke()
    aus = testdaten('--kontakt-live', 'aendern', BUCH, KONTAKT)
    neu = re.search(r'^GEAENDERT \S+ (.*)$', aus, re.M).group(1)
    m = warte(r'Kontakte: %s geändert %s „%s“' % (re.escape(BUCH), re.escape(uid), re.escape(neu)), ab, 60)
    (ok if m else falsch)('Live ändern: „%s“ ohne Neuladen nach %.1f s' % (neu, time.monotonic() - t))
    (ok if warte_zeile(neu) and warte_zeile(name, False) else falsch)('Live ändern: Zeile heißt jetzt „%s“ (AT-SPI)' % neu)
    bild('live-geaendert')
    t = time.monotonic()
    ab = marke()
    testdaten('--kontakt-live', 'loeschen', BUCH, KONTAKT)
    m = warte(r'Kontakte: %s entfernt %s$' % (re.escape(BUCH), re.escape(uid)), ab, 60)
    (ok if m else falsch)('Live löschen: ohne Neuladen nach %.1f s' % (time.monotonic() - t))
    m = warte(r'Kontakte: %s entfernt 1, Liste (\d+)' % re.escape(BUCH), ab, 10)
    (ok if m and int(m.group(1)) == n0 else falsch)('Live löschen: Liste %s (vorher %d)' % (m and m.group(1), n0))
    (ok if warte_zeile(neu, False) else falsch)('Live löschen: Zeile „%s“ weg (AT-SPI)' % neu)
    bild('live-geloescht')


def graph_probe():
    """Änderung am Server ohne EDS: kommt sie ohne Refresh? Sonst nach e_book_client_refresh_sync."""
    t = time.monotonic()
    ab = marke()
    aus = testdaten('--kontakt-graph', 'anlegen', GRAPH_DATEI)
    name = re.search(r'^ANGELEGT \S+ (.*)$', aus, re.M).group(1)
    muster = r'Kontakte: %s neu (\S+) „%s“' % (re.escape(BUCH), re.escape(name))
    m = warte(muster, ab, GRAPH_WARTEN)
    if m:
        wert('Graph anlegen: „%s“ ohne Refresh nach %.1f s' % (name, time.monotonic() - t))
    else:
        wert('Graph anlegen: „%s“ nach %d s ohne Refresh nicht in der Liste (Grenze des Unterbaus)' % (name, GRAPH_WARTEN))
        t = time.monotonic()
        testdaten('--kontakt-live', 'refresh', BUCH, KONTAKT)
        m = warte(muster, ab, 60)
        (ok if m else falsch)('Graph anlegen: nach e_book_client_refresh live in der Liste nach %.1f s' % (time.monotonic() - t))
    if m:
        (ok if warte_zeile(name) else falsch)('Graph anlegen: Zeile „%s“ (AT-SPI)' % name)
        bild('graph-neu')
    uid = m.group(1) if m else None
    t = time.monotonic()
    ab = marke()
    testdaten('--kontakt-graph', 'loeschen', GRAPH_DATEI)
    if uid is None:
        return
    muster = r'Kontakte: %s entfernt %s$' % (re.escape(BUCH), re.escape(uid))
    m = warte(muster, ab, GRAPH_WARTEN)
    if m:
        wert('Graph löschen: ohne Refresh nach %.1f s aus der Liste' % (time.monotonic() - t))
    else:
        wert('Graph löschen: nach %d s ohne Refresh noch in der Liste (Grenze des Unterbaus)' % GRAPH_WARTEN)
        t = time.monotonic()
        testdaten('--kontakt-live', 'refresh', BUCH, KONTAKT)
        m = warte(muster, ab, 60)
        (ok if m else falsch)('Graph löschen: nach e_book_client_refresh live entfernt nach %.1f s' % (time.monotonic() - t))
    (ok if warte_zeile(name, False) else falsch)('Graph löschen: Zeile „%s“ weg (AT-SPI)' % name)


# ------------------------------------------------------------------ Detailansicht (#369)

EMAIL_TYP = (('WORK', 'Arbeit'), ('HOME', 'Privat'))
# Wie Evolution get_phone_location: genau ein Sondertyp → sein Name (Ort Arbeit/Privat in Klammern), Fax ohne Ort
# → „Weiteres Fax“, PREF allein → „Telefon, primär“, sonst nur der Ort bzw. „Telefon“ (#369 Runde 2).
TELEFON_SONDER = (('X-EVOLUTION-ASSISTANT', 'Telefon des Assistenten'), ('X-EVOLUTION-CALLBACK', 'Rückruf-Telefon'),
                  ('CAR', 'Autotelefon'), ('X-EVOLUTION-COMPANY', 'Firmentelefon'), ('ISDN', 'ISDN'), ('CELL', 'Mobil'),
                  ('FAX', 'Fax'), ('PAGER', 'Pager'), ('X-EVOLUTION-RADIO', 'Funk'), ('X-EVOLUTION-TELEX', 'Telex'),
                  ('X-EVOLUTION-TTYTDD', 'TTY/TDD'))


def telefon_typ(typen):
    arbeit, privat = 'WORK' in typen, 'HOME' in typen
    ort = 'Arbeit' if arbeit and not privat else 'Privat' if privat and not arbeit else None
    sonder = [(t, n) for t, n in TELEFON_SONDER if t in typen]
    if not sonder and 'PREF' in typen:
        return 'Telefon, primär'
    if len(sonder) != 1:
        return ort or 'Telefon'
    if ort is None:
        return 'Weiteres Fax' if sonder[0][0] == 'FAX' else sonder[0][1]
    return '%s (%s)' % (sonder[0][1], ort)
ADRESSE_TYP = (('work', 'Arbeit'), ('home', 'Privat'), ('other', 'Weitere'))


def typ_name(typen, tabelle, sonst):
    for typ, name in tabelle:
        if typ in typen:
            return name
    return sonst


def adresse_text(a):
    """Erwarteter Text einer Adresse: Label der vCard, sonst Postfach, Zusatz, Straße, PLZ Ort, Region, Land."""
    if a.get('label'):
        return a['label']
    ort = ' '.join(x for x in (a.get('code'), a.get('locality')) if x).strip()
    return '\n'.join(x for x in (a.get('po'), a.get('ext'), a.get('street'), ort, a.get('region'), a.get('country')) if x)


def detail_soll(f):
    """{Liste (Barrierefreiheits-Name): [(Typ, Wert)]} wie die Detailansicht sie zeigen muss (leere fehlen)."""
    soll = {'E-Mail-Adressen': [(typ_name(e['typen'], EMAIL_TYP, 'E-Mail'), e['wert'].strip()) for e in f['emails']],
            'Telefonnummern': [(telefon_typ(t['typen']), t['wert'].strip()) for t in f['telefone']],
            'Adressen': [(name, adresse_text(f['adressen'][art])) for art, name in ADRESSE_TYP
                         if art in f['adressen'] and adresse_text(f['adressen'][art])],
            'Organisation': [(n, f[k]) for k, n in (('firma', 'Firma'), ('abteilung', 'Abteilung'),
                                                     ('position', 'Position')) if f.get(k)],
            'Notizen': [('Notizen', f['notiz'])] if f.get('notiz') else []}
    return {k: v for k, v in soll.items() if v}


def knoten(o):
    if o is None:
        return
    yield o
    for i in range(o.get_child_count()):
        yield from knoten(o.get_child_at_index(i))


def zeilen_texte(liste):
    """[[Texte der Labels]] je Zeile („list item“) einer Liste."""
    out = []
    for i in range(liste.get_child_count()):
        z = liste.get_child_at_index(i)
        if z is not None:
            out.append([k.get_name() for k in knoten(z) if k.get_role_name() == 'label'])
    return out


def detail_lesen(felder_vorher=None):
    aus = testdaten('--kontakt-live', 'lesen', BUCH, DETAIL_DATEI)
    return json.loads(re.search(r'^FELDER (.*)$', aus, re.M).group(1))


def detail_vergleichen(f, schritt):
    """Jeder Wert und Typ im Detail (AT-SPI) gleich dem unabhängig gelesenen (Python-GI)."""
    w = wurzel()
    name = f['name']
    labels = [k.get_name() for k in knoten(w) if k.get_role_name() == 'label']
    (ok if labels.count(name) >= 2 else falsch)('%s: Name „%s“ im Kopf (und in der Liste), AT-SPI' % (schritt, name))
    soll = detail_soll(f)
    for liste_name in ('E-Mail-Adressen', 'Telefonnummern', 'Adressen', 'Organisation', 'Notizen'):
        liste = finde(w, 'list', liste_name)
        ist = zeilen_texte(liste) if liste is not None else []
        erwartet = soll.get(liste_name, [])
        if not erwartet:
            (ok if liste is None or not ist else falsch)('%s: Karte „%s“ leer und nicht angezeigt (%s)'
                                                         % (schritt, liste_name, ist))
            continue
        passt = len(ist) == len(erwartet) and all(typ in z and wert in z for (typ, wert), z in zip(erwartet, ist))
        (ok if passt else falsch)('%s: Karte „%s“ = Python-GI: %s%s' % (
            schritt, liste_name, ' | '.join('%s: %s' % (t, v.replace('\n', '⏎')) for t, v in erwartet),
            '' if passt else ' – angezeigt: %s' % ist))


def detail_suchen(name):
    """Testkontakt über die Suche (ein Treffer) wählen: AT-SPI select_child nimmt die Position im Modell."""
    fokus_liste()
    ab = marke()
    taste('Control_L', 'f')
    begriff = name.split(' ', 1)[1]
    tippe(begriff)
    m = warte(r'Kontakte: %s Ansicht „%s“' % (re.escape(BUCH), re.escape('(contains "x-evolution-any-field" "%s")'
                                                                          % begriff)), ab, 30)
    n = fertig(BUCH, ab + m.end(), 60) if m else None
    if n != 1:
        falsch('Detail: Suche „%s“: %s Treffer, erwartet 1' % (name, n))
        return None
    if not warte_zeile(name):
        falsch('Detail: Zeile „%s“ nicht in der Liste' % name)
        return None
    ab = marke()
    kontaktliste().select_child(0)
    return ab


def composer_fenster(weg=False, zeit=20):
    """Composer: sichtbares Fenster mit anderem Titel als das Hauptfenster (wie composer-treiber.py)."""
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        treffer = None
        for w in d.screen().root.query_tree().children:
            try:
                name = w.get_wm_name()
                if name and name != 'Revolution' and w.get_attributes().map_state == X.IsViewable:
                    treffer = w
            except Exception:
                pass
        if (treffer is None) == weg:
            return True if weg else treffer
        time.sleep(0.2)
    return False if weg else None


def fokussiert():
    for k in knoten(wurzel()):
        if k.get_state_set().contains(Atspi.StateType.FOCUSED):
            return k
    return None


def composer_pruefen(an, weg, ab):
    """Composer offen mit An = an (AT-SPI), Escape schließt ohne Entwurf."""
    m = warte(r'Composer: mailto An (\d+), ', ab, 30)
    w = composer_fenster()
    if not w:
        falsch('Mail schreiben %s: kein Composer (%s)' % (weg, m and m.group(0)))
        return
    warte(r'Composer: Neu vorbereitet', ab, 30)
    time.sleep(2)
    titel = w.get_wm_name()
    frame = finde(wurzel(), 'frame', titel)
    feld = [k for k in knoten(frame) if k.get_name() == 'An' and k.get_role_name() in ('entry', 'text')]
    text = Atspi.Text.get_text(feld[0], 0, Atspi.Text.get_character_count(feld[0])) if feld else None
    (ok if m and m.group(1) == '1' and text == an else falsch)(
        'Mail schreiben %s: Composer „%s“ mit An „%s“ (erwartet „%s“)' % (weg, titel, text, an))
    bild('composer-' + re.sub(r'[^a-z0-9]+', '-', weg.lower()).strip('-')[:40])
    g = w.get_geometry()
    xtest.fake_input(d, X.MotionNotify, x=g.x + g.width // 2, y=g.y + g.height // 2)
    d.sync()
    time.sleep(0.3)
    taste('Escape')
    (ok if composer_fenster(weg=True) else falsch)('Mail schreiben %s: Escape schließt den Composer' % weg)
    (ok if warte(r'Entwurf: ', ab, 3) is None else falsch)('Mail schreiben %s: ungeändert geschlossen, kein Entwurf' % weg)
    # Ohne Fenstermanager bleibt der X-Fokus nach dem Schließen beim Composer-Fenster stehen: wieder dem Zeiger
    # folgen lassen (wie beim Start), Zeiger über das Hauptfenster.
    d.set_input_focus(X.PointerRoot, X.RevertToPointerRoot, X.CurrentTime)
    xtest.fake_input(d, X.MotionNotify, x=900, y=500)
    d.sync()
    time.sleep(0.5)


def detail():
    t = time.monotonic()
    ab = marke()
    aus = testdaten('--kontakt-live', 'detail', BUCH, DETAIL_DATEI)
    uid, name = re.search(r'^ANGELEGT (\S+) (.*)$', aus, re.M).groups()
    soll = json.loads(re.search(r'^SOLL (.*)$', aus, re.M).group(1))
    (ok if warte(r'Kontakte: %s neu %s ' % (re.escape(BUCH), re.escape(uid)), ab, 60) else falsch)(
        'Detail: Testkontakt „%s“ live in der Liste nach %.1f s' % (name, time.monotonic() - t))
    f = detail_lesen()
    wert('Detail Python-GI: %s' % json.dumps(f, ensure_ascii=False))
    for k in ('name', 'given', 'family', 'emails', 'telefone', 'adressen', 'firma', 'abteilung', 'position', 'notiz', 'foto'):
        if f.get(k) != soll.get(k):
            wert('Detail Feld %s: geschrieben %s, im Buch %s' % (k, json.dumps(soll.get(k), ensure_ascii=False),
                                                                json.dumps(f.get(k), ensure_ascii=False)))
    gleich = [k for k in ('name', 'emails', 'telefone', 'adressen', 'firma', 'abteilung', 'position', 'notiz', 'foto')
              if f.get(k) == soll.get(k)]
    wert('Detail: Buch übernimmt unverändert %s' % ', '.join(gleich))
    ab = detail_suchen(name)
    if ab is None:
        return
    m = warte(r'Kontakt-Detail: %s „%s“, (.*)$' % (re.escape(uid), re.escape(f['name'])), ab, 20)
    (ok if m else falsch)('Detail: Auswahl zeigt den Kontakt (%s)' % (m and m.group(1)))
    time.sleep(2)
    detail_vergleichen(f, 'Detail')
    bild('detail', os.path.join(OUT, DETAIL_BILD + '.png'))
    # Zweites Bild mit Organisation und Notiz: Detail ans Ende rollen (Mausrad über dem Detail), dann zurück.
    for knopf in (5, 4):
        xtest.fake_input(d, X.MotionNotify, x=1000, y=600)
        for _ in range(15):
            xtest.fake_input(d, X.ButtonPress, knopf)
            xtest.fake_input(d, X.ButtonRelease, knopf)
        d.sync()
        time.sleep(1)
        if knopf == 5:
            bild('detail-unten', os.path.join(OUT, DETAIL_BILD + '-unten.png'))
    if DETAIL in ('voll', 'anzeige') and f['emails']:
        an = [('%s <%s>' % (f['name'], e['wert'].strip())) for e in f['emails']]
        # Tastatur: Tab bis „Mail schreiben“ oben (erste Adresse), Enter; dann weiter bis zum Papierflieger der
        # letzten Adresse, Enter. Mausklick per Koordinaten geht hier nicht: AT-SPI meldet für Knöpfe im
        # ScrolledWindow falsche Lagen (gemessen 20,20 40x40); die erste Zeile daher über die AT-SPI-Aktion.
        for ziel, adresse in (('Mail schreiben', an[0]), ('Mail an %s schreiben' % f['emails'][-1]['wert'].strip(), an[-1])):
            gefunden = []
            for _ in range(16):
                taste('Tab')
                k = fokussiert()
                gefunden.append(k.get_name() if k else None)
                if k is not None and k.get_name() == ziel:
                    break
            if gefunden and gefunden[-1] == ziel:
                ok('Tastatur: Tab erreicht „%s“ (%s)' % (ziel, ' → '.join(str(x) for x in gefunden)))
                ab = marke()
                taste('Return')
                composer_pruefen(adresse, '„%s“ (Tab/Enter)' % ziel, ab)
            else:
                falsch('Tastatur: Tab erreicht „%s“ nicht (%s)' % (ziel, ' → '.join(str(x) for x in gefunden)))
        ziel = 'Mail an %s schreiben' % f['emails'][0]['wert'].strip()
        knopf = finde(wurzel(), 'button', ziel)
        aktion = knopf.get_action_iface() if knopf is not None else None
        if aktion is None or aktion.get_n_actions() < 1:
            falsch('„%s“: keine AT-SPI-Aktion' % ziel)
        else:
            ab = marke()
            aktion.do_action(0)
            composer_pruefen(an[0], '„%s“ (AT-SPI-Aktion %s)' % (ziel, aktion.get_action_name(0)), ab)
    if DETAIL == 'voll':
        t = time.monotonic()
        ab = marke()
        testdaten('--kontakt-live', 'feld', BUCH, DETAIL_DATEI)
        m = warte(r'Kontakt-Detail: %s „' % re.escape(uid), ab, 60)
        (ok if m else falsch)('Live: Feldänderung von außen im Detail nach %.1f s' % (time.monotonic() - t))
        time.sleep(1)
        neu = detail_lesen()
        (ok if neu['notiz'] != f['notiz'] and neu['telefone'] != f['telefone'] else falsch)(
            'Live: Notiz und Mobil im Buch geändert (Python-GI)')
        detail_vergleichen(neu, 'Live')
        alt = [t['wert'] for t in f['telefone'] if 'CELL' in t['typen']]
        labels = [k.get_name() for k in knoten(wurzel()) if k.get_role_name() == 'label']
        (ok if alt and alt[0] not in labels else falsch)('Live: alte Mobilnummer %s nicht mehr angezeigt' % alt)
        bild('detail-live')
    t = time.monotonic()
    ab = marke()
    testdaten('--kontakt-live', 'loeschen', BUCH, DETAIL_DATEI)
    m = warte(r'Kontakt-Detail: leer', ab, 60)
    (ok if m else falsch)('Löschen von außen leert das Detail nach %.1f s' % (time.monotonic() - t))
    time.sleep(1)
    (ok if finde(wurzel(), 'label', 'Kein Kontakt ausgewählt') is not None else falsch)(
        'Löschen: „Kein Kontakt ausgewählt“ sichtbar (AT-SPI)')
    bild('detail-geloescht')
    taste('Escape')


# ------------------------------------------------------------------ Anlegen, Bearbeiten, Löschen (#370)

ZIEL = os.environ.get('ZIEL', BUCH)
NURLESEND = os.environ.get('NURLESEND', '')
LOKAL = os.environ.get('LOKAL') == '1'
LOKAL_BUCH = 'system-address-book'
KONTAKT_LOKAL = os.environ.get('KONTAKT_LOKAL', KONTAKT + '.lokal')
BILD_PREFIX = os.environ.get('BILD_PREFIX', '')
LOESCHEN_S = 5  # contacts-view.c DELETE_SECONDS
_belegt = {}


def schreibe(text):
    """Text per XTEST; Zeichen ohne Taste in der Xvfb-Belegung (Umlaute) über eine freie Taste, deren Belegung
    vorher auf das Zeichen gesetzt wird."""
    shift = d.keysym_to_keycode(XK.string_to_keysym('Shift_L'))
    for ch in text:
        if ch == '\n':
            taste('Return')
            continue
        sym = ord(ch) if ord(ch) < 0x100 else 0x01000000 + ord(ch)
        codes = list(d.keysym_to_keycodes(sym))
        if not codes:
            if not _belegt:
                info = d.display.info
                leer = [k for k in range(info.max_keycode, info.min_keycode, -1)
                        if not any(d.keycode_to_keysym(k, i) for i in range(4))]
                _belegt['frei'] = leer[0]
            d.change_keyboard_mapping(_belegt['frei'], [(sym, sym, sym, sym)])
            d.sync()
            time.sleep(0.1)
            codes = [(_belegt['frei'], 0)]
        code, index = codes[0]
        if index % 2:
            xtest.fake_input(d, X.KeyPress, shift)
        xtest.fake_input(d, X.KeyPress, code)
        xtest.fake_input(d, X.KeyRelease, code)
        if index % 2:
            xtest.fake_input(d, X.KeyRelease, shift)
        d.sync()
        time.sleep(0.03)
    time.sleep(0.3)


def tab_bis(name, rolle=None, schritte=45):
    """Tab, bis der Fokus (AT-SPI) auf einem Element mit diesem Namen (und dieser Rolle) liegt."""
    weg = []
    for _ in range(schritte):
        k = fokussiert()
        if k is not None and k.get_name() == name and (rolle is None or k.get_role_name() == rolle):
            return True
        weg.append(k.get_name() if k else None)
        taste('Tab')
    falsch('Tastatur: Tab erreicht „%s“ nicht (%s)' % (name, ' → '.join(str(x) for x in weg[-12:])))
    return False


def feld(name, text, ersetzen=True):
    if not tab_bis(name, 'text'):
        return False
    if ersetzen:
        taste('Control_L', 'a')
    schreibe(text)
    return True


def typ_waehlen(schritte):
    """Fokus auf der Typ-Auswahl (GtkDropDown): öffnen (Leertaste), Pfeil runter, Enter."""
    taste('space')
    time.sleep(0.5)
    for _ in range(schritte):
        taste('Down')
    taste('Return')
    time.sleep(0.4)


def knopf(name):
    k = finde(wurzel(), 'button', name)
    a = k.get_action_iface() if k is not None else None
    if a is None or a.get_n_actions() < 1:
        falsch('Knopf „%s“ fehlt oder ohne AT-SPI-Aktion' % name)
        return False
    a.do_action(0)
    time.sleep(0.5)
    return True


def sichtbar(rolle, name):
    k = finde(wurzel(), rolle, name)
    return k is not None and k.get_state_set().contains(Atspi.StateType.SHOWING)


def text_von(name):
    k = [x for x in knoten(wurzel()) if x.get_role_name() == 'text' and x.get_name() == name]
    return Atspi.Text.get_text(k[0], 0, Atspi.Text.get_character_count(k[0])) if k else None


def server(buch, datei):
    aus = testdaten('--kontakt-server', buch, datei)
    m = re.search(r'^SERVER (\S+)(?: (.*))?$', aus, re.M)
    return m.group(1), (json.loads(m.group(2)) if m and m.group(2) else None)


def lesen(buch, datei):
    return json.loads(re.search(r'^FELDER (.*)$', testdaten('--kontakt-live', 'lesen', buch, datei), re.M).group(1))


def vorhanden(buch, datei):
    return re.search(r'^VORHANDEN (\d)', testdaten('--kontakt-live', 'pruefen', buch, datei), re.M).group(1) == '1'


def soll_felder(zeit, konten):
    return {'given': 'Neu', 'family': 'Revolution-Test Neu %s' % zeit,
            'emails': [(konten[0], 'HOME'), (konten[1], 'WORK')],
            'telefone': [('+49 171 0000370', 'CELL'), ('+49 30 0000370', 'WORK')],
            'adresse': {'street': 'Friedrichstraße 70', 'code': '10117', 'locality': 'Berlin', 'country': 'Deutschland'},
            'firma': 'Freiraum Medien Test', 'abteilung': 'Entwicklung', 'position': 'Testerin',
            'notiz': 'Erste Zeile: Grüße aus Köln\nZweite Zeile: ÄÖÜ ß'}


def vergleiche_eds(f, s, schritt, m365=False):
    """Felder aus EDS (Python-GI, eigener Prozess) gegen das im Formular Eingegebene."""
    name = '%s %s' % (s['given'], s['family'])
    (ok if f['name'] == name and f['given'] == s['given'] and f['family'] == s['family'] else falsch)(
        '%s: Name „%s“ (Vorname %s, Nachname %s)' % (schritt, f['name'], f['given'], f['family']))
    mails = [(e['wert'], e['typen']) for e in f['emails']]
    if m365:
        passt = sorted(a for a, _ in mails) == sorted(a for a, _ in s['emails'])
    else:
        passt = len(mails) == len(s['emails']) and all(a == sa and st in ty for (a, ty), (sa, st) in zip(mails, s['emails']))
    (ok if passt else falsch)('%s: E-Mails %s' % (schritt, mails))
    tels = [(t['wert'], t['typen']) for t in f['telefone']]
    if m365:
        passt = sorted(a for a, _ in tels) == sorted(a for a, _ in s['telefone'])
    else:
        passt = len(tels) == len(s['telefone']) and all(a == sa and st in ty for (a, ty), (sa, st) in zip(tels, s['telefone']))
    (ok if passt else falsch)('%s: Telefone %s' % (schritt, tels))
    a = f['adressen'].get('work') or {}
    passt = all(a.get(k) == v for k, v in s['adresse'].items())
    (ok if passt else falsch)('%s: Adresse Arbeit %s' % (schritt, {k: a.get(k) for k in s['adresse']}))
    for k in ('firma', 'abteilung', 'position', 'notiz'):
        (ok if (f.get(k) or '').replace('\r\n', '\n').strip() == s[k] else falsch)(
            '%s: %s %s' % (schritt, k, json.dumps(f.get(k), ensure_ascii=False)))


def vergleiche_server(buch, datei, s, schritt):
    status, x = server(buch, datei)
    if buch == LOKAL_BUCH:
        wert('%s: lokales Buch, kein Server' % schritt)
        return
    if status != '200' or x is None:
        falsch('%s: am Server nicht gefunden (%s)' % (schritt, status))
        return
    wert('%s: Server %s' % (schritt, json.dumps(x, ensure_ascii=False)))
    if 'givenName' in x:  # Graph
        mails = sorted(e['address'] for e in x.get('emailAddresses', []))
        tels = sorted(([x['mobilePhone']] if x.get('mobilePhone') else []) + x.get('businessPhones', []) + x.get('homePhones', []))
        werte = {'Vorname': (x.get('givenName'), s['given']), 'Nachname': (x.get('surname'), s['family']),
                 'E-Mails': (mails, sorted(a for a, _ in s['emails'])),
                 'Telefone': (tels, sorted(a for a, _ in s['telefone'])),
                 'Firma': (x.get('companyName'), s['firma']), 'Abteilung': (x.get('department'), s['abteilung']),
                 'Position': (x.get('jobTitle'), s['position']),
                 'Notiz': ((x.get('personalNotes') or '').replace('\r\n', '\n').strip(), s['notiz']),
                 'Straße': ((x.get('businessAddress') or {}).get('street'), s['adresse']['street'])}
    else:  # CardDAV
        roh = x['notiz'] or ''
        if '\\:' in roh:
            # Google liefert „:“ in NOTE als „\:“, EDS e_vcard parst das nicht zurück (vCard 3.0 kennt nur \\ \, \; \n).
            wert('%s: Server-NOTE mit „\\:“ statt „:“ (Google-Escaping, Grenze des Unterbaus): %s' % (schritt, json.dumps(roh, ensure_ascii=False)))
            x['notiz'] = roh.replace('\\:', ':')
        werte = {'Vorname': (x['given'], s['given']), 'Nachname': (x['family'], s['family']),
                 'E-Mails': (sorted(e['wert'] for e in x['emails']), sorted(a for a, _ in s['emails'])),
                 'Telefone': (sorted(t['wert'] for t in x['telefone']), sorted(a for a, _ in s['telefone'])),
                 'Firma': (x['firma'], s['firma']), 'Abteilung': (x['abteilung'], s['abteilung']),
                 'Position': (x['position'], s['position']),
                 'Notiz': ((x['notiz'] or '').replace('\r\n', '\n').strip(), s['notiz']),
                 'Straße': ((x['adressen'].get('work') or {}).get('street'), s['adresse']['street'])}
    falsche = {k: v for k, v in werte.items() if v[0] != v[1]}
    (ok if not falsche else falsch)('%s: am Server ohne EDS (%s) gleich dem Formular%s' % (
        schritt, 'Graph' if 'givenName' in x else 'CardDAV GET', '' if not falsche else ': %s' % falsche))


def nurlesend_pruefen(soll):
    """Nur-lesend-Buch: Bearbeiten und Löschen nicht angeboten, Entf ohne Wirkung."""
    log = [(int(m.group(1)), m.group(2)) for m in re.finditer(r'Adressbuch: (\d+) (\S+) ', log_text(), re.M)]
    pos = [p for p, u in log[-len(soll):] if u == NURLESEND]
    if not pos:
        falsch('Nur lesend: Buch %s nicht in der Seitenleiste' % NURLESEND)
        return
    ab = buch_waehlen(pos[0], NURLESEND)
    n = fertig(NURLESEND, ab) if ab is not None else None
    if not n:
        falsch('Nur lesend: Buch %s ohne Kontakte (%s)' % (NURLESEND, n))
        return
    m = warte(r'Kontakte: %s verbunden, nur lesend (ja|nein)' % re.escape(NURLESEND), 0, 30)
    (ok if m and m.group(1) == 'ja' else falsch)('Nur lesend: Buch %s meldet nur lesend %s' % (NURLESEND, m and m.group(1)))
    ab = marke()
    kontaktliste().select_child(0)
    warte(r'Kontakt-Detail: ', ab, 10)
    time.sleep(1)
    (ok if not sichtbar('button', 'Kontakt bearbeiten') and not sichtbar('button', 'Kontakt löschen') else falsch)(
        'Nur lesend: „Kontakt bearbeiten“ und „Kontakt löschen“ nicht angeboten (AT-SPI)')
    fokus_liste()
    ab = marke()
    taste('Delete')
    (ok if warte(r'Kontakt: Löschen vorgemerkt', ab, 3) is None else falsch)('Nur lesend: Entf in der Liste löscht nicht')
    bild('nurlesend')
    ab = buch_waehlen([p for p, u in log[-len(soll):] if u == BUCH][0], BUCH)
    fertig(BUCH, ab)


def fokus_ansicht():
    """Fokus in die Kontakte-Ansicht: Liste, bei leerem Buch (Statusseite statt Liste) die Seitenleiste unten."""
    if kontaktliste() is not None:
        fokus_liste()
        return
    liste = finde(wurzel(), 'list', 'Adressbücher')
    r = liste.get_component_iface().get_extents(Atspi.CoordType.SCREEN)
    xtest.fake_input(d, X.MotionNotify, x=r.x + r.width // 2, y=r.y + r.height - 4)
    xtest.fake_input(d, X.ButtonPress, 1)
    xtest.fake_input(d, X.ButtonRelease, 1)
    d.sync()
    time.sleep(0.5)


def neu_formular(ziel, datei, voll, bildname=None, vorgabe=BUCH):
    """Strg+N, Buch im Formular wählen, Felder per Tastatur füllen, „Hinzufügen“ per Tab/Enter. Rückgabe (UID, SOLL)."""
    sys.path.insert(0, os.path.dirname(TESTDATEN))
    from testkonten import KONTO_IMAP_1, KONTO_IMAP_2
    soll_buecher = [u for u, (_, nurlesend, _) in vergleich().items() if not nurlesend]
    s = soll_felder(time.strftime('%H%M%S'), (KONTO_IMAP_2, KONTO_IMAP_1))
    fokus_ansicht()
    ab = marke()
    taste('Control_L', 'n')
    m = warte(r'Kontakt-Formular: neu, Bücher((?: \S+)+), Vorgabe (\S+)$', ab, 10)
    if m is None:
        falsch('Strg+N: kein Formular „Neuer Kontakt“')
        return None, None
    buecher = m.group(1).split()
    (ok if sorted(buecher) == sorted(soll_buecher) else falsch)(
        'Formular neu: Buch-Auswahl nur schreibbare Bücher %s (Python-GI nicht nur lesend: %s)'
        % ([b[:8] for b in buecher], [b[:8] for b in soll_buecher]))
    (ok if NURLESEND not in buecher else falsch)('Formular neu: Nur-lesend-Buch %s nicht wählbar' % NURLESEND[:8])
    (ok if m.group(2) == vorgabe else falsch)('Formular neu: Vorgabe = gewähltes Buch %s (Soll %s)' % (m.group(2)[:8], vorgabe[:8]))
    time.sleep(1)
    if ziel != m.group(2):
        k = fokussiert()
        (ok if k is not None and k.get_role_name() == 'combo box' else falsch)(
            'Formular neu: Fokus zuerst auf „Adressbuch“ (%s)' % (k and k.get_name()))
        schritte = buecher.index(ziel) - buecher.index(m.group(2))
        # Die Liste im Popover beginnt oben (gemessen: Pfeil hoch ab der Vorgabe landete auf Zeile 0).
        taste('space')
        time.sleep(0.7)
        taste('Home')
        for _ in range(buecher.index(ziel)):
            taste('Down')
        taste('Return')
        time.sleep(0.5)
    # Vor dem Schreiben: gewähltes Buch im Formular (AT-SPI) = Ziel, sonst abbrechen (kein Schreiben ins falsche Buch).
    konto, buchname = vergleich()[ziel][0].split(' / ', 1)
    soll_label = '%s (%s)' % (buchname, konto)
    combo = finde(wurzel(), 'combo box', 'Adressbuch')
    texte = [k.get_name() for k in knoten(combo)] if combo is not None else []
    if soll_label not in texte:
        falsch('Formular neu: Buch-Auswahl zeigt nicht „%s“ (%s), abgebrochen ohne Schreiben' % (soll_label, texte))
        taste('Escape')
        return None, None
    ok('Formular neu: Buch-Auswahl per Tastatur „%s“ (AT-SPI)' % soll_label)
    if os.environ.get('TROCKEN') == '1':
        taste('Escape')
        return None, None
    feld('Vorname', s['given'])
    feld('Nachname', s['family'])
    if bildname:
        time.sleep(0.5)
        bild(bildname + '-oben', os.path.join(OUT, bildname + '-oben.png'))
    if voll:
        feld('E-Mail', s['emails'][0][0])
        taste('Tab')
        typ_waehlen(1)  # Arbeit → Privat
        k = fokussiert()
        (ok if k is not None and k.get_name() == 'Privat' else falsch)('Formular: E-Mail-Typ per Tastatur „Privat“ (%s)' % (k and k.get_name()))
        knopf('E-Mail hinzufügen')
        schreibe(s['emails'][1][0])
        feld('Telefon', s['telefone'][0][0])
        knopf('Telefonnummer hinzufügen')
        schreibe(s['telefone'][1][0])
        taste('Tab')
        typ_waehlen(1)  # Mobil → Arbeit
        knopf('Adresse hinzufügen')
        schreibe(s['adresse']['street'])
        feld('Postleitzahl', s['adresse']['code'])
        feld('Stadt', s['adresse']['locality'])
        feld('Land', s['adresse']['country'])
        feld('Firma', s['firma'])
        feld('Abteilung', s['abteilung'])
        feld('Position', s['position'])
        feld('Notizen', s['notiz'])
    else:
        s = {'given': s['given'], 'family': s['family'], 'emails': [s['emails'][0]]}
        feld('E-Mail', s['emails'][0][0])
    time.sleep(0.5)
    if bildname:
        bild(bildname, os.path.join(OUT, bildname + '.png'))
    ab = marke()
    if tab_bis('Hinzufügen', 'button'):
        taste('Return')
    m = warte(r'Kontakt: anlegen (\S+) (\S+) fertig', ab, 60)
    fehler_m = warte(r'Kontakt: anlegen \S+ Fehler: (.*)$', ab, 1)
    if m is None:
        falsch('Anlegen: nicht geschrieben (%s)' % (fehler_m and fehler_m.group(1)))
        return None, None
    uid = m.group(2)
    with open(datei, 'w') as f:
        f.write('KONTAKT %s %s %s %s\n' % (m.group(1), uid, s['given'], s['family']))
    print('SCHREIBVORGANG %s' % m.group(1), flush=True)
    (ok if m.group(1) == ziel else falsch)('Anlegen: in Buch %s (im Formular gewählt, Soll %s)' % (m.group(1)[:8], ziel[:8]))
    g = warte(r'Kontakte: neuer Kontakt %s gewählt' % re.escape(uid), ab, 60)
    (ok if g else falsch)('Anlegen: neuer Kontakt kommt über die Live-Ansicht und ist gewählt')
    (ok if warte(r'Kontakt-Detail: %s ' % re.escape(uid), ab, 20) else falsch)('Anlegen: Detail zeigt den neuen Kontakt')
    return uid, s


def aendern(buch, datei, s, extra):
    """Escape bricht ab (nichts geschrieben); dann Telefon und Notiz ändern, „Fertig“; Unbekanntes bleibt."""
    uid = open(datei).read().split()[2]
    if extra:
        ab = marke()
        testdaten('--kontakt-live', 'extra', buch, datei)
        print('SCHREIBVORGANG %s' % buch, flush=True)
        warte(r'Kontakte: %s geändert %s ' % (re.escape(buch), re.escape(uid)), ab, 60)
    ab = marke()
    knopf('Kontakt bearbeiten')
    (ok if warte(r'Kontakt-Formular: bearbeiten %s %s' % (re.escape(buch), re.escape(uid)), ab, 10) else falsch)(
        'Bearbeiten: Formular mit dem Kontakt')
    (ok if text_von('Vorname') == s['given'] and text_von('Nachname') == s['family'] else falsch)(
        'Bearbeiten: Formular zeigt Vorname/Nachname (%s %s)' % (text_von('Vorname'), text_von('Nachname')))
    feld('Vorname', 'Verworfen')
    taste('Escape')
    m = warte(r'Kontakt-Formular: abgebrochen', ab, 5)
    (ok if m and warte(r'Kontakt: ändern ', ab, 2) is None else falsch)('Escape bricht ab, nichts geschrieben')
    ab = marke()
    knopf('Kontakt bearbeiten')
    warte(r'Kontakt-Formular: bearbeiten ', ab, 10)
    s = dict(s)
    s['notiz'] = s['notiz'] + '\nGeändert in Revolution'
    # Zeile mit der Mobilnummer (microsoft365 liefert die Telefone in eigener Reihenfolge).
    alt = s['telefone'][0][0]
    s['telefone'] = [('+49 172 0000370', 'CELL')] + s['telefone'][1:]
    for _ in range(3):
        if not tab_bis('Telefon', 'text'):
            break
        k = fokussiert()
        if Atspi.Text.get_text(k, 0, Atspi.Text.get_character_count(k)) == alt:
            taste('Control_L', 'a')
            schreibe(s['telefone'][0][0])
            break
        taste('Tab')
    if tab_bis('Notizen', 'text'):
        taste('Control_L', 'End')
        schreibe('\nGeändert in Revolution')
    time.sleep(0.5)
    taste('Tab')
    taste('Shift_L', 'Tab')
    bild('formular-bearbeiten', os.path.join(OUT, '%sformular-bearbeiten.png' % BILD_PREFIX))
    ab = marke()
    if tab_bis('Fertig', 'button'):
        taste('Return')
    m = warte(r'Kontakt: ändern (\S+) (\S+) fertig', ab, 60)
    if m is None:
        falsch('Ändern: nicht geschrieben (%s)' % (warte(r'Kontakt: ändern .*$', ab, 1) and warte(r'Kontakt: ändern .*$', ab, 1).group(0)))
        return s
    print('SCHREIBVORGANG %s' % m.group(1), flush=True)
    (ok if warte(r'Kontakte: %s geändert %s ' % (re.escape(buch), re.escape(uid)), ab, 60) else falsch)(
        'Ändern: Änderung kommt über die Live-Ansicht')
    return s


def detail_uid():
    m = re.findall(r'Kontakt-Detail: (\S+) ', log_text())
    return m[-1] if m else None


def loeschen(buch, datei, name, rueckgaengig, per_knopf=False):
    uid = open(datei).read().split()[2]
    liste = kontaktliste()
    eintraege = [n for r, n in zeilen(liste) if r != 'row header'] if liste is not None else []
    if name not in eintraege:
        falsch('Löschen: „%s“ nicht in der Liste %s' % (name, eintraege))
        return
    ab = marke()
    liste.select_child(eintraege.index(name))
    warte(r'Kontakt-Detail: %s ' % re.escape(uid), ab, 10)
    # Sicherheit: gelöscht wird die Auswahl; nur wenn das Detail genau den gespeicherten Testkontakt zeigt.
    if detail_uid() != uid:
        falsch('Löschen: Auswahl %s ist nicht der Testkontakt %s, nichts gelöscht' % (detail_uid(), uid))
        return
    ab = marke()
    if per_knopf:
        knopf('Kontakt löschen')
    else:
        fokus_liste()
        if detail_uid() != uid:
            falsch('Löschen: Auswahl nach dem Fokus %s ist nicht der Testkontakt, nichts gelöscht' % detail_uid())
            return
        taste('Delete')
    m = warte(r'Kontakt: Löschen vorgemerkt %s %s ' % (re.escape(buch), re.escape(uid)), ab, 5)
    (ok if m else falsch)('%s: Löschen vorgemerkt, Toast mit Rückgängig' % ('Knopf „Kontakt löschen“' if per_knopf else 'Entf'))
    (ok if warte_zeile(name, False, 5) else falsch)('Entf: Zeile sofort aus der Liste (AT-SPI)')
    time.sleep(0.5)
    (ok if finde(wurzel(), 'label', 'Kontakt gelöscht') is not None else falsch)('Toast „Kontakt gelöscht“ sichtbar (AT-SPI)')
    if rueckgaengig:
        bild('toast', os.path.join(OUT, '%stoast-geloescht.png' % BILD_PREFIX))
        ab = marke()
        knopf('Rückgängig')
        (ok if warte(r'Kontakt: Löschen rückgängig %s' % re.escape(uid), ab, 5) else falsch)('Rückgängig: Löschen zurückgenommen')
        (ok if warte_zeile(name, True, 5) else falsch)('Rückgängig: Zeile wieder in der Liste')
        time.sleep(LOESCHEN_S + 2)
        (ok if warte(r'Kontakt: löschen ', ab, 1) is None and vorhanden(buch, datei) else falsch)(
            'Rückgängig: nach %d s kein Löschauftrag, Kontakt noch im Buch (Python-GI)' % (LOESCHEN_S + 2))
        return
    t = time.monotonic()
    m = warte(r'Kontakt: löschen %s %s fertig' % (re.escape(buch), re.escape(uid)), ab, 60)
    (ok if m and time.monotonic() - t >= LOESCHEN_S - 1.5 else falsch)(
        'Löschen: nach Ablauf des Toasts geschrieben (nach %.1f s)' % (time.monotonic() - t))
    if m:
        print('SCHREIBVORGANG %s' % buch, flush=True)
    (ok if warte(r'Kontakte: %s entfernt %s$' % (re.escape(buch), re.escape(uid)), ab, 60) else falsch)(
        'Löschen: kommt über die Live-Ansicht')
    (ok if not vorhanden(buch, datei) else falsch)('Löschen: nicht mehr im Buch (Python-GI, eigener Prozess)')
    if buch != LOKAL_BUCH:
        status, _ = server(buch, datei)
        (ok if status == '404' else falsch)('Löschen: am Server ohne EDS HTTP %s (erwartet 404)' % status)


def schreiben():
    soll = vergleich()
    if NURLESEND:
        nurlesend_pruefen(soll)
    else:
        time.sleep(3)  # Bücher verbinden (Buch-Auswahl im Formular nur mit bekanntem „nur lesend“)
    m365 = ZIEL == '3be9d2a4067fd00b72a3d3d5dbd529075a08ea88'
    uid, s = neu_formular(ZIEL, KONTAKT, True, '%sformular-neu' % BILD_PREFIX)
    if uid is None:
        return
    name = '%s %s' % (s['given'], s['family'])
    time.sleep(2)
    f = lesen(ZIEL, KONTAKT)
    wert('Anlegen Python-GI: %s' % json.dumps(f, ensure_ascii=False))
    vergleiche_eds(f, s, 'Anlegen (Python-GI)', m365)
    vergleiche_server(ZIEL, KONTAKT, s, 'Anlegen')
    bild('angelegt')
    s = aendern(ZIEL, KONTAKT, s, not m365)
    time.sleep(2)
    f2 = lesen(ZIEL, KONTAKT)
    wert('Ändern Python-GI: %s' % json.dumps(f2, ensure_ascii=False))
    vergleiche_eds(f2, s, 'Ändern (Python-GI)', m365)
    if not m365:
        (ok if f2.get('spitzname') == 'Revolution-370-Spitzname' else falsch)(
            'Ändern: Feld außerhalb des Formulars (NICKNAME) erhalten: %s' % f2.get('spitzname'))
    vergleiche_server(ZIEL, KONTAKT, s, 'Ändern')
    loeschen(ZIEL, KONTAKT, name, True)
    loeschen(ZIEL, KONTAKT, name, False, per_knopf=True)
    bild('geloescht')
    if LOKAL:
        # „Persönlich“ einmal anlegen und löschen (Buch-Auswahl im Formular).
        uid, s = neu_formular(LOKAL_BUCH, KONTAKT_LOKAL, False, vorgabe=ZIEL)
        if uid is not None:
            (ok if vorhanden(LOKAL_BUCH, KONTAKT_LOKAL) else falsch)('Lokal: Kontakt in „Persönlich“ (Python-GI)')
            warte_zeile('%s %s' % (s['given'], s['family']), True, 60)
            time.sleep(1)
            loeschen(LOKAL_BUCH, KONTAKT_LOKAL, '%s %s' % (s['given'], s['family']), False)


def absender_menue():
    ab = marke()
    # Menü „Mail ▾“ der Nachricht (mail-view.blp message_menu) über AT-SPI, Eintrag per Aktion.
    menue = [k for k in knoten(wurzel()) if k.get_role_name() == 'toggle button' and k.get_name() == 'Weitere Aktionen']
    if not menue:
        falsch('Absender: Knopf „Weitere Aktionen“ (Menü der Nachricht) fehlt')
        return
    menue[0].get_action_iface().do_action(0)
    time.sleep(1)
    alle = list(knoten(wurzel()))
    # Menüeinträge (GtkPopoverMenu) melden keinen Namen; der Text steht im Label darunter.
    eintrag = [k for k in alle if k.get_role_name() == 'menu item' and k.get_action_iface() is not None and
               any('Kontakten hinzufügen' in (x.get_name() or '') for x in knoten(k))]
    if not eintrag:
        # GTK 4.22 meldet die Einträge ohne Namen und ohne Label-Kinder: letzter Eintrag des Menüs (mail-view.blp
        # message_menu: eigener letzter Abschnitt); ob es der richtige war, belegt die Log-Zeile der Aktion.
        eintrag = [k for k in alle if k.get_role_name() == 'menu item' and k.get_action_iface() is not None][-1:]
    bild('absender-menue')
    if not eintrag:
        falsch('Absender: Menüeintrag „Absender zu Kontakten hinzufügen“ fehlt (Menü: %s)' % [
            (k.get_role_name(), k.get_name()) for k in alle if 'menu' in k.get_role_name()][:20])
        taste('Escape')
        return
    eintrag[0].get_action_iface().do_action(0)
    return ab


def absender():
    """Mail-Ansicht: „Weitere Aktionen“ → „Absender zu Kontakten hinzufügen“ → Kontakte mit vorausgefülltem Formular;
    Escape (nichts geschrieben); dann mit Nachname „Revolution-Test Absender …“ speichern, ein zweites Mal übernehmen
    (Adresse schon im Buch: Formular bearbeitet den vorhandenen Kontakt wie Evolution, Hinweis), Escape, löschen."""
    ab = absender_menue()
    if ab is None:
        return
    m = warte(r'Mail: Absender als Kontakt (.*)$', ab, 10)
    if m is None:
        falsch('Absender: Aktion nicht ausgelöst')
        return
    von = m.group(1)
    wert('Absender der Testmail: %s' % von)
    m = warte(r'Kontakt: Absender übernehmen „(.*)“ <(\S+)>, Buch (\S+)$', ab, 10)
    f = warte(r'Kontakt-Formular: (neu|bearbeiten)[ ,]', ab, 60)
    (ok if m and f else falsch)('Absender: Kontakte mit Formular %s (Buch %s)' % (f and f.group(1), m and m.group(3)))
    if not (m and f):
        return
    time.sleep(1)
    name, adresse = m.group(1), m.group(2)
    (ok if adresse.lower() in von.lower() else falsch)('Absender: Adresse %s aus „%s“' % (adresse, von))
    if f.group(1) == 'neu':
        teile = name.rsplit(' ', 1) if ' ' in name else ['', name]
        ist = (text_von('Vorname'), text_von('Nachname'), text_von('E-Mail'))
        (ok if ist[2] == adresse and (ist[0] + ' ' + ist[1]).strip() == name.strip() else falsch)(
            'Absender: Formular vorausgefüllt Vorname „%s“, Nachname „%s“, E-Mail „%s“' % ist)
        wert('Absender: Name aufgeteilt wie EDS e_contact_name_from_string, erwartet etwa %s' % teile)
    else:
        wert('Absender: Adresse schon im Buch, Formular bearbeitet den vorhandenen Kontakt (wie Evolution)')
    bild('absender-formular', os.path.join(OUT, 'absender-formular.png'))
    ab = marke()
    taste('Escape')
    (ok if warte(r'Kontakt-Formular: abgebrochen', ab, 5) and warte(r'Kontakt: (anlegen|ändern) ', ab, 2) is None else falsch)(
        'Absender: Escape bricht ab, nichts geschrieben')
    if f.group(1) != 'neu':
        return
    # Zweiter Durchgang: als Testkontakt speichern, dann ist die Adresse im Buch.
    taste('Control_L', '1')
    time.sleep(1)
    ab = absender_menue()
    if ab is None or not warte(r'Kontakt-Formular: neu[ ,]', ab, 30):
        falsch('Absender: Formular beim zweiten Mal nicht geöffnet')
        return
    time.sleep(1)
    familie = 'Revolution-Test Absender %s' % time.strftime('%H%M%S')
    feld('Nachname', familie)
    ab = marke()
    if tab_bis('Hinzufügen', 'button'):
        taste('Return')
    m = warte(r'Kontakt: anlegen (\S+) (\S+) fertig', ab, 60)
    if m is None:
        falsch('Absender: Testkontakt nicht gespeichert')
        return
    buch, uid = m.group(1), m.group(2)
    with open(KONTAKT, 'w') as fd:
        fd.write('KONTAKT %s %s %s %s\n' % (buch, uid, ist[0], familie))
    print('SCHREIBVORGANG %s' % buch, flush=True)
    warte(r'Kontakte: neuer Kontakt %s gewählt' % re.escape(uid), ab, 60)
    f2 = lesen(buch, KONTAKT)
    (ok if f2['family'] == familie and [e['wert'] for e in f2['emails']] == [adresse] else falsch)(
        'Absender: als Testkontakt gespeichert, E-Mail %s (Python-GI)' % [e['wert'] for e in f2['emails']])
    taste('Control_L', '1')
    time.sleep(1)
    ab = absender_menue()
    m = warte(r'Kontakt: Absender %s schon im Buch (\S+) als (\S+)$' % re.escape(adresse), ab, 30) if ab else None
    (ok if m and m.group(2) == uid and warte(r'Kontakt-Formular: bearbeiten \S+ %s' % re.escape(uid), ab, 10) else falsch)(
        'Absender: Adresse schon im Buch → Formular bearbeitet den vorhandenen Kontakt %s (wie Evolution)' % (m and m.group(2)))
    time.sleep(0.5)
    (ok if finde(wurzel(), 'label', 'Kontakt ist schon vorhanden') is not None else falsch)('Absender: Hinweis „Kontakt ist schon vorhanden“')
    bild('absender-vorhanden', os.path.join(OUT, 'absender-vorhanden.png'))
    ab = marke()
    taste('Escape')
    (ok if warte(r'Kontakt-Formular: abgebrochen', ab, 5) else falsch)('Absender: Escape, nichts geschrieben')
    time.sleep(1)
    loeschen(buch, KONTAKT, '%s %s' % (ist[0], familie), False, per_knopf=True)


def pruefen():
    soll = vergleich()
    ab = 0
    if not fenster():
        falsch('kein Fenster')
        return
    # Zeiger ins Fenster: ohne Fenstermanager hat das Fenster unter dem Zeiger den Tastaturfokus.
    xtest.fake_input(d, X.MotionNotify, x=900, y=500)
    d.sync()
    time.sleep(2)
    if MODUS == 'absender':
        # #370: Mail-Ansicht mit REVOLUTION_FOLDER/REVOLUTION_MESSAGE (Testmail gewählt).
        warte(r'Nachricht: zeigt ', 0, 90)
        time.sleep(3)
        absender()
        andere = [z for z in log_text().splitlines() if z.startswith('(revolution:') and 'revolution-DEBUG' not in z]
        (ok if not andere else falsch)('keine Warnungen/Criticals im Log%s' % ('' if not andere else ': %s' % andere[:3]))
        return
    taste('Control_L', '3')
    n0 = fertig(BUCH, ab)
    if n0 is None:
        falsch('Buch %s nicht geladen in %d s' % (BUCH, TIMEOUT))
        return
    wert('Buch %s: %d Kontakte beim Start' % (BUCH, n0))
    m = re.search(r'Kontakte: %s neu \d+, geändert \d+, Liste \d+ \(UI-Thread (ja|nein)\)' % re.escape(BUCH), log_text())
    (ok if m is None or m.group(1) == 'ja' else falsch)('Signale der EBookClientView im UI-Thread (%s)' % (m and m.group(1)))
    time.sleep(1)
    bild('start')
    if MODUS == 'alle':
        log = seitenleiste(soll)
        gezaehlt = zaehlen(soll, log)
        pos = [p for p, u, _ in log if u == BUCH]
        if log and log[-1][1] == BUCH:
            n0 = gezaehlt.get(BUCH)  # zuletzt gezählt, noch gewählt
        else:
            ab = buch_waehlen(pos[0], BUCH) if pos else None
            n0 = fertig(BUCH, ab) if ab is not None else None
        if n0 is None:
            falsch('zurück zu %s: nicht geladen' % BUCH)
            return
    if MODUS == 'schreiben':
        schreiben()
    elif MODUS != 'detail':
        live(n0)
    if GRAPH:
        graph_probe()
    if DETAIL:
        detail()
    andere = [z for z in log_text().splitlines() if z.startswith('(revolution:') and 'revolution-DEBUG' not in z]
    (ok if not andere else falsch)('keine Warnungen/Criticals im Log%s' % ('' if not andere else ': %s' % andere[:3]))


def main():
    global app
    log = open(LOG, 'w')
    app = subprocess.Popen([BIN], stdout=log, stderr=subprocess.STDOUT)
    try:
        pruefen()
        w = fenster()
        rc = schliessen(w) if w else None
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
