#!/usr/bin/env python3
# benachrichtigung-treiber.py – Treiber für tests/benachrichtigung.sh (#366), läuft im Xvfb aus xvfb-run.
#
# Startet die App (BIN) mit Log und dbus-monitor (org.freedesktop.Notifications, org.gtk.Notifications,
# Fehlerantworten), legt Testmails über tests/testdaten.py --benachrichtigung an (Zeilen „NEU …“ nach NEU),
# bedient die App über org.gtk.Actions am Session-Bus, XTest (python3-xlib) und AT-SPI.
# Ausgabe: „OK …“, „FEHLER …“, „WERT …“; Screenshots OUT/NAME-<Schritt>.png, Bus-Mitschnitt OUT/NAME-dbus.txt.
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
import urllib.parse

import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi  # noqa: E402

from PIL import Image  # noqa: E402
from Xlib import X, XK, display, protocol  # noqa: E402
from Xlib.ext import xtest  # noqa: E402

BIN, OUT, NAME, WAS, APP_DIR, APP_ID = (os.environ[k] for k in ('BIN', 'OUT', 'NAME', 'WAS', 'APP_DIR', 'APP_ID'))
KONTO, GEOPS, OUTLOOK, XDG, NEU = (os.environ[k] for k in ('KONTO', 'GEOPS', 'OUTLOOK', 'XDG', 'NEU'))
APP_PFAD = '/' + APP_ID.replace('.', '/')
MAILTO_AN, MAILTO_CC, MAILTO_BCC = 'KONTO_IMAP_2@example.invalid', 'KONTO_IMAP_1@example.invalid', 'KONTO_GOOGLE@example.invalid'
MAILTO_BETREFF = '[Revolution-Test] mailto Ä ö & Test'
MAILTO_TEXT = 'Zeile 1\nZeile 2 mit https://example.org'

fehler = 0
d = display.Display()
app_pid = None


def ok(text):
    print('OK      %s: %s' % (NAME, text), flush=True)


def falsch(text):
    global fehler
    fehler += 1
    print('FEHLER  %s: %s' % (NAME, text), flush=True)


def wert(text):
    print('WERT    %s: %s' % (NAME, text), flush=True)


def pruefe(bedingung, text):
    (ok if bedingung else falsch)(text)
    return bedingung


# ------------------------------------------------------------------ Log der App

LOG = os.path.join(OUT, NAME + '.log')
ZEILE_RE = re.compile(r'^\(revolution:\d+\): revolution-DEBUG: [\d:.]+: (.*)$')


def log_zeilen():
    with open(LOG, encoding='utf-8', errors='replace') as f:
        return [m.group(1) for z in f for m in [ZEILE_RE.match(z.rstrip('\n'))] if m]


def marke():
    return len(log_zeilen())


def warte(muster, ab, zeit=60):
    rx = re.compile(muster)
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        for text in log_zeilen()[ab:]:
            if rx.search(text):
                return text
        time.sleep(0.2)
    return None


# ------------------------------------------------------------------ Bus

DBUS = os.path.join(OUT, NAME + '-dbus.txt')
STRING_RE = re.compile(r'string "((?:[^"\\]|\\.)*)"', re.S)


def dbus_bloecke():
    """Nachrichten aus dem Mitschnitt von dbus-monitor (je Kopfzeile ein Block)."""
    with open(DBUS, encoding='utf-8', errors='replace') as f:
        text = f.read()
    return [b for b in re.split(r'\n(?=(?:method call|method return|error|signal) )', text) if b.strip()]


