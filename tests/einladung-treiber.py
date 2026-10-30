#!/usr/bin/env python3
# einladung-treiber.py – Treiber für tests/einladung.sh (#376), läuft im Xvfb aus xvfb-run.
#
# Startet die App (BIN) mit dem Konto der Mail (übrige Konten per REVOLUTION_SKIP_ACCOUNTS aus), REVOLUTION_FOLDER und
# REVOLUTION_MESSAGE wählen die Mail. Prüft die Einladungskarte über AT-SPI (Titel, Zeit, Ort, Organisator, Knöpfe,
# a11y-Namen), wählt den Kalender KALENDER (Anzeigename KALENDER_NAME) in der Kalenderwahl und löst ANTWORT aus:
#   annehmen | vorlaeufig | ablehnen   (METHOD REQUEST)   entfernen (CANCEL)   status (REPLY)   keine (nur Bilder)
# TASTE=1: über die Zugriffstaste (Alt+Buchstabe) statt per Klick. FEHLER_ERWARTET=1: Schreiben scheitert, Banner prüfen. Erwartet MAIL=ja|nein: ob Revolution die Antwort
# selbst über den Postausgang schickt (dann bis zum Log „Senden: … gesendet“ warten).
# Ausgabe: „OK …“, „FEHLER …“, „WERT …“; Bilder OUT/NAME-<Schritt>.png.
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
ANTWORT = os.environ['ANTWORT']
KALENDER, KALENDER_NAME = os.environ.get('KALENDER', ''), os.environ.get('KALENDER_NAME', '')
TITEL = os.environ['TITEL']
METHODE = os.environ['METHODE']
MAIL = os.environ.get('MAIL', 'nein')
TASTE = os.environ.get('TASTE') == '1'
FEHLER_ERWARTET = os.environ.get('FEHLER_ERWARTET') == '1'
# #449: Bannertext bei Googles versteckter Kopie (po/de.po).
GOOGLE_TEXT = ('Google hat diese Einladung bereits in Ihren Google Kalender übernommen. '
               'Bitte antworten Sie dort.')
LOG = os.path.join(OUT, NAME + '.log')
VERSATZ = int(os.environ.get('VERSATZ', '15'))
# Knöpfe (deutsch, abbruch.sh setzt LC_ALL=de_DE.UTF-8) mit Zugriffstaste aus po/de.po.
KNOEPFE = {'annehmen': ('Annehmen', 'a'), 'vorlaeufig': ('Vorläufig', 'v'), 'ablehnen': ('Ablehnen', 'l'),
           'entfernen': ('Aus Kalender entfernen', 'e'), 'status': ('Teilnehmerstatus aktualisieren', 'a')}
ERWARTET = {'REQUEST': ('Ablehnen', 'Vorläufig', 'Annehmen'), 'CANCEL': ('Aus Kalender entfernen',),
            'REPLY': ('Teilnehmerstatus aktualisieren',)}

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


def warte(muster, ab, zeit=120):
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        m = re.search(muster, log_text()[ab:], re.M)
        if m:
            return m
        time.sleep(0.2)
    return None


def taste(*namen):
    codes = [d.keysym_to_keycode(XK.string_to_keysym(n)) for n in namen]
    for c in codes:
        xtest.fake_input(d, X.KeyPress, c)
    for c in reversed(codes):
        xtest.fake_input(d, X.KeyRelease, c)
    d.sync()
    time.sleep(0.4)


def klick(x, y):
    xtest.fake_input(d, X.MotionNotify, x=x, y=y)
    xtest.fake_input(d, X.ButtonPress, 1)
    xtest.fake_input(d, X.ButtonRelease, 1)
    d.sync()
    time.sleep(0.6)


def bild(schritt):
    xtest.fake_input(d, X.MotionNotify, x=1, y=1)
    d.sync()
    time.sleep(0.5)
    root = d.screen().root
    g = root.get_geometry()
    pfad = os.path.join(OUT, '%s-%s.png' % (NAME, schritt))
    Image.frombytes('RGB', (g.width, g.height), root.get_image(0, 0, g.width, g.height, X.ZPixmap,
                                                               0xffffffff).data, 'raw', 'BGRX').save(pfad)
    wert('Bild %s' % pfad)


def fenster(titel='Revolution', zeit=30):
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


def alle(o, rolle, out=None):
    out = [] if out is None else out
    try:
        if o.get_role_name() == rolle and o.get_state_set().contains(Atspi.StateType.SHOWING):
            out.append(o)
        for i in range(o.get_child_count()):
            k = o.get_child_at_index(i)
            if k is not None:
                alle(k, rolle, out)
    except Exception:
        pass
    return out


