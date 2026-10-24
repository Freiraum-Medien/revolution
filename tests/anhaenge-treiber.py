#!/usr/bin/env python3
# anhaenge-treiber.py – Treiber für tests/anhaenge.sh (#356), läuft im Xvfb aus xvfb-run.
#
# Startet die App (BIN) unter strace (execve, alle Kinder) mit Log, bedient die Anhangleiste per
# XTest (python3-xlib) und prüft jede geöffnete und gespeicherte Datei per sha256 gegen die Teile
# vom Server (TEILE, Ausgabe von tests/testdaten.py --anhang-teile). Umgebung: siehe tests/anhaenge.sh.
# Ausgabe: „OK …“, „FEHLER …“, „WERT …“; Screenshots OUT/NAME-<Schritt>.png.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

import hashlib
import os
import re
import stat
import subprocess
import sys
import time
import traceback

from PIL import Image
from Xlib import X, XK, display, protocol
from Xlib.ext import xtest

BIN, OUT, NAME = os.environ['BIN'], os.environ['OUT'], os.environ['NAME']
ORDNER, TEILE, GEOEFFNET, ZIEL = os.environ['ORDNER'], os.environ['TEILE'], os.environ['GEOEFFNET'], os.environ['ZIEL']
UID = ORDNER.split('/', 1)[0]
CACHE = os.path.join(os.path.expanduser('~'), '.cache', 'revolution', 'attachments')
# Fenster 1200×760: Pillen der Anhangleiste (Screenshot build/m4-356/probe.png).
PDF = (700, 171)
PNG = (950, 171)

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
ZEILE_RE = re.compile(r'^\(revolution:\d+\): revolution-DEBUG: [\d:.]+: (.*)$')


def log_zeilen():
    with open(LOG, encoding='utf-8', errors='replace') as f:
        return [m.group(1) for m in (ZEILE_RE.match(z.rstrip('\n')) for z in f) if m]


def marke():
    return len(log_zeilen())


def warte(muster, ab, zeit=30):
    rx = re.compile(muster)
    ende = time.monotonic() + zeit
    while time.monotonic() < ende:
        for text in log_zeilen()[ab:]:
            m = rx.search(text)
            if m:
                return m
        time.sleep(0.2)
    return None


# ------------------------------------------------------------------ Server-Teile

def server_teile():
    """{sha256: Name} aus tests/testdaten.py --anhang-teile."""
    out = {}
    for zeile in open(TEILE, encoding='utf-8'):
        m = re.match(r'^TEIL(?:-TEXT)? (\w+) (\d+) (.*)$', zeile.rstrip('\n'))
        if m:
            out[m.group(1)] = m.group(3)
    return out