def notify_aufrufe():
    """Benachrichtigungen der App an den Dienst, je Aufruf (ersetzt, Titel, Text, Ziel):
    org.gtk.Notifications.AddNotification (GLib-Standard unter GNOME): ersetzt = gleiche ID „new-mail“, Ziel =
    default-action-target; org.freedesktop.Notifications.Notify (GNOTIFICATION_BACKEND=freedesktop): ersetzt =
    replaces_id ≠ 0, Ziel nicht auf dem Bus. Nur der Aufruf der App (die Shell reicht Notify weiter: zweiter Aufruf,
    anderer Absender und Ziel)."""
    out = []
    for b in dbus_bloecke():
        kopf = b.split('\n')[0]
        if 'member=AddNotification' in kopf and 'destination=org.gtk.Notifications ' in kopf:
            s_ = STRING_RE.findall(b)
            if s_[0] != APP_ID:
                continue
            feld = {k: re.search(r'string "%s"\s+variant\s+string "((?:[^"\\]|\\.)*)"' % k, b, re.S) for k in ('title', 'body')}
            ziel = re.search(r'string "default-action-target"\s+variant\s+struct \{\s+string "(.*?)"\s+string "(.*?)"', b, re.S)
            out.append((s_[1] == 'new-mail', feld['title'] and feld['title'].group(1), feld['body'] and feld['body'].group(1),
                        ziel and ziel.groups()))
        elif 'member=Notify' in kopf and 'destination=org.freedesktop.Notifications ' in kopf and \
                re.search(r'string "desktop-entry"\s+variant\s+string "%s"' % re.escape(APP_ID), b):
            s_ = STRING_RE.findall(b)
            out.append((int(re.search(r'uint32 (\d+)', b).group(1)) != 0, s_[2], s_[3], None))
    return out


def zurueckgezogen():
    """Aufrufe CloseNotification (freedesktop) bzw. RemoveNotification „new-mail“ der App (gtk)."""
    out = []
    for b in dbus_bloecke():
        kopf = b.split('\n')[0]
        if ('member=CloseNotification' in kopf and 'destination=org.freedesktop.Notifications ' in kopf) or \
                ('member=RemoveNotification' in kopf and 'destination=org.gtk.Notifications ' in kopf and
                 STRING_RE.findall(b)[:2] == [APP_ID, 'new-mail']):
            out.append(kopf)
    return out


def aktion(name, *parameter):
    """Aktion der App über org.gtk.Actions.Activate (wie der Klick in einer Benachrichtigung über GApplication)."""
    arg = '[%s]' % ', '.join('<%s>' % p for p in parameter)
    r = subprocess.run(['gdbus', 'call', '--session', '-d', APP_ID, '-o', APP_PFAD, '-m', 'org.gtk.Actions.Activate',
                        name, arg, '{}'], capture_output=True, text=True, timeout=30)
    pruefe(r.returncode == 0, 'Aktion %s %s: %s' % (name, arg, (r.stdout + r.stderr).strip()))


def gvariant_str(s):
    return "'%s'" % s.replace('\\', '\\\\').replace("'", "\\'")


# ------------------------------------------------------------------ X11

def zeiger(p):
    xtest.fake_input(d, X.MotionNotify, x=p[0], y=p[1])
    d.sync()
    time.sleep(0.2)


def taste(kombi, pause=0.4):
    codes = [d.keysym_to_keycode(XK.string_to_keysym(k)) for k in kombi.split('+')]
    for c in codes:
        xtest.fake_input(d, X.KeyPress, c)
    for c in reversed(codes):
        xtest.fake_input(d, X.KeyRelease, c)
    d.sync()
    time.sleep(pause)


def fenster(titel, weg=False, zeit=20):
    """Sichtbares Fenster mit diesem Titel (bzw. jedes andere als „Revolution“ bei titel None); weg=True: warten, bis
    keins mehr sichtbar ist."""
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        gefunden = None
        for w in d.screen().root.query_tree().children:
            try:
                name = w.get_wm_name()
                if name and w.get_attributes().map_state == X.IsViewable and \
                        (name == titel if titel is not None else name != 'Revolution'):
                    gefunden = w
            except Exception:
                pass
        if (gefunden is None) == weg:
            return gefunden if not weg else True
        time.sleep(0.2)
    return None


def schliessen(w):
    w.send_event(protocol.event.ClientMessage(
        window=w, client_type=d.intern_atom('WM_PROTOCOLS'),
        data=(32, [d.intern_atom('WM_DELETE_WINDOW'), X.CurrentTime, 0, 0, 0])), event_mask=X.NoEventMask)
    d.sync()


def bild(schritt, w):
    zeiger((1199, 759))
    time.sleep(0.5)
    g = w.get_geometry()
    root = d.screen().root
    s = root.get_geometry()
    raw = root.get_image(0, 0, s.width, s.height, X.ZPixmap, 0xffffffff).data
    pfad = os.path.join(OUT, '%s-%s.png' % (NAME, schritt))
    Image.frombytes('RGB', (s.width, s.height), raw, 'raw', 'BGRX').crop(
        (g.x, g.y, g.x + g.width, g.y + g.height)).save(pfad)
    wert('Screenshot %s' % pfad)


