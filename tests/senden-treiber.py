#!/usr/bin/env python3
# senden-treiber.py – Treiber für tests/senden.sh (#362): Postausgang, Senden, Fehlerfall.
#
# Läuft im Xvfb im Netz-Namensraum von tests/banner-treiber.py („netz“: Relais tests/netz-relay.py,
# Netz „up“/„down“ über CTRL, einzelne Ports per nft über KAPP). Startet die App (BIN) selbst, je Fall
# mit eigenem XDG_DATA_HOME (eigener Postausgang unter OUT/daten-…), legt Testmails als .eml an und
# sendet sie über die Test-Aktion app.test-send (Datei, Identität) (REVOLUTION_TEST_HOOKS=1, main.c).
# Empfänger und Absender nur die vier Testkonten (tests/testdaten.py nur_testkonten). Serverstand
# unabhängig von Camel über das Relais: tests/testdaten.py --zustellung (IMAP/Graph, nur lesend), am Ende
# --aufraeumen (nur die Testmails dieses Laufs).
#
# PHASE
#   senden  geops@ (SMTP) → privat@, outlook.com (microsoft365) → Gmail, Gmail (SMTP) → geops@: je genau
#           einmal beim Empfänger (Posteingang oder Spam), genau eine Kopie in Gesendet des Absenders
#           (geops@: App legt sie an; microsoft365 und Gmail: der Server), Postausgang danach leer
#   netz    geops@ → privat@: (a) Port 465 gesperrt: Fehler, Banner „Nicht gesendet“, Postausgang sichtbar,
#           nichts zugestellt; Netz weg, Port frei, Netz zurück: geht automatisch raus, Banner weg; (b) Netz
#           weg beim Senden: bleibt im Postausgang (offline), Netz zurück: geht raus; (c) Port gesperrt,
#           Fehler, Port frei, F5: geht raus. Je genau einmal beim Empfänger (auch 30 s später).
#   kill    geops@ → privat@, App per SIGKILL beendet (1) nach dem Ablegen, (2) nach Beginn des Sendens,
#           (3) nach dem Senden; Neustart: Nachricht im Postausgang oder zugestellt, nie verloren,
#           höchstens einmal beim Empfänger; (2) bleibt mit „Versand unterbrochen“ im Postausgang, F5 sendet
#           sie nur, wenn sie noch nicht zugestellt ist.
# Ausgabe: „OK …“, „FEHLER …“, „WERT …“; Log OUT/NAME.log (alle Starts), Screenshots OUT/NAME-*.png.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

import email.policy
import email.utils
import importlib.util
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
import traceback
from email.message import EmailMessage

HIER = os.path.dirname(os.path.abspath(__file__))
TESTKONTEN = ('KONTO_IMAP_1@example.invalid', 'KONTO_IMAP_2@example.invalid',
              'KONTO_M365@example.invalid', 'KONTO_GOOGLE@example.invalid')
PREFIX = '[Revolution-Test]'


def relais_modul():
    spec = importlib.util.spec_from_file_location('netz_relay', os.path.join(HIER, 'netz-relay.py'))
    modul = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(modul)
    return modul


