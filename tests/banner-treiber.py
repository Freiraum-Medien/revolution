#!/usr/bin/env python3
# banner-treiber.py – Treiber für tests/banner.sh (#420): Banner „Keine Verbindung“ je Konto.
#
#   banner-treiber.py netz SOCK CTRL KAPP -- BEFEHL …
#       im Netz-Namensraum wie „netz-relay.py innen SOCK CTRL up -- BEFEHL“ (tests/netz-relay.py, Netz
#       bleibt „up“, der Netzmonitor meldet keinen Wechsel); zusätzlich über den Unix-Socket KAPP:
#         erste PORT   nur die erste neue Verbindung zu PORT (Ziel vor dem Relais) bekommt nach dem
#                      Verbindungsaufbau ein RST („Verbindung zurückgesetzt“, wie info@ in Abnahme #358);
#                      jede weitere geht durch
#         sperre PORT  jede neue Verbindung zu PORT abgewiesen (TCP RST auf SYN, „Verbindung verweigert“)
#         kappen PORT  #362: jede Verbindung zu PORT, auch bestehende, bekommt RST
#         haengen PORT #362: Pakete zu PORT verworfen (Verbindungsaufbau hängt)
#         frei PORT    Regel für PORT weg
#       nft-Tabelle „kapp<PORT>“, Kette im Ausgang vor der Umleitung des Relais (Priorität -150, nach
#       conntrack -200, vor NAT -100).
#   banner-treiber.py   (im Xvfb, ohne Capabilities) startet die App (BIN) und prüft je PHASE:
#       erste   Start, erste Verbindung von IMAP_UID zurückgesetzt, EDS verbindet beim Laden der
#               Ordnerliste neu: kein Banner „Keine Verbindung“ übrig (Fall info@ aus Abnahme #358)
#       sperre  Start mit IMAP (993) und microsoft365 (443) gesperrt: Banner „Keine Verbindung
#               (2 Konten)“; 993 frei, F5: nur der Fehler von IMAP_UID weg, Banner „Keine Verbindung“
#               (1 Konto); 443 frei, F5: Banner weg
# Konten: IMAP_UID, M365_UID; alle anderen schließt banner.sh aus (REVOLUTION_SKIP_ACCOUNTS).
# Ausgabe: „OK …“, „FEHLER …“, „WERT …“; Screenshots OUT/NAME-<Schritt>.png, Log OUT/NAME.log.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

import importlib.util
import os
import re
import socket
import subprocess
import sys
import threading
import time
import traceback

HIER = os.path.dirname(os.path.abspath(__file__))


# ------------------------------------------------------------------ Netz-Namensraum (mit Capabilities)

def nft(regeln):
    subprocess.run(['nft', '-f', '-'], input=regeln, text=True, check=True)


def kappen(kommando):
    art, port = kommando.split()
    port = int(port)
    tabelle = 'kapp%d' % port
    if art == 'frei':
        nft('delete table ip %s\n' % tabelle)
        return
    if art == 'erste':
        # quota: nur das erste SYN (60 Byte) markiert seine Verbindung; deren Pakete nach dem Aufbau
        # beantwortet nft mit RST.
        regeln = ('tcp dport %d ct state new quota until 100 bytes ct mark set 0x420\n'
                  'tcp dport %d ct mark 0x420 ct state established reject with tcp reset\n' % (port, port))
    elif art == 'sperre':
        regeln = 'tcp dport %d ct state new reject with tcp reset\n' % port
    elif art == 'kappen':
        # #362: auch bestehende Verbindungen zu PORT (jedes Paket bekommt RST).
        regeln = 'tcp dport %d reject with tcp reset\n' % port
    elif art == 'haengen':
        # #362: Pakete zu PORT verwerfen, eine neue Verbindung hängt im Aufbau.
        regeln = 'tcp dport %d drop\n' % port
    else:
        raise ValueError(art)
    nft('table ip %s {\n chain aus {\n  type filter hook output priority -150; policy accept;\n%s }\n}\n'
        % (tabelle, regeln))