# ------------------------------------------------------------------ AT-SPI

def knoten(o, tiefe=0):
    if o is None or tiefe > 40:
        return
    yield o
    for i in range(o.get_child_count()):
        yield from knoten(o.get_child_at_index(i), tiefe + 1)


def app_fenster(titel):
    desktop = Atspi.get_desktop(0)
    for i in range(desktop.get_child_count()):
        a = desktop.get_child_at_index(i)
        if a is None or a.get_process_id() != app_pid:
            continue
        for j in range(a.get_child_count()):
            f = a.get_child_at_index(j)
            if f is not None and (titel is None or f.get_name() == titel):
                return f
    return None


def text_von(o):
    """Text eines Elements oder seines ersten Nachfahren mit Text-Schnittstelle."""
    for k in knoten(o):
        t = k.get_text_iface()
        if t is not None:
            return Atspi.Text.get_text(t, 0, Atspi.Text.get_character_count(t))
    return None


def dokument_text(o):
    """Text im Editor (WebKit, Rolle „document web“): Text aller Elemente ohne Objekt-Platzhalter (U+FFFC)."""
    teile = []
    for k in knoten(o):
        t = k.get_text_iface()
        if t is not None:
            teile.append(Atspi.Text.get_text(t, 0, Atspi.Text.get_character_count(t)).replace('\ufffc', ''))
    return '\n'.join(t for t in teile if t.strip())


# ------------------------------------------------------------------ Testdaten

def testmails(namen):
    r = subprocess.run(['timeout', '-k', '10', os.environ.get('TD_TIMEOUT', '300'), 'python3',
                        os.path.join(APP_DIR, 'tests', 'testdaten.py'), '--benachrichtigung', KONTO, 'neu', namen],
                       capture_output=True, text=True)
    zeilen = [z for z in r.stdout.splitlines() if z.startswith('NEU ')]
    with open(NEU, 'a', encoding='utf-8') as f:
        for z in zeilen:
            f.write(z + '\n')
    if r.returncode != 0 or len(zeilen) != len(namen.split(',')):
        raise RuntimeError('tests/testdaten.py --benachrichtigung neu %s: Exit %d %s' % (namen, r.returncode,
                                                                                       r.stderr.strip()[-300:]))
    for z in zeilen:
        wert('Testmail %s' % z)
    return {z.split(' ', 3)[3].split(' ')[2]: (z.split(' ')[2], z.split(' ', 3)[3]) for z in zeilen}


# ------------------------------------------------------------------ Läufe

def benachrichtigt(ab, anzahl_vorher, zeit=90):
    """Warten auf die nächste Benachrichtigung im Log und im Bus-Mitschnitt."""
    zeile = warte(r'^Benachrichtigung: \d+ neu in INBOX, zusammen (\d+), Ziel ', ab, zeit)
    ende = time.monotonic() + 10
    while zeile is not None and len(notify_aufrufe()) <= anzahl_vorher and time.monotonic() < ende:
        time.sleep(0.2)
    return zeile


