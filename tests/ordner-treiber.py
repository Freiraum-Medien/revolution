#!/usr/bin/env python3
# ordner-treiber.py – Treiber für tests/ordner.sh (#364), läuft im Xvfb aus xvfb-run.
#
# Startet die App (BIN, nur das Konto UID, Posteingang gewählt) und bedient den Ordnerbaum per Maus (XTest,
# Zeilen über AT-SPI) und Tastatur: Spezialordner Eingang (nur „Neuer Ordner…“, F2/Entf ohne Dialog), dann
# Revolution-Ordnertest anlegen (oberste Ebene), umbenennen (F2), Unterordner anlegen (Kontextmenü), löschen
# (Entf, Bestätigung); landet der Ordner im Papierkorb (microsoft365), dort ein zweites Mal löschen. Nach jedem
# Schritt gleich: Server (tests/testdaten.py --ordnertest KONTO), Ordnerbaum der App (Log „Ordner: UID …“) und
# sichtbare Zeilen der Liste „Ordner“ (AT-SPI); #435: beim Start ausgeblendete virtuelle Papierkorb/Spam erscheinen
# nicht wieder (Log, je eine Zeile „Papierkorb“/„Spam“). Bild des Kontextmenüs OUT/NAME-menue.png.
# Umgebung: BIN, OUT, NAME, UID, KONTO, TESTDATEN, TIMEOUT. Ausgabe: „OK …“, „FEHLER …“, „WERT …“.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import re
import subprocess
import sys
import threading
import time
import traceback

import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi  # noqa: E402
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from testkonten import KONTO_M365  # noqa: E402
from PIL import Image  # noqa: E402
from Xlib import X, XK, display, protocol  # noqa: E402
from Xlib.ext import xtest  # noqa: E402

BIN, OUT, NAME, UID, KONTO = (os.environ[k] for k in ('BIN', 'OUT', 'NAME', 'UID', 'KONTO'))
TIMEOUT = int(os.environ.get('TIMEOUT', '180'))
LOG = os.path.join(OUT, NAME + '.log')
N, R, S = 'Revolution-Ordnertest', 'Revolution-Ordnertest-umbenannt', 'Revolution-Ordnertest-Unterordner'
R2 = 'Revolution-Ordnertest-Umbenannt'
TEST = re.compile(r'(^|/)Revolution-Ordnertest')

fehler = 0
d = display.Display()
app = None
fenster_x = fenster_y = 0


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


def warte(muster, ab, zeit=TIMEOUT):
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
    time.sleep(0.3)


def tippe(text):
    for ch in text:
        taste({'-': 'minus'}.get(ch, ch)) if not ch.isupper() else taste('Shift_L', ch)