def kapp_server(pfad):
    if os.path.exists(pfad):
        os.unlink(pfad)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(pfad)
    srv.listen(4)
    while True:
        c, _ = srv.accept()
        try:
            kommando = c.makefile().readline().strip()
            kappen(kommando)
            c.sendall(('ok %s\n' % kommando).encode())
        except (OSError, ValueError, subprocess.CalledProcessError) as e:
            c.sendall(('fehler %s\n' % e).encode())
        c.close()


def netz_modus(sock, ctrl, kapp, befehl):
    spec = importlib.util.spec_from_file_location('netz_relay', os.path.join(HIER, 'netz-relay.py'))
    relais = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(relais)
    threading.Thread(target=kapp_server, args=(kapp,), daemon=True).start()
    return relais.innen(sock, ctrl, 'up', befehl)


# ------------------------------------------------------------------ Treiber (im Xvfb)

def treiber():
    from PIL import Image
    from Xlib import X, XK, display, protocol
    from Xlib.ext import xtest

    BIN, OUT, NAME = os.environ['BIN'], os.environ['OUT'], os.environ['NAME']
    KAPP, PHASE = os.environ['KAPP'], os.environ['PHASE']
    IMAP_UID, M365_UID = os.environ['IMAP_UID'], os.environ['M365_UID']
    LOGKEY = os.environ['REVOLUTION_FOLDER'].replace('/', ' ', 1)
    LOG = os.path.join(OUT, NAME + '.log')
    ZEILE_RE = re.compile(r'^\(revolution:\d+\): revolution-DEBUG: \S+: (.*)$')
    d = display.Display()
    fehler = [0]

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

    def warte(muster, ab, zeit=60):
        rx = re.compile(muster)
        ende = time.monotonic() + zeit
        while time.monotonic() < ende:
            for text in log_zeilen()[ab:]:
                if rx.search(text):
                    return text
            time.sleep(0.2)
        return None

    def pruefe(schritt, muster, ab, zeit=60):
        treffer = warte(muster, ab, zeit)
        (ok if treffer else falsch)('%s: %s' % (schritt, treffer if treffer else 'Log „%s“ fehlt' % muster))
        return treffer

    def banner(ab=0):
        """Letzter Stand des Banners laut Log ab Marke: Titel oder „weg“ (None ohne Zeile)."""
        stand = None
        for text in log_zeilen()[ab:]:
            m = re.match(r'^Banner: (.*), (\d+) Fehler$', text)
            if m:
                stand = m.group(1)
            elif re.match(r'^Banner: .* weg$', text) and ' verbunden, ' not in text:
                stand = 'weg'
        return stand

    def kapp(kommando):
        c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        c.connect(KAPP)
        c.sendall(kommando.encode() + b'\n')
        antwort = c.makefile().readline().strip()
        c.close()
        (ok if antwort == 'ok ' + kommando else falsch)('Netz %s: %s' % (kommando, antwort))

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
        for _ in range(100):
            for w in d.screen().root.query_tree().children:
                try:
                    if w.get_wm_name() == 'Revolution' and w.get_attributes().map_state == X.IsViewable:
                        return w
                except Exception:
                    pass
            time.sleep(0.2)
        return None

    def stand(schritt, soll, ab=0):
        ist = banner(ab)
        (ok if ist == soll else falsch)('%s: Banner %s (erwartet %s)' % (schritt, ist, soll))

    def phase_erste():
        schritt = 'Erste Verbindung zurückgesetzt'
        pruefe(schritt, r'^Verbindung gescheitert: %s, ' % IMAP_UID, 0, 90)
        pruefe('%s: neu angemeldet' % schritt, r'^angemeldet: %s \S+ angenommen$' % IMAP_UID, 0, 60)
        pruefe('%s: Ordner vom Server' % schritt, r'^Ordner: %s \d+ geladen$' % IMAP_UID, 0, 60)
        pruefe('%s: Liste' % schritt, r'^Nachrichten: %s vollständig' % re.escape(LOGKEY), 0, 90)
        time.sleep(5)
        # Der Fehler kam nie ins Banner (Store schon verbunden) oder ist mit dem Verbinden wieder weg.
        texte = log_zeilen()
        erledigt = [t for t in texte if t == 'Verbindung gescheitert: %s, inzwischen verbunden, kein Banner' % IMAP_UID
                    or t.startswith('Banner: %s verbunden, ' % IMAP_UID)]
        (ok if erledigt else falsch)('%s: Fehler von %s nicht im Banner bzw. wieder weg (%s)' % (
            schritt, IMAP_UID[:8], erledigt or 'Log fehlt'))
        wert('%s: Banner-Zeilen %s' % (schritt, [t for t in texte if t.startswith('Banner: ')] or 'keine'))
        weg = banner()
        (ok if erledigt and weg in (None, 'weg') else falsch)('%s: kein Banner übrig (Stand %s)' % (schritt, weg))
        bild('nachher')

    def phase_sperre():
        schritt = 'Gesperrt'
        for uid in (IMAP_UID, M365_UID):
            pruefe('%s: %s' % (schritt, uid[:8]), r'^Verbindung gescheitert: %s, Ordner aus dem Cache' % uid, 0, 90)
        pruefe('%s: Liste aus dem Cache' % schritt, r'^Nachrichten: %s vollständig' % re.escape(LOGKEY), 0, 90)
        time.sleep(3)
        stand(schritt, 'Keine Verbindung (2 Konten)')
        bild('1-vorher')

        schritt = '993 frei, F5'
        kapp('frei 993')
        ab = len(log_zeilen())
        taste('F5')
        pruefe(schritt, r'^Banner: %s verbunden, Keine Verbindung weg, \d+ Fehler übrig$' % IMAP_UID, ab, 60)
        pruefe('%s: %s erneut gescheitert' % (schritt, M365_UID[:8]),
               r'^Verbindung gescheitert: %s, Ordner aus dem Cache' % M365_UID, ab, 90)
        pruefe('%s: Liste vom Server' % schritt, r'^Aktualisieren: %s fertig$' % re.escape(LOGKEY), ab, 90)
        time.sleep(2)
        stand(schritt, 'Keine Verbindung')
        weg = [t for t in log_zeilen()[ab:] if t.startswith('Banner: %s verbunden' % M365_UID)]
        (falsch if weg else ok)('%s: Fehler von %s bleibt (%s)' % (schritt, M365_UID[:8], weg or 'ok'))
        bild('2-mitte')

        schritt = '443 frei, F5'
        kapp('frei 443')
        ab = len(log_zeilen())
        taste('F5')
        pruefe(schritt, r'^Banner: %s verbunden, Keine Verbindung weg, 0 Fehler übrig$' % M365_UID, ab, 90)
        pruefe('%s: Banner' % schritt, r'^Banner: Keine Verbindung weg$', ab, 30)
        time.sleep(3)
        stand(schritt, 'weg')
        bild('3-nachher')

    if PHASE == 'erste':
        kapp('erste 993')
    else:
        kapp('sperre 993')
        kapp('sperre 443')
    log = open(LOG, 'w')
    app = subprocess.Popen([BIN], stdout=log, stderr=subprocess.STDOUT)
    try:
        w = fenster()
        if w is None:
            falsch('kein Fenster')
            return 1
        w.configure(width=1200, height=900)
        d.sync()
        (phase_erste if PHASE == 'erste' else phase_sperre)()
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
    # Erwartet: nur DEBUG-Zeilen und die Warnungen „Anmeldung …“ der gekappten Konten.
    rest = [z.strip() for z in open(LOG, encoding='utf-8', errors='replace')
            if z.strip() and not z.startswith('libEGL warning:') and 'revolution-DEBUG' not in z
            and not re.match(r'^\(revolution:\d+\): revolution-WARNING \*\*: \S+: Anmeldung ', z)]
    (falsch if rest else ok)('Log: %s' % ('%d unerwartete Zeilen: %s' % (len(rest), rest[:3]) if rest
                                          else 'nur DEBUG-Zeilen und Warnungen „Anmeldung …“'))
    return 1 if fehler[0] else 0


if __name__ == '__main__':
    if len(sys.argv) > 6 and sys.argv[1] == 'netz' and sys.argv[5] == '--':
        sys.exit(netz_modus(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[6:]))
    try:
        sys.exit(treiber())
    except Exception as e:  # #411: Abbruch zählt als FEHLER
        traceback.print_exc()
        print('FEHLER  %s: Abbruch: %s: %s' % (os.environ.get('NAME', '?'), type(e).__name__,
                                                str(e).replace('\n', ' ')[:300]), flush=True)
        sys.exit(1)
