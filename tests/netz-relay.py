#!/usr/bin/env python3
# netz-relay.py – schaltbares Netz für tests/netz.sh (#408), ohne root.
#
# Die App läuft in einem eigenen Netz-Namensraum (unshare -n --map-current-user --keep-caps); dort
# gibt es kein Netz. Dieser Helfer gibt ihr echtes Netz und nimmt es wieder weg, wie slirp4netns
# bzw. pasta (beide nicht installiert), nur einfacher: TCP und DNS laufen über einen Unix-Socket
# zu einem Gegenstück im normalen Netz, das die echten Server verbindet (keine Attrappe: TLS,
# Anmeldung und Daten gehen Ende zu Ende zwischen App und Server).
#
#   außen SOCK LOG                 im normalen Netz: nimmt Verbindungen über SOCK an, verbindet
#                                  das ursprüngliche Ziel, schreibt je Verbindung eine Zeile in LOG
#                                  und führt über SOCK Abfragen „testdaten --flags|--server|
#                                  --testordner …“ (tests/testdaten.py) im normalen Netz aus
#   innen SOCK CTRL ZUSTAND -- BEFEHL …
#                                  im Namensraum: lo, Gerät d0 (dummy), nft-Umleitung aller TCP-
#                                  Verbindungen nach außen auf 127.0.0.1:15000, DNS auf
#                                  127.0.0.53:53 (/etc/resolv.conf); startet BEFEHL ohne
#                                  Capabilities und nimmt über CTRL „up“/„down“ an.
#
# up:   Standardroute über d0, Weiterleitung an. Der Netzmonitor von GIO (netlink, liest die
#       Routen des Namensraums) meldet „network-available“ = TRUE.
# down: Standardroute weg (connect() scheitert mit ENETUNREACH, der Netzmonitor meldet FALSE),
#       alle weitergeleiteten Verbindungen hart geschlossen, DNS aus. Das entspricht einem
#       gezogenen Kabel samt Abbau der Verbindungen.
#
# Copyright 2026 Arnd Messer
#
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import socket
import struct
import subprocess
import sys
import threading
import time

SO_ORIGINAL_DST = 80
TCP_PORT = 15000


def pumpe(a, b):
    try:
        while True:
            daten = a.recv(65536)
            if not daten:
                break
            b.sendall(daten)
    except OSError:
        pass
    for s in (a, b):
        try:
            s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


def zeile(s):
    out = b''
    while not out.endswith(b'\n'):
        c = s.recv(1)
        if not c:
            raise OSError('Ende vor Kopfzeile')
        out += c
    return out.decode().strip()


def lies(s, n):
    out = b''
    while len(out) < n:
        c = s.recv(n - len(out))
        if not c:
            raise OSError('Ende vor %d Byte' % n)
        out += c
    return out


def dns_name(paket):
    teile, i = [], 12
    while i < len(paket) and paket[i] != 0:
        teile.append(paket[i + 1:i + 1 + paket[i]].decode(errors='replace'))
        i += 1 + paket[i]
    return '.'.join(teile)


# ------------------------------------------------------------------ außen

def aussen(sock_pfad, log_pfad):
    log = open(log_pfad, 'a', buffering=1)
    sperre = threading.Lock()

    def notiz(text):
        with sperre:
            log.write('%s %s\n' % (time.strftime('%H:%M:%S'), text))

    def bediene(c):
        try:
            kopf = zeile(c)
            if kopf == 'dns':
                frage = lies(c, struct.unpack('!H', lies(c, 2))[0])
                u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                u.settimeout(5)
                u.sendto(frage, ('127.0.0.53', 53))
                antwort = u.recv(65536)
                u.close()
                c.sendall(struct.pack('!H', len(antwort)) + antwort)
                notiz('DNS %s' % dns_name(frage))
                c.close()
                return
            if kopf.startswith('testdaten '):
                # #408: Serverstand unabhängig von der App im normalen Netz (auch während die App
                # offline ist): nur die Abfragen und Testkonto-Änderungen aus tests/testdaten.py.
                args = kopf.split()[1:]
                if args[0] not in ('--flags', '--server', '--testordner'):
                    raise OSError('nicht erlaubt: %s' % kopf)
                r = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'testdaten.py')]
                                   + args, capture_output=True, text=True, timeout=300)
                c.sendall((r.stdout + r.stderr).encode())
                notiz('TESTDATEN %s' % ' '.join(args))
                c.close()
                return
            _, ip, port = kopf.split()
            t = socket.create_connection((ip, int(port)), timeout=15)
            t.settimeout(None)
            notiz('TCP %s:%s' % (ip, port))
            threading.Thread(target=pumpe, args=(c, t), daemon=True).start()
            pumpe(t, c)
        except OSError as e:
            notiz('FEHLER %s' % e)
            c.close()

    if os.path.exists(sock_pfad):
        os.unlink(sock_pfad)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(sock_pfad)
    srv.listen(64)
    notiz('bereit')
    while True:
        c, _ = srv.accept()
        threading.Thread(target=bediene, args=(c,), daemon=True).start()


# ------------------------------------------------------------------ innen

