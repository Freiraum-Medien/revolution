#!/usr/bin/env python3
# composer-treiber.py – Treiber für tests/composer.sh (#359, #360), läuft im Xvfb aus xvfb-run.
#
# Startet die App (BIN) mit Log, bedient sie per XTest (python3-xlib) und prüft die Nachrichten, die
# Senden im Test in REVOLUTION_TEST_MIME schreibt (composer.c, kein Versand), gegen die Kopfzeilen des
# Originals vom Server (KOPF, tests/testdaten.py --antwort-kopf) und die Mail-Identitäten der Registry.
# Ausgabe: „OK …“, „FEHLER …“, „WERT …“; Screenshots OUT/NAME-<Schritt>.png, Nachrichten OUT/NAME-<Schritt>.eml.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

import email
import email.policy
import hashlib
import email.utils
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import traceback

import gi
gi.require_version('Atspi', '2.0')
gi.require_version('EDataServer', '1.2')
from gi.repository import Atspi, EDataServer  # noqa: E402

from PIL import Image  # noqa: E402
from Xlib import X, XK, display, protocol  # noqa: E402
from Xlib.ext import xtest  # noqa: E402

BIN, OUT, NAME, ORDNER, KOPF = (os.environ[k] for k in ('BIN', 'OUT', 'NAME', 'ORDNER', 'KOPF'))
MIME = os.environ['REVOLUTION_TEST_MIME']
KONTO = 'KONTO_IMAP_2@example.invalid'
LOGKEY = ORDNER.replace('/', ' ', 1)
# Zeiger über dem Hauptfenster rechts (Lesebereich), außerhalb des Composers (960×720 bei 0,0).
HAUPT = (1150, 420)
COMPOSER = (480, 400)

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


# ------------------------------------------------------------------ XTest

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


def tippe(text):
    shift = d.keysym_to_keycode(XK.string_to_keysym('Shift_L'))
    for ch in text:
        code, index = next(iter(d.keysym_to_keycodes(ord(ch))))
        if index % 2:
            xtest.fake_input(d, X.KeyPress, shift)
        xtest.fake_input(d, X.KeyPress, code)
        xtest.fake_input(d, X.KeyRelease, code)
        if index % 2:
            xtest.fake_input(d, X.KeyRelease, shift)
        d.sync()
        time.sleep(0.03)
    time.sleep(0.3)


def klick(p):
    zeiger(p)
    xtest.fake_input(d, X.ButtonPress, 1)
    xtest.fake_input(d, X.ButtonRelease, 1)
    d.sync()
    time.sleep(0.5)


def fenster(titel=None, weg=False, zeit=20):
    """Hauptfenster („Revolution“) bzw. Composer (anderer Titel); weg=True: warten, bis keiner mehr da ist."""
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        gefunden = None
        for w in d.screen().root.query_tree().children:
            try:
                name = w.get_wm_name()
                if name and w.get_attributes().map_state == X.IsViewable and \
                        (name == titel if titel == 'Revolution' else name != 'Revolution'):
                    gefunden = w
            except Exception:
                pass
        if (gefunden is None) == weg:
            return gefunden if not weg else True
        time.sleep(0.2)
    return None


def aufklapper():
    """Sichtbare Popups (override-redirect, z. B. Popover)."""
    out = []
    for w in d.screen().root.query_tree().children:
        try:
            a = w.get_attributes()
            if a.map_state == X.IsViewable and a.override_redirect:
                out.append(w)
        except Exception:
            pass
    return out


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
    """Elemente des Fensters; der Inhalt der WebView (Mailtext, Rolle „document web“) zählt als ein Element."""
    if o is None or tiefe > 40:
        return
    yield o
    if o.get_role_name() == 'document web':
        return
    for i in range(o.get_child_count()):
        yield from knoten(o.get_child_at_index(i), tiefe + 1)


def pfad(o):
    out = []
    while o is not None and o.get_role_name() != 'frame':
        p = o.get_parent()
        out.append('%s[%d/%d]%s' % (o.get_role_name(), o.get_index_in_parent(), p.get_child_count() if p else 0,
                                    o.get_name() and '„%s“' % o.get_name() or ''))
        o = p
    return ' < '.join(out)


def composer_frame(titel):
    desktop = Atspi.get_desktop(0)
    for i in range(desktop.get_child_count()):
        a = desktop.get_child_at_index(i)
        if a is None or a.get_process_id() != app_pid:
            continue
        for j in range(a.get_child_count()):
            f = a.get_child_at_index(j)
            if f is not None and f.get_name() == titel:
                return f
    return None


def composer_a11y(titel):
    """Namen aller bedienbaren Elemente im Composer-Fenster (AT-SPI)."""
    f = composer_frame(titel)
    return [(o.get_role_name(), o.get_name(), pfad(o),
             o.get_attributes(), o.get_extents(Atspi.CoordType.WINDOW)) for o in knoten(f)] if f else []


