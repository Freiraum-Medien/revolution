#!/usr/bin/env python3
# composer-treiber.py – Treiber für tests/composer.sh (#359), läuft im Xvfb aus xvfb-run.
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
import email.utils
import os
import re
import shutil
import subprocess
import sys
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


def composer_a11y(titel):
    """Namen aller bedienbaren Elemente im Composer-Fenster (AT-SPI)."""
    desktop = Atspi.get_desktop(0)
    for i in range(desktop.get_child_count()):
        a = desktop.get_child_at_index(i)
        if a is None or a.get_process_id() != app_pid:
            continue
        for j in range(a.get_child_count()):
            f = a.get_child_at_index(j)
            if f is not None and f.get_name() == titel:
                return [(o.get_role_name(), o.get_name(), pfad(o),
                         o.get_attributes(), o.get_extents(Atspi.CoordType.WINDOW)) for o in knoten(f)]
    return []


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
    pruefe(warte(r'^Composer: Antworten vorbereitet, Von %s$' % re.escape(KONTO), ab, 60) is not None,
           'konversation: Strg+R öffnet den Composer')
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
    pruefe(str(m['Subject']) == 'Re: [Revolution-Test] Konversation Marktstand', 'konversation: Subject %s' % m['Subject'])


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
        if NAME.endswith('-hell'):
            antwort('antworten', 'Control_L+r', False, eigen)
            antwort('allen', 'Control_L+Shift_L+r', True, eigen)
            neu()
            escape()
        elif NAME.endswith('-a11y'):
            a11y()
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