def klick(o, knopf=1):
    """Klick in die Zeile o. Zeilen der Liste „Ordner“: AT-SPI-Lage relativ zur Liste (PARENT) plus Lage der Liste
    im Fenster; GTK meldet die Liste (WINDOW) unter Xvfb 10/17 px links/oberhalb ihrer gezeichneten Lage (gemessen
    am Bild, build/m5-364*/…-menue.png). Danach prüft der Treiber die Auswahl (SELECTED)."""
    e = o.get_extents(Atspi.CoordType.PARENT)
    liste = o.get_parent().get_extents(Atspi.CoordType.WINDOW)
    xtest.fake_input(d, X.MotionNotify, x=fenster_x + liste.x + 10 + e.x + e.width // 2,
                     y=fenster_y + liste.y + 17 + e.y + e.height // 2)
    xtest.fake_input(d, X.ButtonPress, knopf)
    xtest.fake_input(d, X.ButtonRelease, knopf)
    d.sync()
    time.sleep(1)
    if not o.get_state_set().contains(Atspi.StateType.SELECTED):
        falsch('Klick auf „%s“: Zeile nicht gewählt' % o.get_name())


def bild(schritt):
    root = d.screen().root
    g = root.get_geometry()
    pfad = os.path.join(OUT, '%s-%s.png' % (NAME, schritt))
    Image.frombytes('RGB', (g.width, g.height), root.get_image(0, 0, g.width, g.height, X.ZPixmap,
                                                               0xffffffff).data, 'raw', 'BGRX').save(pfad)
    wert('Bild %s' % pfad)


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


def schliessen(w):
    w.send_event(protocol.event.ClientMessage(
        window=w, client_type=d.intern_atom('WM_PROTOCOLS'),
        data=(32, [d.intern_atom('WM_DELETE_WINDOW'), X.CurrentTime, 0, 0, 0])), event_mask=X.NoEventMask)
    d.sync()
    try:
        return app.wait(30)
    except subprocess.TimeoutExpired:
        return None


# ------------------------------------------------------------------ AT-SPI

def wurzel():
    desktop = Atspi.get_desktop(0)
    for i in range(desktop.get_child_count()):
        a = desktop.get_child_at_index(i)
        if a is not None and a.get_process_id() == app.pid:
            return a
    return None


def alle(o, rolle, out):
    if o.get_role_name() == rolle and o.get_state_set().contains(Atspi.StateType.SHOWING):
        out.append(o)
    for i in range(o.get_child_count()):
        k = o.get_child_at_index(i)
        if k:
            alle(k, rolle, out)
    return out


def zeilen():
    """Sichtbare Zeilen der Liste „Ordner“: [(Anzeigename, Objekt)]."""
    liste = [o for o in alle(wurzel(), 'list', []) if o.get_name() == 'Ordner']
    return [(z.get_name().split(', ')[0], z) for z in alle(liste[0], 'list item', [])] if liste else []


def zeile(name):
    """Letzte Zeile mit diesem Namen (Eingang: die des Kontos, nicht die der Favoriten)."""
    treffer = [z for n, z in zeilen() if n == name]
    return treffer[-1] if treffer else None


def menue_eintraege():
    """Zahl der sichtbaren Einträge des offenen Kontextmenüs (GtkPopoverMenu meldet keine Namen über AT-SPI)."""
    return len(alle(wurzel(), 'menu item', []))


def dialog_offen():
    return bool(alle(wurzel(), 'alert', []) or alle(wurzel(), 'dialog', []))


# ------------------------------------------------------------------ Vergleich Server, Ordnerbaum, Zeilen

def baum(muster=TEST):
    """Ordnerbaum des Kontos aus dem Log: append_tree „Ordner: UID NAME“, merge_tree „neu“/„entfernt“."""
    namen = set()
    for z in log_text().splitlines():
        m = re.search(r'revolution-DEBUG: \S+: Ordner: %s (neu |entfernt )?(.*)$' % UID, z)
        if not m or re.match(r'\d+ geladen$|abgeglichen|angelegt |umbenannt |gelöscht |anlegen |umbenennen |'
                             r'löschen |\S+ nicht ', m.group(2)):
            continue
        if m.group(1) == 'entfernt ':
            namen = {n for n in namen if n != m.group(2) and not n.startswith(m.group(2) + '/')}
        else:
            namen.add(m.group(2))
    return sorted(n for n in namen if muster is None or muster.search(n))


def server():
    env = dict(os.environ, XDG_RUNTIME_DIR=os.environ.get('ECHT_XDG_RUNTIME_DIR', ''))
    aus = subprocess.run([sys.executable, os.environ['TESTDATEN'], '--ordnertest', KONTO], env=env,
                         capture_output=True, text=True, timeout=300).stdout
    return sorted(z[len('ORDNERTEST '):] for z in aus.splitlines() if z.startswith('ORDNERTEST '))


def vergleich(schritt, soll):
    """soll: erwartete Anzeigenamen der Testordner (Blätter der Pfade)."""
    time.sleep(2)
    srv, app_baum = server(), baum()
    sichtbar = sorted(n for n, _ in zeilen() if n.startswith(N))
    (ok if srv == app_baum else falsch)('%s: Server %s = Ordnerbaum %s' % (schritt, srv, app_baum))
    (ok if sorted(p.split('/')[-1] for p in srv) == sichtbar and (soll is None or sichtbar == sorted(soll)) else falsch)(
        '%s: sichtbare Zeilen %s, Server %s, erwartet %s' % (schritt, sichtbar, srv, soll))
    # #435: virtuelle .#evolution/Trash und /Junk, beim Start ausgeblendet (mail-folder.c), bleiben es.
    aus = set(re.findall(r'Spezialordner: %s virtueller Ordner (\S+) ausgeblendet$' % UID, log_text(), re.M))
    wieder = [n for n in baum(None) if n in aus]
    namen = [n for n, _ in zeilen()]
    zahl = {n: namen.count(n) for n in ('Papierkorb', 'Spam')}
    (ok if not wieder and all(v == 1 if aus else v <= 1 for v in zahl.values()) else falsch)(
        '%s: kein doppelter Papierkorb/Spam: Zeilen %s, ausgeblendet %s, wieder im Ordnerbaum %s'
        % (schritt, zahl, sorted(aus), wieder))
    return srv


# ------------------------------------------------------------------ Schritte

def menue(name, neu=False, foto=None):
    """Rechtsklick auf die Zeile name; Zahl der Einträge des Kontextmenüs. neu: erster Eintrag „Neuer Ordner…“ (hat
    den Fokus) mit Eingabe, sonst Escape."""
    z = zeile(name)
    if z is None:
        falsch('Zeile „%s“ fehlt' % name)
        return 0
    klick(z, 3)
    n = menue_eintraege()
    if foto:
        bild(foto)
    taste('Return' if neu else 'Escape')
    time.sleep(1)
    return n


def dialog(text, unterordner=None, leeren=False):
    """Namen tippen, ggf. „Unterordner von …“ umschalten, Eingabe bestätigen."""
    if not dialog_offen():
        falsch('Dialog nicht geöffnet')
        return False
    if leeren:
        taste('Control_L', 'a')
        taste('BackSpace')
    tippe(text)
    if unterordner is False:
        # „Unterordner von …“ ist vorgewählt (mail-view.c); aus: Tab, Leertaste, zurück ins Eingabefeld (das Kästchen
        # hat keine AT-SPI-Aktion, AT-SPI meldet zudem Kästchen geschlossener Dialoge weiter). Ort prüft schritt().
        taste('Tab')
        taste('space')
        taste('Shift_L', 'Tab')
    if unterordner is False:
        bild('dialog')
    taste('Return')
    return True


def schritt(name, ab, muster, soll):
    m = warte(r'Ordner: %s %s$' % (UID, muster), ab)
    (ok if m else falsch)('%s: %s' % (name, m.group(0) if m else 'kein Log „%s“ in %d s' % (muster, TIMEOUT)))
    fehl = re.search(r'Ordner: %s .* nicht (?!abonniert)\S+: (.*)$' % UID, log_text()[ab:], re.M)
    if fehl:
        falsch('%s: %s' % (name, fehl.group(1)))
    return vergleich(name, soll)


def loeschen(name, was):
    """Entf, Bestätigung (Vorgabe „Nicht löschen“, daher Tab zu „Löschen“). microsoft365 legt den Ordner in den
    Papierkorb (Graph DELETE), dort ein zweites Mal löschen."""
    for runde in (1, 2):
        z = zeile(name)
        if z is None:
            falsch('Löschen %s: Zeile „%s“ fehlt' % (was, name))
            return
        klick(z)
        ab = len(log_text())
        taste('Delete')
        time.sleep(1)
        knopf = [o for o in alle(wurzel(), 'button', []) if o.get_name() == 'Löschen']
        if not knopf:
            falsch('Löschen %s: keine Bestätigung' % was)
            return
        if runde == 1:
            bild('loeschen-' + was.replace(' ', '-'))
        # Fokus liegt auf der Vorgabe „Nicht löschen“ (Bild); Tab zu „Löschen“ (AT-SPI-Aktion wirkt dort nicht).
        taste('Tab')
        taste('Return')
        srv = schritt('Löschen %s „%s“ (%d)' % (was, name, runde), ab, r'gelöscht (\S*)', None)
        if not any(p.split('/')[-1] == name for p in srv):
            return


def pruefen():
    global R
    if not warte(r'Ordner: %s \d+ geladen' % UID, 0):
        falsch('Ordner nicht geladen in %d s' % TIMEOUT)
        return
    time.sleep(3)
    konten = re.findall(r'Mail-Konto (\S+) „.*“: angezeigt$', log_text(), re.M)
    (ok if konten == [UID] else falsch)('nur das Testkonto geladen: %s' % konten)
    if vergleich('Start', []):
        falsch('Testordner aus einem früheren Lauf am Server, Abbruch (von Hand aufräumen)')
        return

    # Spezialordner Posteingang (microsoft365: Name vom Server): nur „Neuer Ordner…“, F2 und Entf ohne Dialog.
    eingang = 'Eingang' if zeile('Eingang') else 'Posteingang'
    n = menue(eingang, foto='menue')
    (ok if n == 1 else falsch)('Kontextmenü %s: %d Eintrag (nur „Neuer Ordner…“, Bild)' % (eingang, n))
    # Prüf-Lauf R1: Nachricht gewählt, Fokus am Spezialordner im Ordnerbaum: F2, Entf, ⌫ ändern nichts (Entf/⌫
    # gingen sonst an message.delete, #363).
    if not warte(r'Nachrichten: %s .* vollständig' % UID, 0):
        falsch('Nachrichtenliste nicht geladen')
    liste = [o for o in alle(wurzel(), 'list', []) if o.get_name() == 'Nachrichten']
    if liste:
        liste[0].select_child(0)
        time.sleep(2)
    klick(zeile(eingang))
    gewaehlt = bool(liste) and any(z.get_state_set().contains(Atspi.StateType.SELECTED)
                                   for z in alle(liste[0], 'list item', []))
    ab = len(log_text())
    for t in ('F2', 'Delete', 'BackSpace'):
        taste(t)
        time.sleep(1.5)
        (falsch if dialog_offen() else ok)('%s: %s öffnet keinen Dialog' % (eingang, t))
        if dialog_offen():
            taste('Escape')
    aktion = re.findall(r'Aktion: .*', log_text()[ab:])
    (ok if gewaehlt and not aktion else falsch)('%s: Nachricht gewählt (%s), Entf/⌫ im Ordnerbaum löschen nichts %s'
                                               % (eingang, gewaehlt, aktion[:2]))

    ab = len(log_text())
    menue(eingang, neu=True)
    dialog(N, unterordner=False)
    schritt('Anlegen „%s“' % N, ab, r'angelegt (\S*)', [N])

    # Umbenennen per F2 am gewählten neuen Ordner (die App wählt ihn nach dem Anlegen).
    klick(zeile(N))
    ab = len(log_text())
    taste('F2')
    dialog(R, leeren=True)
    schritt('Umbenennen in „%s“' % R, ab, r'umbenannt (\S*)', [R])
    if KONTO == KONTO_M365:
        # Prüf-Lauf R1: nur Groß-/Kleinschreibung (eds-compat.c lässt die Summary dann stehen).
        klick(zeile(R))
        ab = len(log_text())
        taste('F2')
        dialog(R2, leeren=True)
        schritt('Umbenennen in „%s“ (nur Großschreibung)' % R2, ab, r'umbenannt (\S*)', [R2])
        R = R2

    n = menue(R, foto='menue-ordner')
    (ok if n == 3 else falsch)('Kontextmenü %s: %d Einträge (Neuer Ordner…, Umbenennen…, Löschen…, Bild)' % (R, n))
    ab = len(log_text())
    klick(zeile(R))
    taste('Control_L', 'Shift_L', 'n')   # Kontextmenü: der Eintrag unter dem Zeiger hätte den Fokus
    dialog(S)
    schritt('Unterordner „%s“' % S, ab, r'angelegt (\S*%s/%s)' % (R, S), [R, S])

    # #363 (PM 28.10.): Unterordner allein löschen, dann neu anlegen und den Ordner mit Unterordner löschen.
    loeschen(S, 'Unterordner')
    ab = len(log_text())
    klick(zeile(R))
    taste('Control_L', 'Shift_L', 'n')   # Kontextmenü: der Eintrag unter dem Zeiger hätte den Fokus
    dialog(S)
    schritt('Unterordner „%s“ neu' % S, ab, r'angelegt (\S*%s/%s)' % (R, S), [R, S])
    loeschen(R, 'mit Unterordner')
    rest = server()
    (ok if not rest else falsch)('Ende: kein Testordner am Server (%s)' % rest)

    graph_pruefen()
    andere = [z for z in log_text().splitlines() if z.startswith('(revolution:') and 'revolution-DEBUG' not in z]
    (ok if not andere else falsch)('keine Warnungen/Criticals im Log%s' % ('' if not andere else ': %s' % andere[:3]))


def filtern(quelle, ziel):
    """Ausgabe der App ins Log; mit M365_DEBUG=1 Graph-Anfragen nur als „[graph] METHODE Pfad“, alle übrigen
    HTTP-Zeilen (Kopf mit Token, Rumpf) verworfen (Muster tests/konversationen-treiber.py)."""
    for roh in quelle:
        zeile = roh.decode('utf-8', 'replace')
        g = re.match(r'^> (GET|POST|PATCH|PUT|DELETE) (\S+)', zeile)
        if g:
            ziel.write('[graph] %s %s\n' % (g.group(1), g.group(2).split('?')[0]))
        elif zeile.startswith(('(revolution:', 'libEGL')):
            ziel.write(zeile)
        ziel.flush()


def graph_pruefen():
    """#416: Ordner anlegen/umbenennen/löschen ändert keine Nachrichten (kein PATCH an messages, Echo)."""
    anfragen = re.findall(r'^\[graph\] (\S+) (\S+)$', log_text(), re.M)
    if not anfragen:
        return
    schreibend = ['%s %s' % (m, re.sub(r'/[A-Za-z0-9_=-]{40,}', '/ID', p)) for m, p in anfragen if m != 'GET']
    wert('Graph schreibend: %s' % schreibend)
    patch = [a for a in schreibend if a.startswith('PATCH') and '/messages' in a]
    (ok if not patch else falsch)('Graph: kein PATCH an Nachrichten (Echo, #416): %s' % patch)


def main():
    global app, fenster_x, fenster_y
    log = open(LOG, 'w')
    app = subprocess.Popen([BIN], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    threading.Thread(target=filtern, args=(app.stdout, log), daemon=True).start()
    try:
        w = fenster()
        if w is None:
            falsch('kein Fenster')
            return 1
        g = w.translate_coords(d.screen().root, 0, 0)
        fenster_x, fenster_y = -g.x, -g.y
        pruefen()
        rc = schliessen(w)
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