def sha(pfad):
    with open(pfad, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


# ------------------------------------------------------------------ XTest

def taste(kombi, pause=0.4):
    codes = [d.keysym_to_keycode(XK.string_to_keysym(k)) for k in kombi.split('+')]
    if 0 in codes:
        raise RuntimeError('Unbekannte Taste: %s' % kombi)
    for c in codes:
        xtest.fake_input(d, X.KeyPress, c)
    for c in reversed(codes):
        xtest.fake_input(d, X.KeyRelease, c)
    d.sync()
    time.sleep(pause)


ZEICHEN = {'/': 'slash', '.': 'period', '-': 'minus', '_': 'underscore', ' ': 'space', ':': 'colon'}


def tippen(text):
    """Text Zeichen für Zeichen; Umschalt, wenn das Zeichen auf der zweiten Ebene liegt."""
    shift = d.keysym_to_keycode(XK.string_to_keysym('Shift_L'))
    for z in text:
        ks = XK.string_to_keysym(ZEICHEN.get(z, z))
        code = d.keysym_to_keycode(ks)
        if code == 0:
            raise RuntimeError('Zeichen nicht tippbar: %r' % z)
        hoch = d.keycode_to_keysym(code, 0) != ks
        if hoch:
            xtest.fake_input(d, X.KeyPress, shift)
        xtest.fake_input(d, X.KeyPress, code)
        xtest.fake_input(d, X.KeyRelease, code)
        if hoch:
            xtest.fake_input(d, X.KeyRelease, shift)
        d.sync()
        time.sleep(0.01)
    time.sleep(0.3)


def klick(p, knopf=1, anzahl=1):
    xtest.fake_input(d, X.MotionNotify, x=p[0], y=p[1])
    for _ in range(anzahl):
        xtest.fake_input(d, X.ButtonPress, knopf)
        xtest.fake_input(d, X.ButtonRelease, knopf)
        d.sync()
        time.sleep(0.05)
    time.sleep(0.6)


def bild(schritt):
    xtest.fake_input(d, X.MotionNotify, x=0, y=0)
    d.sync()
    time.sleep(0.4)
    root = d.screen().root
    g = root.get_geometry()
    raw = root.get_image(0, 0, g.width, g.height, X.ZPixmap, 0xffffffff).data
    pfad = os.path.join(OUT, '%s-%s.png' % (NAME, schritt))
    Image.frombytes('RGB', (g.width, g.height), raw, 'raw', 'BGRX').save(pfad)
    wert('Screenshot %s' % pfad)


def fenster(titel, zeit=20):
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


# ------------------------------------------------------------------ Schritte

def oeffnen(schritt, bedienung, dateiname, teile):
    ab = marke()
    zeilen_vorher = len(open(GEOEFFNET).read().splitlines())
    bedienung()
    m = warte(r'^Anhang: (.*) geöffnet aus (.*)$', ab, 30)
    if m is None or m.group(1) != dateiname:
        falsch('%s: Log „Anhang: %s geöffnet aus …“ fehlt (%s)' % (schritt, dateiname, m.group(0) if m else '-'))
        return
    pfad = m.group(2)
    ok('%s: %s' % (schritt, m.group(0)))
    # Test-Handler: Zeile „GEOEFFNET <sha256>  <Pfad>“.
    ende = time.monotonic() + 10
    neu = []
    while time.monotonic() < ende and not neu:
        neu = open(GEOEFFNET).read().splitlines()[zeilen_vorher:]
        time.sleep(0.2)
    if len(neu) != 1 or not neu[0].endswith('  ' + pfad):
        falsch('%s: Test-Handler bekam nicht genau diese Datei: %s' % (schritt, neu))
        return
    summe = neu[0].split()[1]
    ok('%s: Test-Handler bekam %s' % (schritt, pfad))
    (ok if summe in teile else falsch)('%s: sha256 %s %s Server-Teil %s' % (
        schritt, summe[:16], '=' if summe in teile else '≠', teile.get(summe, '-')))
    ordner = os.path.dirname(pfad)
    rechte_datei = stat.S_IMODE(os.stat(pfad).st_mode)
    rechte_ordner = stat.S_IMODE(os.stat(ordner).st_mode)
    (ok if os.path.dirname(ordner) == CACHE and rechte_datei == 0o600 and rechte_ordner == 0o700 else falsch)(
        '%s: Datei unter %s, Rechte Datei %o, Ordner %o' % (schritt, os.path.dirname(ordner), rechte_datei, rechte_ordner))
    return pfad


def anfang():
    """Fokus und Auswahl auf die erste Pille (Pos1 scrollt die Leiste an den Anfang)."""
    klick(PNG)
    taste('Home', 0.8)


def speichern_unter(teile):
    """PDF: Rechtsklick → „Speichern unter …“, Pfad im GTK-Dialog, dann dasselbe Ziel noch einmal
    (GTK fragt nach dem Ersetzen; Abbrechen lässt die Datei unverändert)."""
    ziel = os.path.join(ZIEL, 'einzeln', 'lageplan-gespeichert.pdf')
    for runde in (1, 2):
        ab = marke()
        anfang()
        klick(PDF, knopf=3)
        if runde == 1:
            bild('4-kontextmenue')
        # GtkPopoverMenu fokussiert beim Öffnen den ersten Eintrag („Öffnen“).
        taste('Down')
        if runde == 1:
            bild('4b-kontextmenue-auswahl')
        taste('Return')
        w = fenster('Anhang speichern')
        if w is None:
            falsch('Speichern unter (%d): Dialog „Anhang speichern“ nicht erschienen' % runde)
            bild('fehler-speichern-unter')
            taste('Escape', 1)
            return
        time.sleep(1)
        if runde == 1:
            bild('5-speichern-dialog')
        taste('Control_L+a')
        tippen(ziel)
        taste('Return', 1.5)
        if runde == 1:
            m = warte(r'^Anhänge: 1 gespeichert als (.*)$', ab, 30)
            if m is None or m.group(1) != ziel:
                falsch('Speichern unter: Log „Anhänge: 1 gespeichert als %s“ fehlt (%s)' % (ziel, m.group(0) if m else '-'))
                return
            summe = sha(ziel)
            (ok if summe in teile else falsch)('Speichern unter: %s sha256 %s %s Server-Teil %s' % (
                ziel, summe[:16], '=' if summe in teile else '≠', teile.get(summe, '-')))
            vorher = os.stat(ziel)
        else:
            # Rückfrage von GTK („… existiert bereits“) über dem Dialog: Abbrechen, dann Dialog schließen.
            bild('6-ersetzen-rueckfrage')
            taste('Escape', 1)
            taste('Escape', 1)
            m = warte(r'^Anhänge: \d+ gespeichert', ab, 3)
            nachher = os.stat(ziel)
            (ok if m is None and (nachher.st_mtime_ns, nachher.st_ino) == (vorher.st_mtime_ns, vorher.st_ino) else falsch)(
                'Speichern unter auf vorhandene Datei, Rückfrage abgebrochen: Datei unverändert (mtime, Inode), kein Speichern im Log')
            (ok if fenster('Anhang speichern', 2) is None else falsch)('Dialog nach Abbrechen geschlossen')


def alle_speichern(teile, anzahl):
    """Menü-Taste an der Pille mit dem Fokus → „Alle speichern …“, Ordner im GTK-Dialog; zweimal in
    denselben Ordner, dann in einen schreibgeschützten Ordner (jeder Anhang schlägt fehl: die App
    versucht alle und meldet die Fehler gesammelt)."""
    ordner = os.path.join(ZIEL, 'alle')
    gesperrt = os.path.join(ZIEL, 'gesperrt')
    os.makedirs(gesperrt, exist_ok=True)
    os.chmod(gesperrt, 0o500)
    for runde, ziel_r in ((1, ordner), (2, ordner), (3, gesperrt)):
        ab = marke()
        anfang()
        taste('Menu')
        if runde == 1:
            bild('7-menue-taste')
        taste('Down')
        taste('Down')
        taste('Return')
        w = fenster('Alle Anhänge speichern')
        if w is None:
            falsch('Alle speichern (%d): Dialog „Alle Anhänge speichern“ nicht erschienen' % runde)
            bild('fehler-alle-speichern')
            taste('Escape', 1)
            return
        time.sleep(1)
        taste('Control_L+l')
        tippen(ziel_r)
        # Die Ortszeile ergänzt beim Tippen nachlaufend (Vervollständigung des Ordners); Rest hinter
        # der Schreibmarke entfernen.
        time.sleep(0.5)
        taste('Shift_L+End')
        taste('Delete')
        if runde == 1:
            bild('8-ordner-dialog')
        taste('Return', 1.5)
        if runde == 1:
            bild('8b-ordner-dialog-eingabe')
        if fenster('Alle Anhänge speichern', 1) is not None:
            taste('Return', 1.5)
            if runde == 1:
                bild('8c-ordner-dialog-eingabe2')
        m = warte(r'^Anhänge: (\d+) gespeichert im Ordner (.*)$', ab, 30)
        if runde == 3:
            time.sleep(1)
            bild('8d-gesperrt-banner')
            warnung = [z for z in open(LOG, encoding='utf-8', errors='replace') if 'Anhang nicht gespeichert' in z]
            namen = ('Lageplan Übersicht.pdf', 'Marktstand.png', 'Unterlagen.zip', 'revolution-pfad.txt', 'Weitergeleitet')
            gesammelt = warnung and '%d von %d nicht gespeichert' % (anzahl, anzahl) in warnung[-1] and all(
                n in warnung[-1] for n in namen)
            (ok if m is not None and m.group(1) == '0' and gesammelt else falsch)(
                'Alle speichern in schreibgeschützten Ordner: alle %d versucht, Fehler gesammelt: %s' % (
                    anzahl, warnung[-1].strip()[:300] if warnung else '-'))
            os.chmod(gesperrt, 0o700)
            continue
        if m is None or m.group(2) != ordner or int(m.group(1)) != anzahl:
            falsch('Alle speichern (%d): Log „Anhänge: %d gespeichert im Ordner %s“ fehlt (%s)' % (
                runde, anzahl, ordner, m.group(0) if m else '-'))
            continue
        ok('Alle speichern (%d): %s' % (runde, m.group(0)))
        pfade = re.findall(r'^Anhang: .* gespeichert unter (.*)$', '\n'.join(log_zeilen()[ab:]), re.M)
        for pfad in pfade:
            summe = sha(pfad)
            drin = os.path.dirname(pfad) == ordner
            # Angehängte Nachricht: Camel schreibt die Nachricht neu (Kopfzeilen), der Server-Teil
            # aus Python ist eine andere Serialisierung; nur als Messwert.
            pruefe = ok if drin and summe in teile else (wert if drin and 'Weitergeleitet' in pfad else falsch)
            pruefe('Alle speichern (%d): %s sha256 %s %s Server-Teil %s' % (
                runde, os.path.relpath(pfad, ZIEL), summe[:16], '=' if summe in teile else '≠', teile.get(summe, '-')))
    namen = sorted(os.listdir(ordner))
    wert('Ordner nach zwei Läufen: %s' % namen)
    zweite = [n for n in namen if ' (1)' in n]
    (ok if len(namen) == 2 * anzahl and len(zweite) == anzahl else falsch)(
        'Namenskonflikt wie Evolution: %d Dateien, davon %d mit „ (1)“' % (len(namen), len(zweite)))
    wert('Dateiname mit „../“ im Ordner gespeichert als: %s' % [n for n in namen if 'pfad' in n])
    aussen = [n for n in os.listdir(os.path.dirname(ZIEL)) if 'pfad' in n] + [n for n in os.listdir(ZIEL) if 'pfad' in n]
    (ok if not aussen else falsch)('nichts außerhalb des Zielordners geschrieben: %s' % aussen)


def lauf():
    teile = server_teile()
    wert('Server-Teile: %s' % sorted(teile.values()))
    m = warte(r'^Nachricht: %s \S+ \S+ geladen nach .* (\d+) Anhänge\)$' % re.escape(UID), 0, 120)
    if m is None:
        falsch('Anhang-Mail nicht geladen (Log „Nachricht: %s … Anhänge“)' % UID)
        return
    anzahl = int(m.group(1))
    (ok if anzahl == 5 else falsch)('Anhang-Mail geladen, %d Anhänge (PDF, PNG, ZIP, Text, Nachricht): %s' % (anzahl, m.group(0)))
    if warte(r'^Nachricht: sichtbar', 0, 30) is None:
        falsch('Nachricht nicht sichtbar')
    time.sleep(1.5)
    bild('1-anhangleiste')

    pfade = []

    def eingabe(p):
        def tu():
            klick(p)
            taste('Return')
        return tu

    def rechts_eingabe():
        klick(PNG)
        taste('Right')
        taste('Return')

    pfade.append(oeffnen('PDF öffnen (Klick, Eingabe)', eingabe(PDF), 'Lageplan Übersicht.pdf', teile))
    pfade.append(oeffnen('PNG öffnen (Doppelklick)', lambda: klick(PNG, anzahl=2), 'Marktstand.png', teile))
    pfade.append(oeffnen('ZIP öffnen (Pfeil rechts, Eingabe)', rechts_eingabe, 'Unterlagen.zip', teile))
    bild('2-nach-oeffnen')
    speichern_unter(teile)
    alle_speichern(teile, anzahl)
    bild('9-ende')
    # Temporäre Dateien dieses Laufs wieder entfernen (Arnds Cache).
    for pfad in filter(None, pfade):
        os.unlink(pfad)
        os.rmdir(os.path.dirname(pfad))


def main():
    log = open(LOG, 'w')
    strace = os.path.join(OUT, NAME + '-strace.txt')
    app = subprocess.Popen(['strace', '-f', '-qq', '-e', 'trace=execve', '-s', '300', '-o', strace, BIN],
                           stdout=log, stderr=subprocess.STDOUT)
    try:
        w = fenster('Revolution', 30)
        if w is None:
            falsch('kein Fenster')
            return 1
        w.configure(width=1200, height=760)
        d.sync()
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

    # strace: gestartete Programme (execve) der App und aller Kinder.
    programme = {}
    for zeile in open(strace, encoding='utf-8', errors='replace'):
        m = re.match(r'^\d+ +execve\("([^"]+)", \[(.*?)\]', zeile)
        if m and not re.search(r'= -1 E[A-Z]+', zeile):
            programme.setdefault(m.group(1), []).append(m.group(2)[:200])
    for prog, aufrufe in sorted(programme.items()):
        wert('execve %s ×%d: %s' % (prog, len(aufrufe), aufrufe[0]))
    handler = [a for p, aa in programme.items() for a in aa if p.endswith('/xdg/handler.sh')]
    (ok if len(handler) == 3 else falsch)('strace: Test-Handler %d× gestartet (PDF, PNG, ZIP)' % len(handler))
    # gio-launch-desktop: Starthilfe von GLib für Programme aus .desktop-Dateien (startet den Handler).
    erlaubt = (BIN, '/usr/lib/x86_64-linux-gnu/glib-2.0/gio-launch-desktop', '/usr/bin/bwrap', '/usr/bin/xdg-dbus-proxy', '/bin/sh', '/usr/bin/sh', '/usr/bin/sha256sum',
               '/usr/bin/strace')
    fremd = [p for p in programme if not (p in erlaubt or p.endswith('/xdg/handler.sh') or '/webkitgtk-6.0/' in p
                                          or p.startswith('/usr/libexec/webkitgtk-6.0/'))]
    (ok if not fremd else falsch)('strace: keine anderen Programme gestartet%s' % (': %s' % fremd if fremd else ''))

    rest = [z for z in open(LOG, encoding='utf-8', errors='replace')
            if not z.startswith('(revolution:') and not z.startswith('libEGL warning:') and z.strip()]
    # Erwartet: die gesammelte Fehlermeldung aus „Alle speichern“ in den schreibgeschützten Ordner.
    andere = [z for z in open(LOG, encoding='utf-8', errors='replace')
              if z.startswith('(revolution:') and 'revolution-DEBUG' not in z
              and not ('revolution-WARNING' in z and 'Anhang nicht gespeichert' in z and '/gesperrt:' in z)]
    rest += andere
    (falsch if rest else ok)('Log: %s' % ('%d unerwartete Zeilen: %s' % (len(rest), rest[:3]) if rest else 'nur DEBUG-Zeilen'))
    return 1 if fehler else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as e:  # #411: Abbruch (Traceback, Zeitlimit eines Aufrufs) zählt als FEHLER
        traceback.print_exc()
        falsch('Abbruch: %s: %s' % (type(e).__name__, str(e).replace('\n', ' ')[:300]))
        sys.exit(1)