def haupt():
    folder_uri = 'folder://%s/INBOX' % urllib.parse.quote(GEOPS, safe='')
    if warte(r'^Posteingang: %s INBOX aktualisiert' % GEOPS, 0, 120) is None:
        falsch('Posteingang von geops@ nicht abgeglichen')
        return
    for uid in (GEOPS, OUTLOOK):
        z = warte(r'^Abruf: %s alle \d+ Minuten' % uid, 0, 5)
        pruefe(z is not None, 'Abruf-Zeitgeber: %s' % z)
    time.sleep(2)

    # 1) eine ungelesene und eine gelesene Mail
    ab, n0 = marke(), len(notify_aufrufe())
    mails = testmails('eins,gelesen')
    zeile = benachrichtigt(ab, n0)
    pruefe(zeile is not None, 'neue Mail: %s' % zeile)
    time.sleep(3)
    aufrufe = notify_aufrufe()
    wert('Notify: %s' % aufrufe[n0:])
    pruefe(len(aufrufe) == n0 + 1, 'genau eine Benachrichtigung der App %s am Bus: %d' % (APP_ID, len(aufrufe) - n0))
    if len(aufrufe) > n0:
        _, titel, text, _ = aufrufe[n0]
        pruefe(titel == 'Revolution Test' and text == mails['eins'][1],
               'Titel = Absender, Text = Betreff: „%s“ / „%s“' % (titel, text))
    pruefe(not any(mails['gelesen'][1] in a[2] for a in aufrufe), 'keine Benachrichtigung für die gelesene Mail')
    pruefe(warte(r'zusammen 1, Ziel %s %s$' % (re.escape(folder_uri), mails['eins'][0]), ab, 1) is not None,
           'Ziel der Aktion: %s UID %s' % (folder_uri, mails['eins'][0]))

    # 1b) „eins“ am Server gelesen: ungelesene weniger → Benachrichtigung zurückgezogen, Zähler zurück
    # (Evolution unread_notify_status, mail-notification.c:644-647).
    ab, z0 = marke(), len(zurueckgezogen())
    r = subprocess.run(['timeout', '-k', '10', '120', 'python3', os.path.join(APP_DIR, 'tests', 'testdaten.py'),
                        '--benachrichtigung', KONTO, 'gelesen', mails['eins'][0]], capture_output=True, text=True)
    wert('gelesen: %s' % r.stdout.strip())
    if r.returncode != 0:
        raise RuntimeError('tests/testdaten.py --benachrichtigung gelesen: Exit %d %s' % (r.returncode, r.stderr.strip()[-300:]))
    z = warte(r'^Benachrichtigung: INBOX ungelesen \d+ statt \d+, zurückgezogen', ab, 90)
    time.sleep(2)
    pruefe(z is not None and len(zurueckgezogen()) > z0, 'am Server gelesen → zurückgezogen: %s' % z)

    # 2) zwei weitere: dieselbe Benachrichtigung, „(und 1 weitere)“ (Zähler nach dem Zurückziehen neu)
    ab, n1 = marke(), len(notify_aufrufe())
    mails.update(testmails('zwei,drei'))
    ende = time.monotonic() + 90
    while warte(r'zusammen 2, Ziel ', ab, 1) is None and time.monotonic() < ende:
        pass
    time.sleep(3)
    aufrufe = notify_aufrufe()
    wert('Notify: %s' % aufrufe[n1:])
    letzte = aufrufe[-1] if len(aufrufe) > n1 else (False, '', '', None)
    pruefe(letzte[2] == '%s\n(und 1 weitere)' % mails['drei'][1], 'zusammengefasst: „%s“' % (letzte[2] or '').replace('\n', '⏎'))
    pruefe(all(a[0] for a in aufrufe[n1:]) and len(aufrufe) > n1, 'ersetzt die vorige Benachrichtigung (gleiche ID bzw. replaces_id)')
    ziel = warte(r'zusammen 2, Ziel ', ab, 1) or ''
    m = re.search(r'Ziel (\S+) (\S+)$', ziel)
    pruefe(m is not None and m.group(1) == folder_uri and m.group(2) == mails['drei'][0],
           'Ziel = neueste Mail „drei“: %s' % (m and m.groups(),))
    if letzte[3] is not None:
        pruefe(letzte[3] == (folder_uri, mails['drei'][0]), 'Ziel der Aktion am Bus (default-action-target): %s' % (letzte[3],))

    # 3) Klick: Aktion app.show-message mit dem Ziel der Benachrichtigung
    hinweis_m365()
    ab = marke()
    aktion('show-message', '(%s, %s)' % (gvariant_str(folder_uri), gvariant_str(mails['drei'][0])))
    z = warte(r'^Benachrichtigung: Nachricht %s gewählt' % mails['drei'][0], ab, 60)
    pruefe(z is not None, 'Klick öffnet die Mail: %s' % z)
    z = warte(r'^(Nachricht: fertig |Konversation: \S+ mit \d+ Nachrichten angezeigt)', ab, 60)
    pruefe(z is not None, 'Lesebereich: %s' % z)
    pruefe(warte(r'^Nachrichten: %s INBOX ' % GEOPS, ab, 1) is not None, 'Posteingang von geops@ gewählt')
    time.sleep(2)
    pruefe(len(zurueckgezogen()) > z0 + 1, 'Klick: Benachrichtigung zurückgezogen (%d Aufrufe)' % len(zurueckgezogen()))
    w = fenster('Revolution')
    if w is not None:
        bild('geoeffnet', w)
    reader = app_fenster('Revolution')
    namen = [k.get_name() for k in knoten(reader)] if reader is not None else []
    pruefe(any(mails['drei'][1] in (n or '') for n in namen), 'Betreff „%s“ im Lesebereich (AT-SPI)' % mails['drei'][1])

    # 4) Abruf-Zeitgeber sofort auslösen (Test-Hook)
    ab = marke()
    aktion('test-refresh', gvariant_str(OUTLOOK))
    z = warte(r'^Abruf: %s \S' % OUTLOOK, ab, 10)
    pruefe(z is not None and 'ohne Posteingang' not in z, 'Abruf ausgelöst: %s' % z)
    z = warte(r'^Posteingang: %s .* aktualisiert' % OUTLOOK, ab, 60)
    pruefe(z is not None, 'Abruf microsoft365: %s' % z)

    mailto()

    # 5) Hintergrund: Schließen verbirgt nur
    w = fenster('Revolution')
    schliessen(w)
    time.sleep(3)
    pruefe(app_laeuft() and fenster('Revolution', weg=True, zeit=5),
           'Fenster geschlossen: Prozess läuft weiter, Fenster verborgen')
    ab, n2 = marke(), len(notify_aufrufe())
    mails.update(testmails('vier'))
    zeile = benachrichtigt(ab, n2)
    pruefe(zeile is not None and len(notify_aufrufe()) > n2, 'im Hintergrund: Benachrichtigung für „vier“: %s' % zeile)
    ab = marke()
    aktion('show-message', '(%s, %s)' % (gvariant_str(folder_uri), gvariant_str(mails['vier'][0])))
    pruefe(warte(r'^Benachrichtigung: Nachricht %s gewählt' % mails['vier'][0], ab, 60) is not None and
           fenster('Revolution', zeit=10) is not None, 'Klick zeigt das Fenster wieder und öffnet „vier“')
    schliessen(fenster('Revolution'))
    time.sleep(2)
    pruefe(app_laeuft(), 'erneut geschlossen: Prozess läuft weiter')
    aktion('quit')