def treiber():
    from PIL import Image
    from Xlib import X, XK, display, protocol
    from Xlib.ext import xtest

    relais = relais_modul()
    BIN, OUT, NAME, PHASE = os.environ['BIN'], os.environ['OUT'], os.environ['NAME'], os.environ['PHASE']
    KAPP, CTRL, RSOCK, APP_ID = os.environ['KAPP'], os.environ['CTRL'], os.environ['RSOCK'], os.environ['APP_ID']
    # Konten: Adresse=Konto-UID=Identität=Versanddienst je Zeile in KONTEN (senden.sh).
    KONTEN = {}
    for zeile in os.environ['KONTEN'].split(';'):
        adresse, konto, ident, transport = zeile.split('=')
        KONTEN[adresse] = {'konto': konto, 'ident': ident, 'transport': transport}
    LOG = os.path.join(OUT, NAME + '.log')
    ZEILE_RE = re.compile(r'^\(revolution:\d+\): revolution-DEBUG: \S+: (.*)$')
    d = display.Display()
    fehler = [0]
    gesendet = []      # (Absender, Empfänger, Message-ID) zum Aufräumen
    app = [None]
    log = open(LOG, 'w')

    def ok(text):
        print('OK      %s: %s' % (NAME, text), flush=True)

    def falsch(text):
        fehler[0] += 1
        print('FEHLER  %s: %s' % (NAME, text), flush=True)

    def wert(text):
        print('WERT    %s: %s' % (NAME, text), flush=True)

    def log_zeilen():
        with open(LOG, encoding='utf-8', errors='replace') as f:
            return [m.group(1) for m in (ZEILE_RE.match(z.rstrip('\n')) for z in f) if m]

    def warte(muster, ab, zeit=60, takt=0.2):
        rx = re.compile(muster)
        ende = time.monotonic() + zeit
        while time.monotonic() < ende:
            for text in log_zeilen()[ab:]:
                if rx.search(text):
                    return text
            time.sleep(takt)
        return None

    def pruefe(schritt, muster, ab, zeit=60):
        treffer = warte(muster, ab, zeit)
        (ok if treffer else falsch)('%s: %s' % (schritt, treffer if treffer else 'Log „%s“ fehlt' % muster))
        return treffer

    def nicht(schritt, muster, ab):
        treffer = [t for t in log_zeilen()[ab:] if re.search(muster, t)]
        (falsch if treffer else ok)('%s: %s' % (schritt, treffer[:2] if treffer else 'kein „%s“' % muster))

    def kapp(kommando):
        c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        c.connect(KAPP)
        c.sendall(kommando.encode() + b'\n')
        antwort = c.makefile().readline().strip()
        c.close()
        (ok if antwort == 'ok ' + kommando else falsch)('Port %s: %s' % (kommando, antwort))

    def netz(kommando):
        antwort = relais.steuere(CTRL, kommando)
        (ok if antwort == 'ok ' + kommando else falsch)('Netz %s: %s' % (kommando, antwort))

    def td(*args):
        out = relais.testdaten(RSOCK, *args)
        zeilen = out.splitlines()
        if not zeilen or zeilen[-1] != 'TESTDATEN-EXIT 0':
            falsch('tests/testdaten.py %s: Abbruch: %s' % (' '.join(args), ' | '.join(zeilen[-3:])))
        return zeilen

    def zustellung(adresse, mid):
        zahl = {}
        for z in td('--zustellung', adresse, mid):
            m = re.match(r'^ZUSTELLUNG \S+ (\S+) (\d+)$', z)
            if m:
                zahl[m.group(1)] = int(m.group(2))
        return zahl

    def warte_zustellung(adresse, mid, rolle, soll, zeit=300):
        """Zahl der Nachricht in rolle (eingang: Posteingang + Spam) pollen, bis soll erreicht ist."""
        ende = time.monotonic() + zeit
        while True:
            z = zustellung(adresse, mid)
            n = z.get('eingang', 0) + z.get('spam', 0) if rolle == 'eingang' else z.get(rolle, 0)
            if n >= soll or time.monotonic() > ende:
                return n, z
            time.sleep(10)

    def genau_einmal(schritt, absender, empfaenger, mid, gesendet_soll=None):
        n, z = warte_zustellung(empfaenger, mid, 'eingang', 1)
        (ok if n == 1 else falsch)('%s: beim Empfänger %s %d× (%s)' % (schritt, empfaenger, n, z))
        if gesendet_soll is not None:
            g, zg = warte_zustellung(absender, mid, 'gesendet', gesendet_soll)
            (ok if g == gesendet_soll else falsch)('%s: Gesendet von %s %d× (%s)' % (schritt, absender, g, zg))
        time.sleep(30)
        z = zustellung(empfaenger, mid)
        n2 = z.get('eingang', 0) + z.get('spam', 0)
        (ok if n2 == 1 else falsch)('%s: 30 s später beim Empfänger %d× (%s)' % (schritt, n2, z))

    def eml(absender, empfaenger, fall):
        if absender not in TESTKONTEN or empfaenger not in TESTKONTEN:
            raise ValueError('kein Testkonto: %s → %s' % (absender, empfaenger))
        m = EmailMessage(policy=email.policy.SMTP)
        m['Subject'] = '%s Senden #362 %s %s %s' % (PREFIX, NAME, fall, time.strftime('%H:%M:%S'))
        m['From'] = absender
        m['To'] = empfaenger
        m['Date'] = email.utils.formatdate(localtime=True)
        m['Message-ID'] = email.utils.make_msgid('revolution-362', absender.split('@')[1])
        m.set_content('Testmail für #362 (tests/senden.sh, %s, %s), wird danach gelöscht.\n' % (NAME, fall))
        pfad = os.path.join(OUT, '%s-%s.eml' % (NAME, fall))
        with open(pfad, 'wb') as f:
            f.write(m.as_bytes())
        mid = m['Message-ID'].strip('<>')
        gesendet.append((absender, empfaenger, mid))
        return pfad, mid

    def senden(absender, empfaenger, fall):
        pfad, mid = eml(absender, empfaenger, fall)
        ab = len(log_zeilen())
        r = subprocess.run(['gdbus', 'call', '--session', '-d', APP_ID, '-o', '/' + APP_ID.replace('.', '/'),
                            '-m', 'org.gtk.Actions.Activate', 'test-send',
                            "[<('%s', '%s')>]" % (pfad, KONTEN[absender]['ident']), '{}'],
                           capture_output=True, text=True, timeout=30)
        (ok if r.returncode == 0 else falsch)('%s: app.test-send %s → %s (%s)' % (fall, absender, empfaenger,
                                                                                   (r.stdout + r.stderr).strip()))
        pruefe('%s: im Postausgang' % fall, r'^Postausgang: %s abgelegt' % re.escape(mid), ab, 60)
        return mid, ab

    def taste(name):
        c = d.keysym_to_keycode(XK.string_to_keysym(name))
        xtest.fake_input(d, X.KeyPress, c)
        xtest.fake_input(d, X.KeyRelease, c)
        d.sync()
        time.sleep(0.3)

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
        for _ in range(150):
            for w in d.screen().root.query_tree().children:
                try:
                    if w.get_wm_name() == 'Revolution' and w.get_attributes().map_state == X.IsViewable:
                        return w
                except Exception:
                    pass
            time.sleep(0.2)
        return None

    def bus_frei():
        for _ in range(30):
            r = subprocess.run(['gdbus', 'call', '--session', '-d', 'org.freedesktop.DBus', '-o',
                                '/org/freedesktop/DBus', '-m', 'org.freedesktop.DBus.NameHasOwner', APP_ID],
                               capture_output=True, text=True, timeout=30)
            if r.stdout.strip() == '(false,)':
                return True
            time.sleep(1)
        return False

    def start(fall, daten):
        """App mit eigenem Postausgang (XDG_DATA_HOME=daten) starten; Rückgabe Logmarke."""
        if not bus_frei():
            falsch('%s: App-ID noch auf dem Bus' % fall)
        ab = len(log_zeilen())
        log.write('==== %s: Start %s ====\n' % (NAME, fall))
        log.flush()
        env = dict(os.environ, XDG_DATA_HOME=daten)
        app[0] = subprocess.Popen([BIN], stdout=log, stderr=subprocess.STDOUT, env=env)
        w = fenster()
        if w is None:
            raise RuntimeError('kein Fenster')
        w.configure(width=1200, height=900)
        d.sync()
        pruefe('%s: Start' % fall, r'^Postausgang: bereit, \d+ Nachrichten$', ab, 60)
        return ab, w

    def beenden(fall, w):
        w.send_event(protocol.event.ClientMessage(
            window=w, client_type=d.intern_atom('WM_PROTOCOLS'),
            data=(32, [d.intern_atom('WM_DELETE_WINDOW'), X.CurrentTime, 0, 0, 0])), event_mask=X.NoEventMask)
        d.sync()
        try:
            rc = app[0].wait(60)
            (ok if rc == 0 else falsch)('%s: Beenden: Exit %d' % (fall, rc))
        except subprocess.TimeoutExpired:
            falsch('%s: Beenden: nach 60 s nicht beendet' % fall)
            app[0].kill()
            app[0].wait()
        app[0] = None

    def toeten(fall):
        app[0].send_signal(signal.SIGKILL)
        rc = app[0].wait(30)
        wert('%s: SIGKILL, Exit %d' % (fall, rc))
        app[0] = None

    def daten_ordner(fall):
        pfad = os.path.join(OUT, 'daten-%s-%s' % (NAME, fall))
        shutil.rmtree(pfad, ignore_errors=True)
        os.makedirs(pfad, mode=0o700)
        return pfad

    def verbunden(fall, ab, adressen):
        for a in adressen:
            pruefe('%s: %s verbunden' % (fall, a), r'^verbunden: %s$' % KONTEN[a]['konto'], ab, 120)

    def gesendet_ok(fall, mid, ab, transport, protokoll, kopie):
        pruefe('%s: Senden beginnt' % fall, r'^Senden: %s über %s \(%s\) beginnt$' % (
            re.escape(mid), transport, protokoll), ab, 120)
        pruefe('%s: gesendet' % fall, r'^Senden: %s gesendet nach' % re.escape(mid), ab, 120)
        pruefe('%s: Gesendet' % fall, r'^Senden: %s %s' % (re.escape(mid), kopie), ab, 120)
        pruefe('%s: aus dem Postausgang' % fall, r'^Senden: %s aus dem Postausgang entfernt$' % re.escape(mid), ab, 120)

    # ------------------------------------------------------------------ Phasen

    def phase_senden():
        fall = 'senden'
        ab, w = start(fall, daten_ordner(fall))
        verbunden(fall, ab, ('KONTO_IMAP_2@example.invalid', 'KONTO_M365@example.invalid', 'KONTO_GOOGLE@example.invalid'))
        faelle = (
            ('KONTO_IMAP_2@example.invalid', 'KONTO_IMAP_1@example.invalid', 'smtp', r'Kopie in %s/' % KONTEN['KONTO_IMAP_2@example.invalid']['konto']),
            ('KONTO_M365@example.invalid', 'KONTO_GOOGLE@example.invalid', 'microsoft365',
             r'Kopie in Gesendet legt der Server an \(microsoft365\)$'),
            ('KONTO_GOOGLE@example.invalid', 'KONTO_IMAP_2@example.invalid', 'smtp', r'ohne Gesendet-Ordner, keine Kopie$'),
        )
        mids = []
        for absender, empfaenger, protokoll, kopie in faelle:
            kurz = absender.split('@')[0]
            mid, ab_s = senden(absender, empfaenger, kurz)
            gesendet_ok(kurz, mid, ab_s, KONTEN[absender]['transport'], protokoll, kopie)
            # Sendelauf zu Ende, bevor die nächste kommt (sonst ist der Postausgang zu Recht kurz sichtbar).
            pruefe('%s: Postausgang leer' % kurz, r'^Postausgang: \d+ gesendet, 0 übrig$', ab_s, 60)
            mids.append((kurz, absender, empfaenger, mid))
        nicht('senden: kein Sendefehler', r'^Senden: \S+ fehlgeschlagen', ab)
        nicht('senden: Postausgang nie sichtbar', r'^Postausgang: \d+ Nachrichten, sichtbar$', ab)
        bild('senden')
        beenden(fall, w)
        for kurz, absender, empfaenger, mid in mids:
            genau_einmal(kurz, absender, empfaenger, mid, gesendet_soll=1)

    def phase_netz():
        absender, empfaenger = 'KONTO_IMAP_2@example.invalid', 'KONTO_IMAP_1@example.invalid'
        transport = KONTEN[absender]['transport']
        fall = 'netz'
        ab, w = start(fall, daten_ordner(fall))
        verbunden(fall, ab, (absender,))

        # (a) Server nicht erreichbar, dann Netz weg und zurück.
        kapp('sperre 465')
        mid, ab_a = senden(absender, empfaenger, 'gesperrt')
        pruefe('gesperrt: Fehler', r'^Senden: %s fehlgeschlagen: ' % re.escape(mid), ab_a, 120)
        pruefe('gesperrt: Banner', r'^Banner: Nicht gesendet, \d+ Fehler$', ab_a, 30)
        pruefe('gesperrt: Postausgang sichtbar', r'^Postausgang: 1 Nachrichten, sichtbar$', ab_a, 30)
        bild('1-gesperrt')
        time.sleep(20)
        z = zustellung(empfaenger, mid)
        (ok if z.get('eingang', 0) + z.get('spam', 0) == 0 else falsch)('gesperrt: nicht zugestellt (%s)' % z)
        ab_b = len(log_zeilen())
        netz('down')
        pruefe('gesperrt: Netz weg', r'^Netz: nicht verfügbar, offline', ab_b, 30)
        kapp('frei 465')
        time.sleep(5)
        netz('up')
        pruefe('gesperrt: Netz zurück', r'^Netz: online, ', ab_b, 60)
        pruefe('gesperrt: Sendelauf', r'^Postausgang: Sendelauf \(Netz zurück\)$', ab_b, 30)
        gesendet_ok('gesperrt', mid, ab_b, transport, 'smtp', r'Kopie in %s/' % KONTEN[absender]['konto'])
        pruefe('gesperrt: Banner weg', r'^Banner: %s verbunden, Nicht gesendet weg, 0 Fehler übrig$' % transport,
               ab_b, 30)
        pruefe('gesperrt: Postausgang ausgeblendet', r'^Postausgang: 0 Nachrichten, ausgeblendet$', ab_b, 30)
        bild('2-zurueck')
        genau_einmal('gesperrt', absender, empfaenger, mid)

        # (b) Netz weg beim Senden.
        ab_c = len(log_zeilen())
        netz('down')
        pruefe('offline: Netz weg', r'^Netz: nicht verfügbar, offline', ab_c, 30)
        mid, ab_d = senden(absender, empfaenger, 'offline')
        pruefe('offline: bleibt im Postausgang', r'^Senden: %s offline, bleibt im Postausgang$' % re.escape(mid),
               ab_d, 60)
        pruefe('offline: Postausgang sichtbar', r'^Postausgang: 1 Nachrichten, sichtbar$', ab_d, 30)
        bild('3-offline')
        ab_e = len(log_zeilen())
        netz('up')
        pruefe('offline: Netz zurück', r'^Netz: online, ', ab_e, 60)
        gesendet_ok('offline', mid, ab_e, transport, 'smtp', r'Kopie in %s/' % KONTEN[absender]['konto'])
        pruefe('offline: Postausgang ausgeblendet', r'^Postausgang: 0 Nachrichten, ausgeblendet$', ab_e, 30)
        genau_einmal('offline', absender, empfaenger, mid)

        # (c) Server nicht erreichbar, dann F5.
        verbunden('F5', ab_e, (absender,))
        kapp('sperre 465')
        mid, ab_f = senden(absender, empfaenger, 'f5')
        pruefe('F5: Fehler', r'^Senden: %s fehlgeschlagen: ' % re.escape(mid), ab_f, 120)
        pruefe('F5: Banner', r'^Banner: Nicht gesendet, \d+ Fehler$', ab_f, 30)
        kapp('frei 465')
        ab_g = len(log_zeilen())
        taste('F5')
        pruefe('F5: Sendelauf', r'^Postausgang: Sendelauf \(F5\)$', ab_g, 30)
        gesendet_ok('F5', mid, ab_g, transport, 'smtp', r'Kopie in %s/' % KONTEN[absender]['konto'])
        pruefe('F5: Banner weg', r'^Banner: %s verbunden, Nicht gesendet weg, 0 Fehler übrig$' % transport, ab_g, 30)
        beenden(fall, w)
        genau_einmal('F5', absender, empfaenger, mid)

    def phase_kill():
        absender, empfaenger = 'KONTO_IMAP_2@example.invalid', 'KONTO_IMAP_1@example.invalid'
        transport = KONTEN[absender]['transport']
        for fall, muster in (('abgelegt', r'^Postausgang: %s abgelegt'),
                             ('beginnt', r'^Senden: %s über '),
                             ('gesendet', r'^Senden: %s gesendet nach')):
            daten = daten_ordner(fall)
            ab, w = start(fall, daten)
            verbunden(fall, ab, (absender,))
            pfad, mid = eml(absender, empfaenger, fall)
            subprocess.run(['gdbus', 'call', '--session', '-d', APP_ID, '-o', '/' + APP_ID.replace('.', '/'),
                            '-m', 'org.gtk.Actions.Activate', 'test-send',
                            "[<('%s', '%s')>]" % (pfad, KONTEN[absender]['ident']), '{}'],
                           capture_output=True, text=True, timeout=30)
            treffer = warte(muster % re.escape(mid), ab, 120, takt=0.02)
            if treffer is None:
                falsch('%s: Log „%s“ fehlt, kein SIGKILL' % (fall, muster % mid))
                beenden(fall, w)
                continue
            toeten(fall)
            wert('%s: SIGKILL nach „%s“' % (fall, treffer))
            stand = [t for t in log_zeilen()[ab:] if t.startswith('Senden: %s ' % mid)]
            wert('%s: Stand vor dem Abbruch %s' % (fall, stand))

            ab2, w = start(fall + '-neustart', daten)
            rest = warte(r'^Postausgang: bereit, \d+ Nachrichten$', ab2, 30)
            vorher = int(re.search(r'(\d+) Nachrichten', rest).group(1)) if rest else -1
            wert('%s: nach Neustart %s' % (fall, rest))
            ende = warte(r'^Senden: %s (aus dem Postausgang entfernt|Zustellung unklar)' % re.escape(mid), ab2, 120)
            wert('%s: Ergebnis %s' % (fall, ende))
            n, z = warte_zustellung(empfaenger, mid, 'eingang', 1, zeit=120)
            if ende is not None and 'Zustellung unklar' in ende:
                pruefe('%s: Banner' % fall, r'^Banner: Versand unterbrochen, \d+ Fehler$', ab2, 30)
                bild('%s-unklar' % fall)
                (ok if vorher == 1 else falsch)('%s: nicht verloren, im Postausgang (%d)' % (fall, vorher))
                if n == 0:
                    ab3 = len(log_zeilen())
                    taste('F5')
                    gesendet_ok('%s F5' % fall, mid, ab3, transport, 'smtp', r'Kopie in %s/' % KONTEN[absender]['konto'])
                    pruefe('%s: Banner weg' % fall,
                           r'^Banner: %s verbunden, Versand unterbrochen weg, 0 Fehler übrig$' % transport, ab3, 30)
                else:
                    wert('%s: schon vor dem Abbruch zugestellt (%s), bleibt unklar im Postausgang, kein F5' % (fall, z))
            elif ende is not None:
                (ok if vorher == 1 or n == 1 else falsch)('%s: nicht verloren (Postausgang %d, zugestellt %d)' % (
                    fall, vorher, n))
            else:
                (ok if vorher == 0 and n == 1 else falsch)('%s: Postausgang %d, zugestellt %d' % (fall, vorher, n))
            beenden(fall + '-neustart', w)
            n2, z2 = warte_zustellung(empfaenger, mid, 'eingang', 1, zeit=120)
            (ok if n2 == 1 else falsch)('%s: beim Empfänger genau einmal (%s)' % (fall, z2))
            time.sleep(30)
            z3 = zustellung(empfaenger, mid)
            n3 = z3.get('eingang', 0) + z3.get('spam', 0)
            (ok if n3 == 1 else falsch)('%s: 30 s später beim Empfänger %d× (%s)' % (fall, n3, z3))
            g = zustellung(absender, mid).get('gesendet', 0)
            (ok if g == 1 else wert)('%s: Gesendet von %s %d×' % (fall, absender, g))

    try:
        {'senden': phase_senden, 'netz': phase_netz, 'kill': phase_kill}[PHASE]()
    finally:
        if app[0] is not None and app[0].poll() is None:
            app[0].kill()
            app[0].wait()
        log.close()
        # Nur die Testmails dieses Laufs, je Konto einmal (Empfänger: Posteingang/Spam, Absender: Gesendet).
        je_konto = {}
        for absender, empfaenger, mid in gesendet:
            for a in (absender, empfaenger):
                je_konto.setdefault(a, []).append(mid)
        for a, mids in je_konto.items():
            for z in td('--aufraeumen', a, *mids):
                if z.startswith('AUFGERAEUMT'):
                    wert('Aufräumen: %s' % z)
        for eintrag in os.listdir(OUT):
            if eintrag.startswith('daten-%s-' % NAME):
                shutil.rmtree(os.path.join(OUT, eintrag), ignore_errors=True)

    # Erwartet: nur DEBUG-Zeilen, Warnungen „Anmeldung …“ und die Marken des Treibers.
    rest = [z.strip() for z in open(LOG, encoding='utf-8', errors='replace')
            if z.strip() and not z.startswith('libEGL warning:') and 'revolution-DEBUG' not in z
            and not z.startswith('==== ')
            and not re.match(r'^\(revolution:\d+\): revolution-WARNING \*\*: \S+: Anmeldung ', z)]
    (falsch if rest else ok)('Log: %s' % ('%d unerwartete Zeilen: %s' % (len(rest), rest[:3]) if rest
                                          else 'nur DEBUG-Zeilen'))
    return 1 if fehler[0] else 0


if __name__ == '__main__':
    try:
        sys.exit(treiber())
    except Exception as e:  # #411: Abbruch zählt als FEHLER
        traceback.print_exc()
        print('FEHLER  %s: Abbruch: %s: %s' % (os.environ.get('NAME', '?'), type(e).__name__,
                                                str(e).replace('\n', ' ')[:300]), flush=True)
        sys.exit(1)