def mitte(o):
    r = o.get_component_iface().get_extents(Atspi.CoordType.WINDOW)
    return r.x + r.width // 2 + VERSATZ, r.y + r.height // 2 + VERSATZ


def warte_obj(fn, zeit=20):
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        o = fn()
        if o is not None:
            return o
        time.sleep(0.3)
    return None


def texte(o):
    return [x.get_name() for x in alle(o, 'label')]


def kalender_waehlen(karte):
    """Kalenderwahl (GtkDropDown, a11y-Name „Kalender:“) per Maus öffnen und den Eintrag KALENDER_NAME wählen."""
    combo = finde(karte, 'combo box', 'Kalender:')
    if combo is None:
        falsch('Kalenderwahl „Kalender:“ nicht gefunden')
        return False
    ok('a11y: Kalenderwahl „Kalender:“ (labelled-by), Auswahl %s' % texte(combo)[:1])
    if texte(combo)[:1] == [KALENDER_NAME]:
        return True
    # Wie tests/kalender-treiber.py kombi_waehlen: Popover per Maus öffnen, Eintrag per Tastatur (Pos1, Ab, Eingabe).
    klick(*mitte(combo))
    time.sleep(1)
    sp = finde(combo, 'scroll pane', None)
    eintraege = []
    for lab in alle(sp, 'label') if sp else []:
        p = lab.get_parent()
        for _ in range(5):
            if p is None or p.get_role_name() == 'list item':
                break
            p = p.get_parent()
        if p is not None:
            eintraege.append(lab.get_name())
    wert('Kalenderwahl: %d Einträge' % len(eintraege))
    if KALENDER_NAME not in eintraege:
        falsch('Kalender „%s“ nicht in der Kalenderwahl' % KALENDER_NAME)
        taste('Escape')
        return False
    taste('Home')
    for _ in range(eintraege.index(KALENDER_NAME)):
        taste('Down')
    taste('Return')
    time.sleep(0.5)
    (ok if texte(combo)[:1] == [KALENDER_NAME] else falsch)('Kalender gewählt: %s' % texte(combo)[:1])
    return texte(combo)[:1] == [KALENDER_NAME]


def lauf():
    m = warte(r'Einladung: METHOD (\S+), UID (\S+), (\d+) schreibbare Kalender, vorhanden in (\S+), Status (-?\d+), '
              r'Konflikte (\S+)', 0, 180)
    if m is None:
        falsch('Einladungskarte: Log „Einladung: METHOD …“ fehlt')
        bild('fehler-keine-karte')
        return
    (ok if m.group(1) == METHODE else falsch)('Kalenderteil erkannt: %s' % m.group(0))
    karte = warte_obj(lambda: finde(wurzel(), None, 'Einladung'), 20)
    if karte is None:
        falsch('Karte (a11y „Einladung“) nicht sichtbar')
        bild('fehler-karte')
        return
    time.sleep(1.5)
    t = texte(karte)
    wert('Karte: %s' % t)
    # Exchange setzt in REPLY und CANCEL ein Präfix vor SUMMARY („Angenommen: …“, „Abgesagt: …“).
    titel = next((x for x in t if TITEL in (x or '')), None)
    (ok if titel else falsch)('Karte zeigt Titel „%s“' % titel)
    for feld in ('Zeit:', 'Ort:', 'Organisator:'):
        (ok if feld in t else falsch)('Karte zeigt „%s“' % feld)
    knoepfe = [b.get_name() for b in alle(karte, 'button')]
    soll = ERWARTET[METHODE]
    (ok if all(k in knoepfe for k in soll) else falsch)('a11y: Knöpfe %s (soll %s)' % (knoepfe, list(soll)))
    bild('1-karte')
    if ANTWORT == 'keine':
        return
    if METHODE == 'REQUEST' and not kalender_waehlen(karte):
        return
    name, zugriff = KNOEPFE[ANTWORT]
    knopf = finde(karte, 'button', name)
    if knopf is None:
        falsch('Knopf „%s“ fehlt' % name)
        return
    ab = marke()
    if TASTE:
        # Fokus in die Karte (Titel ist wählbar), dann Alt+Zugriffstaste wie in po/de.po.
        taste('Alt_L', zugriff)
        wert('Zugriffstaste Alt+%s für „%s“' % (zugriff.upper(), name))
    else:
        klick(*mitte(knopf))
    m = warte(r'Einladung: Antwort (\S+), Kalender (\S+)', ab, 20)
    if m is None:
        falsch('Antwort „%s“ nicht ausgelöst (%s)' % (name, 'Taste' if TASTE else 'Klick'))
        bild('fehler-antwort')
        return
    (ok if not KALENDER or m.group(2) == KALENDER else falsch)('Antwort %s in Kalender %s (soll %s) per %s' % (
        m.group(1), m.group(2)[:8], KALENDER[:8] or '-', 'Zugriffstaste' if TASTE else 'Klick'))
    print('SCHREIBVORGANG %s (App: %s)' % (m.group(2), ANTWORT), flush=True)
    if FEHLER_ERWARTET:
        fehler_erwartet(karte, ab)
        return
    m = warte(r'^.*Einladung: ((Sent to|In Kalender|Teilnehmerstatus|Attendee).*)$', ab, 120)
    w = warte(r'revolution-WARNING.*$', ab, 1)
    if m is None:
        falsch('kein Ergebnis im Log%s' % (': %s' % w.group(0) if w else ''))
        bild('fehler-ergebnis')
        return
    ok('Kalender geschrieben: %s' % m.group(1))
    time.sleep(1)
    t = texte(karte)
    (ok if m.group(1) in t else falsch)('Karte zeigt Stand „%s“' % m.group(1))
    bild('2-nach-antwort')
    if METHODE == 'REQUEST':
        p = warte(r'Einladung: Antwort \d+ in \S+ \(vorher vorhanden (\S+), Server plant (\S+)\), Antwort per Mail (\S+)',
                  ab, 10)
        (ok if p and p.group(3) == MAIL else falsch)('Antwort per Mail %s (soll %s): %s' % (
            p and p.group(3), MAIL, p and p.group(0)))
        if MAIL == 'ja':
            s = warte(r'Senden: (\S+) gesendet nach', ab, 180)
            (ok if s else falsch)('Antwort über den Postausgang gesendet: %s' % (s and s.group(0)))
            if s:
                print('ANTWORT-MESSAGE-ID %s' % s.group(1), flush=True)