def hinweis_m365():
    f = app_fenster('Revolution')
    kopf = [k for k in knoten(f) if k.get_name() == 'KONTO_M365@example.invalid' and k.get_role_name() == 'label'] if f else []
    beschreibung = [k.get_description() for k in kopf]
    wert('Abschnittskopf outlook.com: %s' % beschreibung)
    pruefe(any(b and 'kein Push' in b and 'Abruf alle 60 Minuten' in b for b in beschreibung),
           'Hinweis microsoft365 ohne Push am Kontokopf (AT-SPI-Beschreibung)')
    g = [k for k in knoten(f) if k.get_name() == 'KONTO_IMAP_2@example.invalid' and k.get_role_name() == 'label'] if f else []
    pruefe(g and not any(k.get_description() for k in g), 'kein Hinweis bei geops@ (IMAP)')


def mailto_uri():
    return 'mailto:%s?subject=%s&cc=%s&bcc=%s&body=%s' % (
        MAILTO_AN, urllib.parse.quote(MAILTO_BETREFF), MAILTO_CC, MAILTO_BCC, urllib.parse.quote(MAILTO_TEXT))


def pruefe_composer(weg, ab):
    """Composer aus dem mailto-Link: Log, Felder über AT-SPI, Screenshot, Escape schließt."""
    z = warte(r'^Composer: mailto An 1, Kopie 1, Blindkopie 1, Betreff ja, Text ja', ab, 60)
    pruefe(z is not None and warte(r'^mailto: Composer für mailto:', ab, 1) is not None,
           'mailto %s: %s' % (weg, z))
    warte(r'^Composer: Neu vorbereitet', ab, 30)
    w = fenster(MAILTO_BETREFF, zeit=20)
    if not pruefe(w is not None, 'mailto %s: Composer „%s“ offen' % (weg, MAILTO_BETREFF)):
        return
    time.sleep(3)
    f = app_fenster(MAILTO_BETREFF)
    felder = {}
    for k in knoten(f):
        if k.get_name() in ('An', 'Kopie', 'Blindkopie', 'Betreff') and k.get_name() not in felder and \
                k.get_role_name() in ('entry', 'text'):
            t = text_von(k)
            if t is not None:
                felder[k.get_name()] = t
        if k.get_role_name() == 'document web':
            felder['Text'] = dokument_text(k)
    wert('mailto %s Felder: %s' % (weg, felder))
    pruefe(felder.get('An') == MAILTO_AN and felder.get('Kopie') == MAILTO_CC and felder.get('Blindkopie') == MAILTO_BCC
           and felder.get('Betreff') == MAILTO_BETREFF, 'mailto %s: An, Kopie, Blindkopie, Betreff' % weg)
    # Der Link ist ein eigenes Element (magic links wie Evolution).
    pruefe('Zeile 1' in felder.get('Text', '') and 'Zeile 2 mit' in felder.get('Text', '') and
           'https://example.org' in felder.get('Text', ''), 'mailto %s: Text im Editor' % weg)
    bild('mailto-' + weg.split()[0].lower(), w)
    taste('Escape', 0.5)
    pruefe(fenster(MAILTO_BETREFF, weg=True), 'mailto %s: Escape schließt den Composer' % weg)