class Innen:
    def __init__(self, sock_pfad):
        self.sock_pfad = sock_pfad
        self.lock = threading.Lock()
        self.offen = set()
        self.hoerer = []
        self.oben = False

    def merke(self, *socks):
        with self.lock:
            self.offen.update(socks)

    def tcp_bediene(self, c):
        try:
            roh = c.getsockopt(socket.SOL_IP, SO_ORIGINAL_DST, 16)
            port = struct.unpack('!H', roh[2:4])[0]
            ip = socket.inet_ntoa(roh[4:8])
            u = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            u.connect(self.sock_pfad)
            u.sendall(('tcp %s %d\n' % (ip, port)).encode())
            self.merke(c, u)
            threading.Thread(target=pumpe, args=(c, u), daemon=True).start()
            pumpe(u, c)
        except OSError:
            c.close()

    def tcp_hoeren(self, srv):
        while True:
            try:
                c, _ = srv.accept()
            except OSError:
                return
            threading.Thread(target=self.tcp_bediene, args=(c,), daemon=True).start()

    def dns_frage(self, srv, frage, absender):
        try:
            u = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            u.connect(self.sock_pfad)
            u.sendall(b'dns\n' + struct.pack('!H', len(frage)) + frage)
            antwort = lies(u, struct.unpack('!H', lies(u, 2))[0])
            u.close()
            srv.sendto(antwort, absender)
        except OSError:
            pass

    def dns_hoeren(self, srv):
        while True:
            try:
                frage, absender = srv.recvfrom(65536)
            except OSError:
                return
            threading.Thread(target=self.dns_frage, args=(srv, frage, absender), daemon=True).start()

    def up(self):
        if self.oben:
            return
        t = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        t.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        t.bind(('127.0.0.1', TCP_PORT))
        t.listen(64)
        d = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        d.bind(('127.0.0.53', 53))
        self.hoerer = [t, d]
        threading.Thread(target=self.tcp_hoeren, args=(t,), daemon=True).start()
        threading.Thread(target=self.dns_hoeren, args=(d,), daemon=True).start()
        subprocess.run(['ip', 'route', 'add', 'default', 'via', '10.99.0.1', 'dev', 'd0'], check=True)
        self.oben = True

    def down(self):
        if not self.oben:
            return
        subprocess.run(['ip', 'route', 'del', 'default'], check=True)
        for s in self.hoerer:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            s.close()
        self.hoerer = []
        with self.lock:
            offen, self.offen = self.offen, set()
        for s in offen:
            try:
                # RST statt FIN: SO_LINGER 0, wie eine abgerissene Verbindung.
                s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                s.close()
            except OSError:
                pass
        self.oben = False


def innen(sock_pfad, ctrl_pfad, zustand, befehl):
    for cmd in (['ip', 'link', 'set', 'lo', 'up'],
                ['ip', 'link', 'add', 'd0', 'type', 'dummy'],
                ['ip', 'addr', 'add', '10.99.0.2/24', 'dev', 'd0'],
                ['ip', 'link', 'set', 'd0', 'up'],
                ['nft', 'add', 'table', 'ip', 'relay'],
                ['nft', 'add chain ip relay out { type nat hook output priority -100; }'],
                ['nft', 'add rule ip relay out ip daddr != 127.0.0.0/8 meta l4proto tcp redirect to :%d' % TCP_PORT]):
        subprocess.run(cmd, check=True)
    netz = Innen(sock_pfad)
    if zustand == 'up':
        netz.up()

    if os.path.exists(ctrl_pfad):
        os.unlink(ctrl_pfad)
    ctrl = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    ctrl.bind(ctrl_pfad)
    ctrl.listen(4)

    def steuern():
        while True:
            c, _ = ctrl.accept()
            try:
                kommando = zeile(c)
                if kommando == 'up':
                    netz.up()
                elif kommando == 'down':
                    netz.down()
                c.sendall(b'ok up\n' if netz.oben else b'ok down\n')
            except (OSError, subprocess.CalledProcessError) as e:
                c.sendall(('fehler %s\n' % e).encode())
            c.close()

    threading.Thread(target=steuern, daemon=True).start()
    # Die App ohne Capabilities (nur dieser Helfer braucht CAP_NET_ADMIN im Namensraum).
    kind = subprocess.run(['setpriv', '--inh-caps=-all', '--ambient-caps=-all', '--'] + befehl)
    return kind.returncode


def testdaten(sock_pfad, *args):
    """Für den Treiber: tests/testdaten.py im normalen Netz über außen (SOCK) ausführen."""
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.connect(sock_pfad)
    c.sendall(('testdaten %s\n' % ' '.join(args)).encode())
    out = b''
    while True:
        teil = c.recv(65536)
        if not teil:
            break
        out += teil
    c.close()
    return out.decode()


def steuere(ctrl_pfad, kommando):
    """Für den Treiber: „up“/„down“ an innen schicken, Antwort zurück."""
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.connect(ctrl_pfad)
    c.sendall(kommando.encode() + b'\n')
    antwort = zeile(c)
    c.close()
    return antwort


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'außen':
        aussen(sys.argv[2], sys.argv[3])
    elif len(sys.argv) > 6 and sys.argv[1] == 'innen' and sys.argv[4] in ('up', 'down') and sys.argv[5] == '--':
        sys.exit(innen(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[6:]))
    elif len(sys.argv) >= 4 and sys.argv[1] == 'testdaten':
        print(testdaten(sys.argv[2], *sys.argv[3:]), end='')
    elif len(sys.argv) == 4 and sys.argv[1] == 'steuern':
        print(steuere(sys.argv[2], sys.argv[3]))
    else:
        sys.exit('Aufruf: netz-relay.py außen SOCK LOG | innen SOCK CTRL up|down -- BEFEHL … | steuern CTRL up|down'
                 ' | testdaten SOCK ARGUMENTE …')