def a11y_klick(titel, rollen, name, index=-1, klicken=True):
    """#360: Element im Composer per AT-SPI finden und auslösen (Knöpfe über die AT-SPI-Aktion, Textfelder per
    Klick in die Mitte); Anzahl der Treffer."""
    f = composer_frame(titel)
    treffer = [o for o in knoten(f) if o.get_role_name() in rollen.split('|') and o.get_name() == name] if f else []
    if treffer and klicken:
        o = treffer[index]
        e = o.get_extents(Atspi.CoordType.SCREEN)
        aktion = o.get_action_iface() if o.get_role_name() not in ('text', 'entry') else None
        wert('a11y %s „%s“: %d Treffer, Mitte %d,%d, Aktion %s' % (rollen, name, len(treffer), e.x + e.width // 2,
             e.y + e.height // 2, aktion and aktion.get_n_actions() and aktion.get_action_name(0)))
        if aktion is not None and aktion.get_n_actions() > 0:
            aktion.do_action(0)
            time.sleep(0.8)
        else:
            klick((e.x + e.width // 2, e.y + e.height // 2))
    return len(treffer)


def pruefe_a11y(titel):
    elemente = composer_a11y(titel)
    wert('a11y: %s' % [(e[0], e[1]) for e in elemente])
    namen = {e[1] for e in elemente}
    for soll in ('An', 'Kopie', 'Blindkopie', 'Betreff', 'Von', 'Nachrichtentext', 'Hauptmenü', 'Anhang hinzufügen',
                 'Formatierung anzeigen', 'Senden', 'Absatzformat', 'Fett', 'Kursiv', 'Unterstrichen',
                 'Durchgestrichen', 'Aufzählung', 'Nummerierung', 'Link einfügen'):
        pruefe(soll in namen, 'a11y: „%s“ vorhanden' % soll)
    bedienbar = [e for e in elemente if e[0] in ('push button', 'toggle button', 'text', 'entry', 'combo box')]
    # Ohne Fläche (0×0) ist nichts bedienbar: WebKit legt in der WebView ein internes Textfeld an (Kind 2 der
    # WebView neben dem Inhalt, gemessen über AT-SPI); es zählt nicht.
    for e in bedienbar:
        if e[4].width == 0 or e[4].height == 0:
            wert('a11y: ohne Fläche, nicht gezählt: %s %s' % (e[0], e[2]))
    bedienbar = [e for e in bedienbar if e[4].width > 0 and e[4].height > 0]
    ohne = [(e[0], e[2]) for e in bedienbar if not e[1]]
    pruefe(bedienbar and not ohne, 'a11y: %d bedienbare Elemente, ohne Namen: %s' % (len(bedienbar), ohne))


# ------------------------------------------------------------------ Erwartung


def eigene_adressen():
    """Adressen und Aliase der eingeschalteten Mail-Identitäten (wie Evolution generate_recipient_hash)."""
    reg = EDataServer.SourceRegistry.new_sync(None)
    out = set()
    for s in reg.list_sources(EDataServer.SOURCE_EXTENSION_MAIL_IDENTITY):
        if reg.check_enabled(s):
            ext = s.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_IDENTITY)
            if ext.get_address():
                out.add(ext.get_address().lower())
            for a in (ext.get_aliases() or '').split(','):
                if '@' in a:
                    out.add(email.utils.parseaddr(a)[1].lower())
    return out


def original():
    kopf = {}
    for z in open(KOPF, encoding='utf-8'):
        if z.startswith('KOPF '):
            n, _, v = z[5:].rstrip('\n').partition(': ')
            kopf[n] = v
    return kopf


def adressen(wert_):
    return [a.lower() for _, a in email.utils.getaddresses([str(wert_)])] if wert_ else []


def erwartung(kopf, allen, eigen):
    """Erwartete An/Kopie aus dem Original (ohne Reply-To/Mail-Followup-To, wie die Testmail)."""
    von = adressen(kopf.get('From'))
    if not allen:
        return von, []
    an, kopie, gesehen = [], [], set(eigen)
    for a in von + adressen(kopf.get('To')):
        if a not in gesehen:
            an.append(a)
            gesehen.add(a)
    for a in adressen(kopf.get('Cc')):
        if a not in gesehen:
            kopie.append(a)
            gesehen.add(a)
    return an, kopie


def teile(m):
    if m.get_content_type() == 'multipart/alternative':
        return {p.get_content_type(): p.get_content() for p in m.iter_parts()}
    return {m.get_content_type(): m.get_content()}


def pruefe_antwort(schritt, allen, eigen):
    kopf = original()
    m = email.message_from_bytes(open(os.path.join(OUT, '%s-%s.eml' % (NAME, schritt)), 'rb').read(),
                                 policy=email.policy.default)
    an, kopie = erwartung(kopf, allen, eigen)
    zeilen = ['Original:'] + ['  %s: %s' % kv for kv in kopf.items()] + ['Antwort (%s):' % schritt] + \
             ['  %s: %s' % (n, ' '.join(str(m[n]).split())) for n in
              ('From', 'To', 'Cc', 'Subject', 'Message-ID', 'In-Reply-To', 'References', 'Content-Type') if m[n]]
    with open(os.path.join(OUT, '%s-%s-vergleich.txt' % (NAME, schritt)), 'w', encoding='utf-8') as f:
        f.write('\n'.join(zeilen) + '\n')
    pruefe(adressen(m['From']) == [KONTO], '%s: From %s' % (schritt, m['From']))
    pruefe(adressen(m['To']) == an, '%s: To %s (erwartet %s)' % (schritt, adressen(m['To']), an))
    pruefe(adressen(m['Cc']) == kopie, '%s: Cc %s (erwartet %s)' % (schritt, adressen(m['Cc']), kopie))
    pruefe(not (set(adressen(m['To']) + adressen(m['Cc'])) & eigen), '%s: keine eigene Adresse in To/Cc' % schritt)
    betreff = 'Re: ' + re.sub(r'^re ?: ?', '', kopf['Subject'], flags=re.I)
    pruefe(str(m['Subject']) == betreff, '%s: Subject „%s“ (erwartet „%s“)' % (schritt, m['Subject'], betreff))
    pruefe(str(m['In-Reply-To']) == kopf['Message-ID'], '%s: In-Reply-To %s' % (schritt, m['In-Reply-To']))
    refs = kopf['References'] + ' ' + kopf['Message-ID']
    pruefe(' '.join(str(m['References']).split()) == refs, '%s: References %s' % (schritt, m['References']))
    pruefe(m['Message-ID'] and m['Message-ID'] != kopf['Message-ID'] and m['Message-ID'].endswith('@example.invalid>'),
           '%s: eigene Message-ID %s' % (schritt, m['Message-ID']))
    t = teile(m)
    pruefe(set(t) == {'text/plain', 'text/html'} and m.get_content_type() == 'multipart/alternative',
           '%s: multipart/alternative mit %s' % (schritt, sorted(t)))
    html, plain = t.get('text/html', ''), t.get('text/plain', '')
    zitat = re.search(r'<blockquote type="cite"[^>]*>(.*)</blockquote>', html, re.S)
    pruefe(zitat and '<b>Standplan</b>' in zitat.group(1) and 'Stand 12' in zitat.group(1),
           '%s: Zitat als blockquote type="cite" mit dem HTML des Originals' % schritt)
    pruefe(re.search(r'dem 24\.10\.2026 um 09:30 \+0200 schrieb Jana Albrecht:', html) is not None,
           '%s: Zuschreibung im HTML' % schritt)
    pruefe(re.search(r'^> .*Standplan.*Stand 12', plain, re.M) is not None, '%s: Zitat im Text mit „> “' % schritt)


# ------------------------------------------------------------------ Schritte

def senden(schritt, w):
    if os.path.exists(MIME):
        os.remove(MIME)
    ab = marke()
    zeiger(COMPOSER)
    taste('Control_L+Return')
    pruefe(warte(r'^Composer: MIME geschrieben ', ab, 30) is not None, '%s: Strg+Enter schreibt MIME' % schritt)
    pruefe(fenster(weg=True), '%s: Composer nach dem Senden geschlossen' % schritt)
    if os.path.exists(MIME):
        shutil.copy(MIME, os.path.join(OUT, '%s-%s.eml' % (NAME, schritt)))
        return True
    falsch('%s: %s fehlt' % (schritt, MIME))
    return False


def antwort(schritt, kombi, allen, eigen, a11y=False):
    ab = marke()
    zeiger(HAUPT)
    taste(kombi)
    typ = 'Allen antworten' if allen else 'Antworten'
    pruefe(warte(r'^Composer: %s vorbereitet, Von %s$' % (typ, re.escape(KONTO)), ab, 60) is not None,
           '%s: %s öffnet den Composer, Von %s' % (schritt, kombi, KONTO))
    w = fenster()
    if w is None:
        falsch('%s: kein Composer-Fenster' % schritt)
        return
    time.sleep(3)
    titel = w.get_wm_name()
    wert('%s: Fenstertitel „%s“' % (schritt, titel))
    bild(schritt, w)
    if a11y:
        pruefe_a11y(titel)
    if senden(schritt, w):
        pruefe_antwort(schritt, allen, eigen)


def neu():
    ab = marke()
    zeiger(HAUPT)
    taste('Control_L+n')
    pruefe(warte(r'^Composer: Neu vorbereitet, Von %s$' % re.escape(KONTO), ab, 30) is not None,
           'neu: Strg+N öffnet den Composer, Von %s (Konto des Ordners)' % KONTO)
    w = fenster()
    if w is None:
        falsch('neu: kein Composer-Fenster')
        return
    time.sleep(2)
    zeiger(COMPOSER)
    tippe('KONTO_IMAP_1@example.invalid')
    taste('Tab', 0.2)
    taste('Tab', 0.2)
    taste('Tab', 0.2)
    tippe('[Revolution-Test] Neu 359')
    g = w.get_geometry()
    klick((g.x + 300, g.y + 450))
    tippe('Hallo ')
    taste('Control_L+b', 0.3)
    tippe('fett')
    # Absatzformat: neue Zeile, Auswahl „Text ▾“ unten links (Formatierungsleiste), Pfeil runter = Überschrift 1.
    taste('Control_L+b', 0.3)
    taste('Return', 0.3)
    klick((g.x + 45, g.y + g.height - 28))
    taste('Down', 0.3)
    taste('Return', 0.8)
    tippe('Titel')
    bild('neu', w)
    popups = len(aufklapper())
    taste('Control_L+period', 1.5)
    pruefe(len(aufklapper()) > popups, 'neu: Strg+. öffnet die Emoji-Auswahl (%d → %d Popups)' % (popups, len(aufklapper())))
    bild('emoji', w)
    taste('Escape', 1)
    pruefe(len(aufklapper()) == popups and fenster() is not None,
           'neu: Escape schließt zuerst nur die Emoji-Auswahl, der Composer bleibt')
    if not senden('neu', w):
        return
    m = email.message_from_bytes(open(os.path.join(OUT, '%s-neu.eml' % NAME), 'rb').read(), policy=email.policy.default)
    pruefe(adressen(m['To']) == ['KONTO_IMAP_1@example.invalid'] and adressen(m['From']) == [KONTO],
           'neu: From %s To %s' % (m['From'], m['To']))
    pruefe(str(m['Subject']) == '[Revolution-Test] Neu 359', 'neu: Subject %s' % m['Subject'])
    pruefe(m['In-Reply-To'] is None and m['References'] is None, 'neu: ohne In-Reply-To/References')
    t = teile(m)
    pruefe(re.search(r'Hallo (&nbsp;)?<b>fett</b>', t.get('text/html', '')) is not None,
           'neu: Strg+B im Editor ergibt <b>fett</b> (%s)' % t.get('text/html', '')[-120:].replace('\n', ' '))
    pruefe('Hallo fett' in t.get('text/plain', '').replace('\xa0', ' '), 'neu: Text „Hallo fett“')
    # Strg+B vor der neuen Zeile schaltet Fett aus: WebKit schreibt dann <span style="font-weight: normal;">.
    h1 = re.search(r'<h1>(?:<span[^>]*>)?Titel(?:</span>)?</h1>', t.get('text/html', ''))
    pruefe(h1 is not None, 'neu: Absatzformat Überschrift 1 ergibt %s' % (h1 and h1.group(0)))


def a11y():
    ab = marke()
    zeiger(HAUPT)
    taste('Control_L+r')
    warte(r'^Composer: Antworten vorbereitet', ab, 60)
    w = fenster()
    if w is None:
        falsch('a11y: kein Composer-Fenster')
        return
    time.sleep(3)
    pruefe_a11y(w.get_wm_name())
    zeiger(COMPOSER)
    taste('Escape', 0.5)
    pruefe(fenster(weg=True), 'a11y: Escape schließt den Composer (Fokus im Editor)')


def konversation():
    """#357: In der Konversation antwortet Strg+R auf die neueste Nachricht (tests/testdaten.py --konversation,
    Nr. 5 von „Jana Albrecht“ <KONTO_IMAP_1@example.invalid>, References auf 1 bis 4)."""
    ab = marke()
    zeiger(HAUPT)
    taste('Control_L+r')
    # Nr. 5 kommt von KONTO_IMAP_1@example.invalid, einer eigenen Identität: Allen antworten (R2, em-composer-utils.c:5127-5131);
    # ohne andere Empfänger füllt Evolution An mit dem Absender auf (3818-3823).
    pruefe(warte(r'^Composer: Absender privat@arndmesser\.de ist eigene Adresse, Allen antworten$', ab, 60) is not None,
           'konversation: Absender ist eigene Adresse')
    pruefe(warte(r'^Composer: Allen antworten vorbereitet, Von %s$' % re.escape(KONTO), ab, 60) is not None,
           'konversation: Strg+R öffnet den Composer (Allen antworten)')
    w = fenster()
    if w is None:
        falsch('konversation: kein Composer-Fenster')
        return
    time.sleep(3)
    if not senden('konversation', w):
        return
    m = email.message_from_bytes(open(os.path.join(OUT, '%s-konversation.eml' % NAME), 'rb').read(),
                                 policy=email.policy.default)
    mid = ['<revolution-test-konversation-357-%d@example.invalid>' % i for i in range(1, 6)]
    pruefe(str(m['In-Reply-To']) == mid[4], 'konversation: In-Reply-To %s (neueste, Nr. 5)' % m['In-Reply-To'])
    pruefe(' '.join(str(m['References']).split()) == ' '.join(mid), 'konversation: References %s' % m['References'])
    pruefe(adressen(m['To']) == ['KONTO_IMAP_1@example.invalid'], 'konversation: To %s' % m['To'])
    pruefe(not m['Cc'], 'konversation: kein Cc (%s)' % m['Cc'])
    pruefe(str(m['Subject']) == 'Re: [Revolution-Test] Konversation Marktstand', 'konversation: Subject %s' % m['Subject'])


def antwort_eigen(schritt, nr, an, kopie, betreff, regel):
    """R2: Strg+R auf eine eigene Nachricht wird Allen antworten (Evolution em-composer-utils.c:5127-5131 bzw.
    Gesendet 5149-5151), Empfänger ohne eigene Adressen (tests/testdaten.py --antwort, eigen_nachricht)."""
    ab = marke()
    zeiger(HAUPT)
    taste('Control_L+r')
    pruefe(warte(r'^Composer: %s' % re.escape(regel), ab, 60) is not None, '%s: Log „Composer: %s“' % (schritt, regel))
    pruefe(warte(r'^Composer: Allen antworten vorbereitet, Von %s$' % re.escape(KONTO), ab, 60) is not None,
           '%s: Strg+R ergibt Allen antworten' % schritt)
    w = fenster()
    if w is None:
        falsch('%s: kein Composer-Fenster' % schritt)
        return
    time.sleep(3)
    bild(schritt, w)
    if not senden(schritt, w):
        return
    m = email.message_from_bytes(open(os.path.join(OUT, '%s-%s.eml' % (NAME, schritt)), 'rb').read(),
                                 policy=email.policy.default)
    mid = ['<revolution-test-eigen-359-%d@example.invalid>' % i for i in (1, 2, 3)]
    refs = ' '.join(mid[:2]) if nr == 2 else mid[2]
    pruefe(adressen(m['To']) == an, '%s: To %s (erwartet %s)' % (schritt, adressen(m['To']), an))
    pruefe(adressen(m['Cc']) == kopie, '%s: Cc %s (erwartet %s)' % (schritt, adressen(m['Cc']), kopie))
    pruefe(str(m['In-Reply-To']) == mid[nr - 1], '%s: In-Reply-To %s' % (schritt, m['In-Reply-To']))
    pruefe(' '.join(str(m['References']).split()) == refs, '%s: References %s' % (schritt, m['References']))
    pruefe(str(m['Subject']) == betreff, '%s: Subject „%s“ (erwartet „%s“)' % (schritt, m['Subject'], betreff))


# ------------------------------------------------------------------ #360

ANHANG_MID = '<revolution-test-anhang-356@example.invalid>'
ENTWURF = '[Revolution-Test] Entwurf 360'


def identitaet(adresse):
    """UID der Mail-Identität des Kontos mit dieser Adresse (für X-Evolution-Identity)."""
    reg = EDataServer.SourceRegistry.new_sync(None)
    for s in reg.list_sources(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT):
        uid = s.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_ACCOUNT).get_identity_uid()
        i = reg.ref_source(uid) if uid else None
        if i and reg.check_enabled(i) and \
                i.get_extension(EDataServer.SOURCE_EXTENSION_MAIL_IDENTITY).get_address() == adresse:
            return i.get_uid()
    return None


def struktur(m):
    if m.is_multipart() or m.get_content_type() == 'message/rfc822':
        return '%s[%s]' % (m.get_content_type(), ','.join(struktur(t) for t in m.get_payload()))
    return m.get_content_type()


def td(*argumente):
    """tests/testdaten.py (eigene Prüfung am Server, unabhängig von der App)."""
    r = subprocess.run([sys.executable, os.path.join(os.environ['APP_DIR'], 'tests', 'testdaten.py')] + list(argumente),
                       capture_output=True, text=True, timeout=300)
    with open(os.path.join(OUT, NAME + '-testdaten.txt'), 'a', encoding='utf-8') as f:
        f.write('$ testdaten.py %s\n%s%s' % (' '.join(argumente), r.stdout, r.stderr))
    return r.stdout


def entwuerfe(konto, soll, schritt):
    """Sichtbare Entwürfe des Tests am Server (tests/testdaten.py --entwurf)."""
    aus = td('--entwurf', konto)
    m = re.search(r'^ENTWUERFE (\d+) (\d+)$', aus, re.M)
    wert('%s: Server %s: %s' % (schritt, konto, ' | '.join(z for z in aus.splitlines() if z.startswith('ENTWURF'))))
    pruefe(m is not None and int(m.group(1)) == soll,
           '%s: %s zeigt %s sichtbare Entwürfe (erwartet %d)' % (schritt, konto, m and m.group(1), soll))


def weiterleiten():
    """Strg+Alt+F auf die Anhang-Mail (#356): Fwd:, message/rfc822 mit den Originalanhängen, eigene Anhänge
    hinzufügen/entfernen (REVOLUTION_TEST_ATTACH), Signatur GEOPS, multipart/mixed, X-Evolution-Identity."""
    ab = marke()
    zeiger(HAUPT)
    taste('Control_L+Alt_L+f')
    pruefe(warte(r'^Composer: Weiterleiten vorbereitet, Von %s$' % re.escape(KONTO), ab, 60) is not None,
           'weiterleiten: Strg+Alt+F öffnet den Composer')
    w = fenster()
    if w is None:
        falsch('weiterleiten: kein Composer-Fenster')
        return
    time.sleep(3)
    titel = w.get_wm_name()
    wert('weiterleiten: Fenstertitel „%s“' % titel)
    tippe('KONTO_IMAP_1@example.invalid')
    ab = marke()
    taste('Control_L+m', 1)
    pruefe(warte(r'^Composer: 2 Anhänge hinzugefügt$', ab, 30) is not None, 'weiterleiten: Strg+M fügt 2 Dateien an')
    time.sleep(1)
    n = a11y_klick(titel, 'push button|button', 'Anhang entfernen')
    time.sleep(1)
    rest = a11y_klick(titel, 'push button|button', 'Anhang entfernen', klicken=False)
    if n == 0:
        wert('weiterleiten: a11y %s' % [(e[0], e[1]) for e in composer_a11y(titel) if 'Anh' in (e[1] or '') or
                                         e[0] in ('push button', 'list item', 'list')])
    pruefe(n == 3 and rest == 2, 'weiterleiten: 3 Pillen, „Anhang entfernen“ an der letzten lässt %d (vorher %d)' % (rest, n))
    ab = marke()
    pruefe(a11y_klick(titel, 'toggle button', 'Signatur') == 1, 'weiterleiten: Auswahl Signatur geöffnet')
    taste('Down', 0.3)
    taste('Return', 1)
    pruefe(warte(r'^Composer: Signatur GEOPS geladen$', ab, 30) is not None, 'weiterleiten: Signatur GEOPS gewählt')
    time.sleep(1)
    bild('weiterleiten', w)
    if not senden('weiterleiten', w):
        return
    m = email.message_from_bytes(open(os.path.join(OUT, '%s-weiterleiten.eml' % NAME), 'rb').read(),
                                 policy=email.policy.default)
    pruefe(str(m['Subject']) == 'Fwd: [Revolution-Test] Anhänge PDF, Bild, ZIP', 'weiterleiten: Subject %s' % m['Subject'])
    pruefe(str(m['References']) == ANHANG_MID and m['In-Reply-To'] is None,
           'weiterleiten: References %s, ohne In-Reply-To' % m['References'])
    pruefe(adressen(m['From']) == [KONTO] and str(m['X-Evolution-Identity']) == identitaet(KONTO),
           'weiterleiten: From %s, X-Evolution-Identity %s' % (m['From'], m['X-Evolution-Identity']))
    aufbau = struktur(m)
    wert('weiterleiten: Aufbau %s' % aufbau)
    teile_ = list(m.iter_parts())
    pruefe(m.get_content_type() == 'multipart/mixed' and [t.get_content_type() for t in teile_] ==
           ['multipart/alternative', 'message/rfc822', 'application/octet-stream'],
           'weiterleiten: multipart/mixed aus multipart/alternative, message/rfc822 und dem verbliebenen Anhang')
    datei = os.environ['REVOLUTION_TEST_ATTACH'].split(':')[0]
    if len(teile_) == 3:
        pruefe(teile_[2].get_filename() == os.path.basename(datei) and
               teile_[2].get_payload(decode=True) == open(datei, 'rb').read(),
               'weiterleiten: Anhang %s mit gleichen Bytes wie die Datei' % teile_[2].get_filename())
    html = teile(teile_[0]).get('text/html', '')
    plain = teile(teile_[0]).get('text/plain', '')
    pruefe(re.search(r'<div id="revolution-signature" data-uid="[0-9a-f]+">-- <br>', html) is not None and
           '\n-- \n' in plain.replace('\xa0', ' '), 'weiterleiten: Signatur mit Trenner in HTML und Text')
    original = {}
    for z in open(os.environ['TEILE'], encoding='utf-8'):
        feld = z.rstrip('\n').split(' ', 3)
        if feld[0] == 'STRUKTUR':
            original['STRUKTUR'] = feld[1]
        elif feld[0] in ('TEIL', 'TEIL-TEXT'):
            original.setdefault(feld[3].replace(' (CRLF→LF)', ''), set()).add(feld[1])
    if len(teile_) >= 2:
        innen = teile_[1].get_payload()[0]
        pruefe(struktur(innen) == original.get('STRUKTUR'),
               'weiterleiten: Aufbau der Nachricht im Anhang gleich dem Original (%s)' % struktur(innen))
        for a in innen.iter_attachments():
            if a.get_content_type() == 'message/rfc822':
                continue
            h = hashlib.sha256(a.get_payload(decode=True)).hexdigest()
            pruefe(h in original.get(a.get_filename(), ()), 'weiterleiten: Anhang %s sha256 %s gleich dem Server' %
                   (a.get_filename(), h[:16]))
        pruefe(str(teile_[1].get_filename()).startswith('Weitergeleitete Nachricht — '),
               'weiterleiten: Dateiname %s' % teile_[1].get_filename())


def graph_schreibend(ab_zeile):
    """#360/#416: schreibende Graph-Anfragen im Log ab Zeile ab_zeile (Rohzeilen)."""
    with open(LOG, encoding='utf-8', errors='replace') as f:
        return [z.strip() for z in list(f)[ab_zeile:] if re.match(r'^\[graph\] (POST|PATCH|PUT|DELETE) ', z)]


def roh_zeilen():
    with open(LOG, encoding='utf-8', errors='replace') as f:
        return len(f.readlines())


def entwurf_tippen(w, an, betreff, text):
    tippe(an)
    taste('Tab', 0.2)
    taste('Tab', 0.2)
    taste('Tab', 0.2)
    tippe(betreff)
    g = w.get_geometry()
    klick((g.x + 300, g.y + 450))
    tippe(text)


def gespeichert(ab):
    return [z for z in log_zeilen()[ab:] if z.startswith('Entwurf: gespeichert ')]


def entwurf_neu():
    """Autosave (REVOLUTION_TEST_AUTOSAVE) in geops@ (IMAP), dann Identität outlook.com wählen und mit Strg+S
    zweimal speichern (Graph): je genau ein Entwurf am Server."""
    ab = marke()
    zeiger(HAUPT)
    taste('Control_L+n')
    pruefe(warte(r'^Composer: Neu vorbereitet, Von %s$' % re.escape(KONTO), ab, 60) is not None, 'entwurf: Strg+N')
    w = fenster()
    if w is None:
        falsch('entwurf: kein Composer-Fenster')
        return
    time.sleep(2)
    intervall = int(os.environ['REVOLUTION_TEST_AUTOSAVE'])
    entwurf_tippen(w, 'KONTO_IMAP_1@example.invalid', ENTWURF + ' IMAP', 'Erste Fassung')
    pruefe(warte(r'^Entwurf: gespeichert folder://', ab, intervall + 60) is not None, 'entwurf: Autosave speichert')
    tippe(' zweite Fassung')
    pruefe(warte(r'^Entwurf: folder://\S+ \d+ als gelöscht markiert \(abgeglichen\)$', ab, intervall + 60) is not None,
           'entwurf: zweites Autosave ersetzt die erste Fassung')
    time.sleep(1)
    n = len(gespeichert(ab))
    time.sleep(2 * intervall + 5)
    pruefe(len(gespeichert(ab)) == n, 'entwurf: ohne Änderung kein weiteres Autosave (%d Speicherungen)' % n)
    bild('entwurf', w)
    zeiger(COMPOSER)
    taste('Escape', 1)
    pruefe(fenster(weg=True), 'entwurf: Escape schließt den Composer')
    entwuerfe(KONTO, 1, 'entwurf')

    ab = marke()
    zeiger(HAUPT)
    taste('Control_L+n')
    zeile = warte(r'^Composer: Identitäten: ', ab, 60)
    w = fenster()
    if w is None or zeile is None:
        falsch('entwurf graph: kein Composer-Fenster')
        return
    time.sleep(2)
    liste = zeile.split(': ', 2)[2].split()
    wert('entwurf graph: Identitäten %s' % liste)
    titel = w.get_wm_name()
    roh = roh_zeilen()
    # Erst tippen (Fokus in An), dann Absender wählen und sofort speichern.
    entwurf_tippen(w, KONTO, ENTWURF + ' Graph', 'Erste Fassung')
    titel = w.get_wm_name()
    pruefe(a11y_klick(titel, 'toggle button', 'Von') == 1, 'entwurf graph: Auswahl Von geöffnet (%d Popups)' % len(aufklapper()))
    schritte = liste.index('KONTO_M365@example.invalid') - liste.index(KONTO)
    for _ in range(abs(schritte)):
        taste('Down' if schritte > 0 else 'Up', 0.2)
    taste('Return', 1)
    gewechselt = warte(r'^Composer: Von arndmesser@outlook\.com ', ab, 10) is not None
    pruefe(gewechselt, 'entwurf graph: Von KONTO_M365@example.invalid')
    if not gewechselt:
        bild('entwurf-von', w)
        taste('Escape', 0.5)
        zeiger(COMPOSER)
        taste('Escape', 1)
        return
    taste('Control_L+s', 0.5)
    pruefe(warte(r'^Entwurf: gespeichert folder://aed9', ab, 60) is not None, 'entwurf graph: Strg+S speichert in outlook.com')
    g = w.get_geometry()
    klick((g.x + 300, g.y + 450))
    tippe(' zweite Fassung')
    taste('Control_L+s', 0.5)
    pruefe(warte(r'^Entwurf: folder://aed9\S+ \S+ als gelöscht markiert \(abgeglichen\)$', ab, 60) is not None,
           'entwurf graph: zweites Strg+S ersetzt die erste Fassung')
    time.sleep(3)
    wert('entwurf graph: %s' % gespeichert(ab))
    schreib = graph_schreibend(roh)
    wert('entwurf graph: schreibende Graph-Anfragen %s' % schreib)
    pruefe(schreib and not [z for z in schreib if z.startswith('[graph] PATCH')],
           'entwurf graph: kein Echo-PATCH beim Speichern/Ersetzen (#416), %d schreibende Anfragen' % len(schreib))
    zeiger(COMPOSER)
    taste('Escape', 1)
    pruefe(fenster(weg=True), 'entwurf graph: Escape schließt den Composer')
    entwuerfe('KONTO_M365@example.invalid', 1, 'entwurf graph')


def entwurf_oeffnen(konto, art):
    """Strg+O im Entwürfe-Ordner bearbeitet den Entwurf weiter; Strg+S ersetzt ihn, Senden (Test) löscht ihn."""
    ab = marke()
    zeiger(HAUPT)
    taste('Control_L+o')
    pruefe(warte(r'^Composer: Entwurf vorbereitet, Von %s$' % re.escape(konto), ab, 60) is not None,
           'öffnen: Strg+O öffnet den Entwurf, Von %s (X-Evolution-Identity)' % konto)
    w = fenster()
    if w is None:
        falsch('öffnen: kein Composer-Fenster')
        return
    time.sleep(3)
    pruefe(w.get_wm_name() == ENTWURF + ' ' + art, 'öffnen: Betreff %s' % w.get_wm_name())
    g = w.get_geometry()
    klick((g.x + 300, g.y + 450))
    taste('End', 0.2)
    tippe(' dritte Fassung')
    taste('Control_L+s', 0.5)
    pruefe(warte(r'^Entwurf: folder://\S+ \S+ als gelöscht markiert \(abgeglichen\)$', ab, 60) is not None,
           'öffnen: Strg+S ersetzt den geöffneten Entwurf')
    time.sleep(1)
    entwuerfe(konto, 1, 'öffnen')
    ab = marke()
    if not senden('öffnen', w):
        return
    pruefe(warte(r'^Entwurf: folder://\S+ \S+ als gelöscht markiert \(abgeglichen\)$', ab, 60) is not None,
           'öffnen: nach dem Senden ist der Entwurf gelöscht (Haken für #362)')
    m = email.message_from_bytes(open(os.path.join(OUT, '%s-öffnen.eml' % NAME), 'rb').read(), policy=email.policy.default)
    text = teile(m).get('text/plain', '').replace('\xa0', ' ')
    pruefe(str(m['Subject']) == ENTWURF + ' ' + art and adressen(m['From']) == [konto],
           'öffnen: Subject %s, From %s' % (m['Subject'], m['From']))
    pruefe(str(m['X-Evolution-Identity']) == identitaet(konto), 'öffnen: X-Evolution-Identity %s' % m['X-Evolution-Identity'])
    pruefe(m['X-Evolution-Draft-Message'] is not None and str(m['X-Evolution-Draft-Folder']).startswith('folder://'),
           'öffnen: X-Evolution-Draft-Folder/-Message %s %s' % (m['X-Evolution-Draft-Folder'], m['X-Evolution-Draft-Message']))
    pruefe('Erste Fassung zweite Fassung dritte Fassung' in text, 'öffnen: Text aller Fassungen (%s)' % text.strip()[:80])
    entwuerfe(konto, 0, 'öffnen')


# ------------------------------------------------------------------ #361 Adressvervollständigung

KONTAKT = 'Revolution-Test Kontakt 361 <HILFS_KONTAKT361@example.invalid>'
KONTAKT_2 = 'Revolution-Test Kontakt 361 <HILFS_KONTAKT361B@example.invalid>'


def vorschlaege():
    """Zeilen der Vorschlagsliste (AT-SPI „list item“ unter „Adressvorschläge“): [(Name, ausgewählt, Mitte)]."""
    desktop = Atspi.get_desktop(0)
    out = []
    for i in range(desktop.get_child_count()):
        a = desktop.get_child_at_index(i)
        if a is None or a.get_process_id() != app_pid:
            continue
        for o in knoten(a):
            if o.get_role_name() == 'list' and o.get_name() == 'Adressvorschläge':
                for j in range(o.get_child_count()):
                    z = o.get_child_at_index(j)
                    e = z.get_extents(Atspi.CoordType.SCREEN)
                    if e.width > 0 and e.height > 0:
                        out.append((z.get_name(), z.get_state_set().contains(Atspi.StateType.SELECTED),
                                    (e.x + e.width // 2, e.y + e.height // 2)))
    return out


def feld_text(titel, name):
    f = composer_frame(titel)
    for o in knoten(f) if f else []:
        if o.get_role_name() in ('text', 'entry') and o.get_name() == name:
            return Atspi.Text.get_text(o, 0, Atspi.Text.get_character_count(o))
    return None


def vervollstaendigen():
    """#361: Strg+N; „Re“ (2 Zeichen) sucht nicht, „Rev“ zeigt den Testkontakt (tests/testdaten.py --kontakt anlegen,
    Google-Adressbuch von KONTO_GOOGLE@example.invalid); Pfeile, Escape, Enter in An, Mausklick in Kopie; Senden (Test)
    trägt „Name <Adresse>“ in To und Cc."""
    ab = marke()
    zeiger(HAUPT)
    taste('Control_L+n')
    pruefe(warte(r'^Composer: Neu vorbereitet', ab, 30) is not None, 'adressen: Strg+N öffnet den Composer')
    w = fenster()
    if w is None:
        falsch('adressen: kein Composer-Fenster')
        return
    buch = os.environ['ADRESSBUCH']
    pruefe(warte(r'^Adressvervollständigung: %s verbunden' % re.escape(buch), ab, 60) is not None,
           'adressen: Google-Adressbuch %s verbunden' % buch)
    titel = w.get_wm_name()
    time.sleep(1)
    zeiger(COMPOSER)
    popups = len(aufklapper())
    a = marke()
    tippe('Re')
    time.sleep(1.5)
    pruefe(warte(r'^Adressvervollständigung: \d+ Vorschläge', a, 1) is None and len(aufklapper()) == popups,
           'adressen: 2 Zeichen suchen nicht (Evolution minimum_query_length 3)')
    a = marke()
    tippe('v')
    z = warte(r'^Adressvervollständigung: \d+ Vorschläge', a, 30)
    wert('adressen: %s' % z)
    time.sleep(1)
    liste = vorschlaege()
    wert('adressen: Liste %s' % [(n, s) for n, s, _ in liste])
    pruefe(len(aufklapper()) > popups and {KONTAKT, KONTAKT_2} <= {n for n, _, _ in liste},
           'adressen: „Rev“ zeigt beide Adressen des Testkontakts (%d Zeilen)' % len(liste))
    bild('vorschlaege', w)
    pruefe(liste and liste[0][1], 'adressen: erste Zeile ausgewählt')
    taste('Down', 0.5)
    soll = 1 % max(len(liste), 1)
    pruefe([s for _, s, _ in vorschlaege()].index(True) == soll if vorschlaege() else False,
           'adressen: Pfeil runter wählt Zeile %d' % soll)
    taste('Up', 0.5)
    pruefe(vorschlaege() and vorschlaege()[0][1], 'adressen: Pfeil hoch wählt Zeile 0')
    taste('Escape', 0.8)
    pruefe(len(aufklapper()) == popups and fenster() is not None,
           'adressen: Escape schließt nur die Liste, der Composer bleibt')
    # Weitertippen grenzt ein; zur Zeile des Testkontakts mit Pfeil runter, Enter setzt ein.
    a = marke()
    tippe('olution-Test Kontakt')
    warte(r'^Adressvervollständigung: \d+ Vorschläge', a, 30)
    time.sleep(1)
    for _ in range(len(vorschlaege())):
        if any(n == KONTAKT and s for n, s, _ in vorschlaege()):
            break
        taste('Down', 0.3)
    taste('Return', 0.8)
    text = feld_text(titel, 'An')
    pruefe(text == KONTAKT + ', ', 'adressen: Enter setzt „Name <Adresse>, “ in An ein (%r)' % text)
    pruefe(len(aufklapper()) == popups, 'adressen: Liste nach Enter geschlossen')
    # Kopie (Tab aus An): Auswahl per Mausklick auf die Zeile. AT-SPI liefert für Zeilen im Popover keine
    # brauchbaren Bildschirmkoordinaten (gemessen: y=17); Zeile i liegt bei (i + ½) / n der Popup-Höhe.
    taste('Tab', 0.5)
    vorher = {p.id for p in aufklapper()}
    a = marke()
    tippe('kontakt361')
    warte(r'^Adressvervollständigung: \d+ Vorschläge', a, 30)
    time.sleep(1)
    namen = [n for n, _, _ in vorschlaege()]
    popup = [p for p in aufklapper() if p.id not in vorher]
    pruefe(KONTAKT in namen and len(popup) == 1, 'adressen: Liste unter Kopie zeigt den Testkontakt (%s)' % namen)
    if KONTAKT in namen and len(popup) == 1:
        bild('kopie', w)
        g = popup[0].get_geometry()
        klick((g.x + g.width // 2, g.y + int(g.height * (namen.index(KONTAKT) + 0.5) / len(namen))))
    text = feld_text(titel, 'Kopie')
    pruefe(text == KONTAKT + ', ', 'adressen: Klick setzt „Name <Adresse>, “ in Kopie ein (%r)' % text)
    taste('Tab', 0.2)
    taste('Tab', 0.2)
    tippe('[Revolution-Test] Adressen 361')
    if not senden('adressen', w):
        return
    m = email.message_from_bytes(open(os.path.join(OUT, '%s-adressen.eml' % NAME), 'rb').read(),
                                 policy=email.policy.default)
    for kopf in ('To', 'Cc'):
        pruefe(email.utils.getaddresses([str(m[kopf])]) == [('Revolution-Test Kontakt 361',
                                                              'HILFS_KONTAKT361@example.invalid')],
               'adressen: MIME %s %s' % (kopf, m[kopf]))


def escape():
    ab = marke()
    zeiger(HAUPT)
    taste('Control_L+n')
    warte(r'^Composer: Neu vorbereitet', ab, 30)
    w = fenster()
    time.sleep(1)
    zeiger(COMPOSER)
    taste('Escape', 0.5)
    pruefe(w is not None and fenster(weg=True), 'Escape schließt den Composer')


def main():
    global app_pid
    eigen = eigene_adressen()
    wert('eigene Adressen (Identitäten): %s' % sorted(eigen))
    log = open(LOG, 'w')
    if os.environ.get('M365_DEBUG') == '1':
        # #360: Graph-Anfragen nur als „[graph] METHODE Pfad“ (ohne Kopfzeilen, kein Token im Log), Muster
        # tests/verschieben.sh FILTER (#416).
        app = subprocess.Popen([BIN], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')

        def filtern():
            for zeile in app.stdout:
                g = re.match(r'^> (GET|POST|PATCH|PUT|DELETE|HEAD) (\S+)', zeile)
                if g:
                    log.write('[graph] %s %s\n' % (g.group(1), g.group(2).split('?')[0]))
                elif zeile.strip() and not zeile.startswith(('>', '<', ' ')) and 'Bearer' not in zeile:
                    log.write(zeile)
                log.flush()
        threading.Thread(target=filtern, daemon=True).start()
    else:
        app = subprocess.Popen([BIN], stdout=log, stderr=subprocess.STDOUT)
    app_pid = app.pid
    try:
        w = fenster('Revolution', zeit=60)
        if w is None:
            falsch('kein Fenster')
            return 1
        w.configure(width=1200, height=760)
        d.sync()
        if warte(r'^Nachricht: fertig ', 0, 120) is None:
            falsch('Testmail nicht angezeigt (%s)' % ORDNER)
            return 1
        ok('Testmail angezeigt: %s' % ORDNER)
        time.sleep(1)
        if '-weiterleiten-' in NAME:
            weiterleiten()
        elif '-adressen-' in NAME:
            vervollstaendigen()
        elif NAME.endswith('-hell'):
            antwort('antworten', 'Control_L+r', False, eigen)
            antwort('allen', 'Control_L+Shift_L+r', True, eigen)
            neu()
            escape()
        elif NAME.endswith('-a11y'):
            a11y()
        elif NAME.endswith('-entwurf'):
            entwurf_neu()
        elif NAME.endswith('-entwurf-imap'):
            entwurf_oeffnen(KONTO, 'IMAP')
        elif NAME.endswith('-entwurf-graph'):
            entwurf_oeffnen('KONTO_M365@example.invalid', 'Graph')
        elif NAME.endswith('-eigen'):
            antwort_eigen('eigen', 3, ['HILFS_JANA@example.invalid'], ['HILFS_LENA@example.invalid'],
                  'Re: [Revolution-Test] Eigene Mail im Eingang', 'Absender KONTO_IMAP_2@example.invalid ist eigene Adresse')
        elif NAME.endswith('-eigen-konversation'):
            antwort_eigen('eigen-konversation', 2, ['HILFS_JANA@example.invalid'],
                  ['HILFS_LENA@example.invalid', 'HILFS_TOM@example.invalid'],
                  'Re: [Revolution-Test] Eigene Antwort Kisten', 'Nachricht aus Gesendet')
        elif NAME.endswith('-konversation'):
            konversation()
        else:
            antwort('allen', 'Control_L+Shift_L+r', True, eigen)
        w.send_event(protocol.event.ClientMessage(
            window=w, client_type=d.intern_atom('WM_PROTOCOLS'),
            data=(32, [d.intern_atom('WM_DELETE_WINDOW'), X.CurrentTime, 0, 0, 0])), event_mask=X.NoEventMask)
        d.sync()
        try:
            rc = app.wait(30)
            pruefe(rc == 0, 'Beenden: Exit %d' % rc)
        except subprocess.TimeoutExpired:
            falsch('Beenden: nach 30 s nicht beendet')
    finally:
        if app.poll() is None:
            app.kill()
            app.wait()
        log.close()
    rest = [z.strip() for z in open(LOG, encoding='utf-8', errors='replace')
            if z.strip() and not (z.startswith('(revolution:') and 'revolution-DEBUG' in z)
            and not z.startswith('libEGL warning:') and not z.startswith('[graph] ')]
    pruefe(not rest, 'Log: %s' % ('%d unerwartete Zeilen: %s' % (len(rest), rest[:3]) if rest else 'nur DEBUG-Zeilen'))
    return 1 if fehler else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as e:  # #411: Abbruch zählt als FEHLER
        traceback.print_exc()
        falsch('Abbruch: %s: %s' % (type(e).__name__, str(e).replace('\n', ' ')[:300]))
        sys.exit(1)