def mailto():
    uri = mailto_uri()
    wert('mailto: %s' % uri)
    for weg in ('argument', 'gio-open'):
        ab = marke()
        if weg == 'argument':
            r = subprocess.run([BIN, uri], capture_output=True, text=True, timeout=30)
        else:
            umgebung = dict(os.environ, XDG_DATA_HOME=os.path.join(XDG, 'data'), XDG_CONFIG_HOME=os.path.join(XDG, 'config'),
                            XDG_DATA_DIRS=os.path.join(XDG, 'dirs'))
            mime = subprocess.run(['gio', 'mime', 'x-scheme-handler/mailto'], capture_output=True, text=True,
                                  env=dict(umgebung, LC_ALL='C.UTF-8', LANGUAGE=''))
            wert('gio mime x-scheme-handler/mailto (Test-XDG): %s' % mime.stdout.strip().replace('\n', ' | '))
            pruefe('Default application for “x-scheme-handler/mailto”: %s.desktop' % APP_ID in mime.stdout,
                   'Test-XDG: Standard für mailto ist %s.desktop' % APP_ID)
            r = subprocess.run(['gio', 'open', uri], capture_output=True, text=True, timeout=30, env=umgebung)
        pruefe(r.returncode == 0, 'mailto %s: Exit %d %s' % (weg, r.returncode, (r.stdout + r.stderr).strip()))
        pruefe_composer(weg + ' an die laufende Instanz', ab)
    pruefe(not any('org.freedesktop.portal' in b for b in dbus_bloecke()), 'kein Portal-Aufruf im Mitschnitt')


def menuename(k):
    """Name eines Menüeintrags: eigener Name, sonst Name bzw. Text des Elements aus „labelled-by“ (GtkPopoverMenu)."""
    if k.get_name():
        return k.get_name()
    for r in k.get_relation_set():
        if r.get_relation_type() == Atspi.RelationType.LABELLED_BY:
            z = r.get_target(0)
            t = z.get_text_iface()
            return z.get_name() or (Atspi.Text.get_text(t, 0, Atspi.Text.get_character_count(t)) if t else '')
    return ''


def menue():
    # Start mit mailto-Link als Argument (Revolution lief noch nicht): Hauptfenster und Composer.
    pruefe_composer('Kaltstart', 0)
    w = fenster('Revolution')
    time.sleep(2)
    zeiger((600, 380))
    taste('F10', 1.5)
    # Das Popover ist in AT-SPI ein eigenes Element der Anwendung neben dem Fenster: alle durchsuchen.
    eintraege = {}
    desktop = Atspi.get_desktop(0)
    for i in range(desktop.get_child_count()):
        a = desktop.get_child_at_index(i)
        if a is not None and a.get_process_id() == app_pid:
            eintraege.update({menuename(k): k for k in knoten(a)
                              if k.get_role_name() in ('check menu item', 'menu item', 'radio menu item')})
    wert('Hauptmenü (AT-SPI): %s' % sorted((n, e.get_role_name()) for n, e in eintraege.items()))
    hg = eintraege.get('Im Hintergrund weiterlaufen')
    pruefe(hg is not None and hg.get_role_name() == 'check menu item' and
           hg.get_state_set().contains(Atspi.StateType.CHECKED), 'Menü: „Im Hintergrund weiterlaufen“ mit Haken')
    be = eintraege.get('Beenden')
    pruefe(be is not None, 'Menü: „Beenden“')
    bild('menue', w)
    taste('Escape', 0.5)
    taste('Control_L+q', 0.5)