def fehler_erwartet(karte, ab):
    """#376 R2: Schreibfehler (Google HTTP 400, #447): Warnung im Log, Banner per AT-SPI sichtbar, kein Stand auf der
    Karte, keine Antwort im Postausgang. #449: erkannt (Fehlerbereich und HTTP-Status im Log), klarer Bannertext."""
    g = warte(r'Einladung: Schreibfehler (\S+) (\d+), Google (\S+)$', ab, 120)
    (ok if g and g.groups() == ('e-soup-session-error-quark', '400', 'ja') else falsch)(
        'Schreibfehler erkannt: %s' % (g and g.group(0)))
    w = warte(r'revolution-WARNING \*\*: [\d:.]+: (%s): (.*)$' % re.escape(GOOGLE_TEXT), ab, 120)
    if w is None:
        falsch('erwarteter Schreibfehler nicht im Log')
        bild('fehler-kein-fehler')
        return
    ok('Schreibfehler im Log: %s: %s' % (w.group(1), w.group(2)[:120]))
    banner = warte_obj(lambda: finde(wurzel(), 'label', w.group(1)), 10)
    (ok if banner is not None else falsch)('Banner „%s“ per AT-SPI sichtbar' % w.group(1))
    time.sleep(1)
    bild('2-banner')
    (ok if not any('In Kalender' in x for x in texte(karte)) else falsch)('Karte zeigt keinen Stand „In Kalender …“')
    time.sleep(5)
    post = warte(r'(Postausgang: \S+ abgelegt|Senden: \S+ gesendet)', ab, 1)
    (ok if post is None else falsch)('keine Antwort im Postausgang%s' % ('' if post is None else ': %s' % post.group(0)))


def main():
    global app
    log = open(LOG, 'w')
    app = subprocess.Popen([BIN], stdout=log, stderr=subprocess.STDOUT)
    try:
        w = fenster()
        if w is None:
            falsch('kein Fenster')
            return 1
        lauf()
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
    andere = [z for z in log_text().splitlines()
              if z.strip() and 'revolution-DEBUG' not in z and not z.startswith('libEGL warning')
              and not (FEHLER_ERWARTET and 'revolution-WARNING' in z and GOOGLE_TEXT in z)]
    (ok if not andere else falsch)('keine Warnungen/Criticals im Log%s' % ('' if not andere else ': %s' % andere[:3]))
    return 1 if fehler else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as e:  # #411: Abbruch zählt als FEHLER
        traceback.print_exc()
        falsch('Abbruch: %s: %s' % (type(e).__name__, str(e).replace('\n', ' ')[:300]))
        sys.exit(1)