def app_laeuft():
    return app_pid is not None and app.poll() is None


def main():
    global app_pid, app
    monitor = subprocess.Popen(['dbus-monitor', '--session',
                                "type='method_call',interface='org.freedesktop.Notifications'",
                                "type='method_call',interface='org.gtk.Notifications'",
                                "type='method_call',interface='org.freedesktop.portal.OpenURI'",
                                "type='method_call',interface='org.freedesktop.portal.Background'",
                                "type='error'"], stdout=open(DBUS, 'w'), stderr=subprocess.STDOUT)
    log = open(LOG, 'w')
    app = subprocess.Popen([BIN] + ([mailto_uri()] if WAS == 'menue' else []), stdout=log, stderr=subprocess.STDOUT)
    app_pid = app.pid
    try:
        w = fenster('Revolution', zeit=60)
        if w is None:
            falsch('kein Fenster')
            return 1
        if WAS == 'haupt':
            haupt()
        else:
            menue()
        try:
            rc = app.wait(30)
            pruefe(rc == 0, 'Beenden (%s): Exit %d' % ('app.quit' if WAS == 'haupt' else 'Strg+Q', rc))
        except subprocess.TimeoutExpired:
            falsch('Beenden: nach 30 s nicht beendet')
        pruefe(warte(r'^Beenden: \d+ Fenster', 0, 1) is not None, 'Log „Beenden“')
    finally:
        if app.poll() is None:
            app.kill()
            app.wait()
        log.close()
        time.sleep(1)
        monitor.terminate()
        monitor.wait()
    # (Absender, Seriennummer) der Aufrufe der App an den Dienst; Fehlerantworten genau darauf.
    eigene = {re.search(r'sender=(\S+) .*serial=(\d+)', b.split('\n')[0]).groups() for b in dbus_bloecke()
              if b.startswith('method call') and re.search(r'member=(AddNotification|Notify|RemoveNotification|CloseNotification)$',
                                                           b.split('\n')[0])
              and re.search(r'destination=org\.(gtk|freedesktop)\.Notifications ', b.split('\n')[0])}
    fehlerantworten = [b.split('\n')[0] + ' ' + ' '.join(STRING_RE.findall(b))[:200] for b in dbus_bloecke()
                       if b.startswith('error ') and
                       (re.search(r'destination=(\S+)', b).group(1), re.search(r'reply_serial=(\d+)', b).group(1)) in eigene]
    wert('Benachrichtigungsaufrufe: %d, Backend %s' % (len(notify_aufrufe()), os.environ.get('GNOTIFICATION_BACKEND', 'Standard (GLib)')))
    if os.environ.get('GNOTIFICATION_BACKEND') == 'freedesktop':
        pruefe(not fehlerantworten, 'keine Fehlerantwort des Dienstes auf Notify: %s' % fehlerantworten)
    else:
        # Auf AddNotification bekommt die App keine Antwort (auch keinen Fehler); ob die Shell annimmt, prüft
        # benachrichtigung.sh vorab mit einem eigenen, synchronen AddNotification (Zeile OK bzw. BLOCKER).
        wert('Antworten an die App auf AddNotification/RemoveNotification: %s' % (fehlerantworten or 'keine'))
    rest = [z.strip() for z in open(LOG, encoding='utf-8', errors='replace')
            if z.strip() and not (z.startswith('(revolution:') and 'revolution-DEBUG' in z)
            and not z.startswith('libEGL warning:')]
    pruefe(not rest, 'Log: %s' % ('%d unerwartete Zeilen: %s' % (len(rest), rest[:3]) if rest else 'nur DEBUG-Zeilen'))
    return 1 if fehler else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as e:  # #411: Abbruch zählt als FEHLER
        traceback.print_exc()
        falsch('Abbruch: %s: %s' % (type(e).__name__, str(e).replace('\n', ' ')[:300]))
        sys.exit(1)
